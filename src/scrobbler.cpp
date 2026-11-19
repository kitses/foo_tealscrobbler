#include "scrobbler.h"

#include "config.h"
#include "foobar_http.h"
#include "play_queue.h"
#include "track_metadata.h"
#include "upload_policy.h"

#include <SDK/foobar2000.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{

scrobbler *g_instance = nullptr;

void log_line(const std::string &text)
{
    FB2K_console_formatter() << "teal.fm: " << text.c_str();
}

} // namespace

scrobbler *scrobbler::instance()
{
    return g_instance;
}

scrobbler::scrobbler()
{
    client_agent_ = track_metadata::make_client_agent();
    g_instance = this;
}

scrobbler::~scrobbler()
{
    shutdown();
    if (g_instance == this)
    {
        g_instance = nullptr;
    }
}

void scrobbler::read_settings()
{
    const std::string new_password = get_app_password().value_or(std::string());
    const std::string new_handle = cfg_handle.get().get_ptr();
    std::lock_guard lock(mutex_);
    enabled_ = cfg_enabled.get();
    media_library_only_ = cfg_media_library_only.get();
    dynamic_sources_ = cfg_dynamic_sources.get();
    if (handle_ != new_handle || password_ != new_password)
    {
        handle_ = new_handle;
        password_ = new_password;
        credentials_generation_++;
    }
}

unsigned scrobbler::get_flags()
{
    return flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_seek | flag_on_playback_pause |
           flag_on_playback_time | flag_on_playback_edited | flag_on_playback_dynamic_info_track;
}

void scrobbler::start()
{
    read_settings();
    load_queue();
    std::lock_guard lock(mutex_);
    if (started_)
    {
        return;
    }
    stopping_ = false;
    abort_ = std::make_unique<abort_callback_impl>();
    started_ = true;
    worker_ = std::thread([this] { worker_main(); });
}

void scrobbler::shutdown()
{
    {
        std::lock_guard lock(mutex_);
        if (!started_)
        {
            return;
        }
        stopping_ = true;
        if (abort_)
        {
            abort_->set_state(true);
        }
        save_queue_locked();
    }
    condition_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
    {
        std::lock_guard lock(mutex_);
        started_ = false;
        abort_.reset();
    }
}

bool scrobbler::build_view(teal::PlayView &view) const
{
    if (current_.handle.is_empty())
    {
        return false;
    }
    const std::string artist =
        track_metadata::trim(track_metadata::format_field(current_.handle, cfg_artist.get().get_ptr()));
    const std::string title =
        track_metadata::trim(track_metadata::format_field(current_.handle, cfg_title.get().get_ptr()));
    if (artist.empty() || title.empty())
    {
        return false;
    }
    view = {};
    view.trackName = title;
    view.releaseName = track_metadata::trim(track_metadata::format_field(current_.handle, cfg_album.get().get_ptr()));
    view.recordingMbId =
        teal::normalizeMbid(track_metadata::format_field(current_.handle, cfg_mb_track.get().get_ptr()));
    view.isrc = track_metadata::trim(track_metadata::format_field(current_.handle, "[%isrc%]"));
    view.musicServiceUri = "https://foobar2000.org";
    view.submissionClientAgent = client_agent_;
    view.playedTime = current_.played_time;
    if (current_.duration > 0.0)
    {
        view.durationSeconds = static_cast<std::int64_t>(std::llround(current_.duration));
    }
    std::size_t start = 0;
    while (start <= artist.size())
    {
        const std::size_t semicolon = artist.find(';', start);
        const std::size_t end = semicolon == std::string::npos ? artist.size() : semicolon;
        std::string name = track_metadata::trim(artist.substr(start, end - start));
        if (!name.empty())
        {
            view.artists.push_back({std::move(name), {}});
        }
        if (semicolon == std::string::npos)
        {
            break;
        }
        start = semicolon + 1;
    }
    return !view.artists.empty();
}

void scrobbler::publish_status()
{
    {
        std::lock_guard lock(mutex_);
        if (!enabled_ || handle_.empty() || password_.empty())
        {
            return;
        }
    }
    if (!current_.active || playback_control::get()->is_paused())
    {
        return;
    }
    teal::PlayView view;
    if (!build_view(view))
    {
        return;
    }
    teal::StatusRecord status;
    status.item = std::move(view);
    status.time = teal::utcNowIso();
    std::int64_t lifetime = 600;
    if (current_.duration > 0.0)
    {
        const double position = std::max(0.0, playback_control::get()->playback_get_position());
        const double remaining = std::max(0.0, current_.duration - position);
        lifetime = static_cast<std::int64_t>(std::min(remaining + 60.0, 3.0 * 60.0 * 60.0));
    }
    status.expiry = teal::utcIsoFromEpochSeconds(teal::unixNowSeconds() + lifetime);
    {
        std::lock_guard lock(mutex_);
        status_job_ = status_job{false, std::move(status)};
    }
    condition_.notify_all();
}

void scrobbler::clear_status()
{
    {
        std::lock_guard lock(mutex_);
        if (handle_.empty() || password_.empty())
        {
            return;
        }
        status_job_ = status_job{true, {}};
    }
    condition_.notify_all();
}

void scrobbler::begin_track(metadb_handle_ptr track)
{
    const bool was_published = current_.active;
    reset_track();
    if (track.is_empty())
    {
        return;
    }
    bool enabled = false;
    bool media_library_only = false;
    bool dynamic_sources = false;
    {
        std::lock_guard lock(mutex_);
        enabled = enabled_;
        media_library_only = media_library_only_;
        dynamic_sources = dynamic_sources_;
    }
    if (!enabled)
    {
        if (was_published)
        {
            clear_status();
        }
        return;
    }
    if (media_library_only && !library_manager::get()->is_item_in_library(track))
    {
        log_line("Track is not in Media Library. Not submitting to teal.fm.");
        if (was_published)
        {
            clear_status();
        }
        return;
    }
    const char *path = track->get_path();
    if (!dynamic_sources && filesystem::g_is_remote_or_unrecognized(path))
    {
        log_line("Skipping track from a dynamic source.");
        if (was_published)
        {
            clear_status();
        }
        return;
    }

    current_.handle = track;
    current_.duration = track->get_length();
    current_.played_time = teal::utcNowIso();
    if (!build_view(current_.view))
    {
        log_line("Skipping track due to missing artist or title info.");
        reset_track();
        if (was_published)
        {
            clear_status();
        }
        return;
    }
    current_.active = true;
    current_.submitted = false;
    current_.threshold = track_metadata::threshold_for(current_.duration);
    current_.elapsed = 0.0;
    current_.last_time = std::max(0.0, playback_control::get()->playback_get_position());
    publish_status();
}

void scrobbler::reset_track()
{
    current_ = track_state{};
}

void scrobbler::maybe_submit()
{
    if (!current_.active || current_.submitted)
    {
        return;
    }
    if (current_.elapsed + 0.001 < current_.threshold)
    {
        return;
    }
    current_.submitted = true;
    enqueue_play();
}

void scrobbler::enqueue_play()
{
    if (!current_.active)
    {
        return;
    }
    teal::PlayRecord play;
    play.view = current_.view;
    play.rkey = teal::makeTid();
    play_queue::queue_item item;
    item.rkey = play.rkey;
    item.record_json = teal::buildPlayRecordJson(play);
    item.attempts = 0;
    item.next_attempt = 0;
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(item));
        while (queue_.size() > 2000)
        {
            queue_.erase(queue_.begin());
        }
        save_queue_locked();
    }
    condition_.notify_all();
}

void scrobbler::finish_track(play_control::t_stop_reason reason)
{
    if (current_.active)
    {
        if (reason == play_control::stop_reason_eof && current_.duration > 0.0 && current_.last_time >= 0.0 &&
            current_.duration > current_.last_time && current_.duration - current_.last_time <= 5.0)
        {
            current_.elapsed += current_.duration - current_.last_time;
            current_.last_time = current_.duration;
        }
        maybe_submit();
        if (reason != play_control::stop_reason_starting_another && reason != play_control::stop_reason_shutting_down)
        {
            clear_status();
        }
    }
    reset_track();
}

void scrobbler::on_playback_starting(play_control::t_track_command command, bool paused)
{
    (void)command;
    (void)paused;
}

void scrobbler::on_playback_new_track(metadb_handle_ptr track)
{
    if (current_.active)
    {
        maybe_submit();
    }
    begin_track(track);
}

void scrobbler::on_playback_stop(play_control::t_stop_reason reason)
{
    finish_track(reason);
}

void scrobbler::on_playback_seek(double time)
{
    if (current_.active)
    {
        current_.last_time = time;
    }
}

void scrobbler::on_playback_pause(bool state)
{
    if (!current_.active)
    {
        return;
    }
    const double position = playback_control::get()->playback_get_position();
    if (position >= 0.0)
    {
        if (state && position > current_.last_time && position - current_.last_time <= 5.0)
        {
            current_.elapsed += position - current_.last_time;
        }
        current_.last_time = position;
    }
    if (state)
    {
        clear_status();
        return;
    }
    publish_status();
}

void scrobbler::on_playback_edited(metadb_handle_ptr track)
{
    if (!current_.active || current_.handle.get_ptr() != track.get_ptr())
    {
        return;
    }
    teal::PlayView view;
    if (!build_view(view))
    {
        reset_track();
        clear_status();
        return;
    }
    current_.view = std::move(view);
    current_.duration = track->get_length();
    current_.threshold = track_metadata::threshold_for(current_.duration);
    maybe_submit();
    publish_status();
}

void scrobbler::on_playback_dynamic_info(const file_info &info)
{
    (void)info;
}

void scrobbler::on_playback_dynamic_info_track(const file_info &info)
{
    (void)info;
    if (!current_.active)
    {
        return;
    }
    teal::PlayView view;
    if (!build_view(view) || track_metadata::same_stream_title(view, current_.view))
    {
        return;
    }
    maybe_submit();
    const std::string played_time = teal::utcNowIso();
    view.playedTime = played_time;
    current_.view = std::move(view);
    current_.played_time = played_time;
    current_.elapsed = 0.0;
    current_.submitted = false;
    current_.threshold = track_metadata::threshold_for(current_.duration);
    publish_status();
}

void scrobbler::on_playback_time(double time)
{
    if (!current_.active || time < 0.0 || playback_control::get()->is_paused())
    {
        return;
    }
    if (current_.last_time >= 0.0 && time >= current_.last_time && time - current_.last_time <= 5.0)
    {
        current_.elapsed += time - current_.last_time;
    }
    current_.last_time = time;
    maybe_submit();
}

void scrobbler::on_volume_change(float new_value)
{
    (void)new_value;
}

void scrobbler::settings_changed()
{
    const std::string new_password = get_app_password().value_or(std::string());
    const std::string new_handle = cfg_handle.get().get_ptr();
    bool enabled = false;
    bool credentials_changed = false;
    {
        std::lock_guard lock(mutex_);
        enabled = cfg_enabled.get();
        media_library_only_ = cfg_media_library_only.get();
        dynamic_sources_ = cfg_dynamic_sources.get();
        if (handle_ != new_handle || password_ != new_password)
        {
            handle_ = new_handle;
            password_ = new_password;
            credentials_generation_++;
            credentials_changed = true;
        }
        enabled_ = enabled;
    }
    if (!enabled)
    {
        reset_track();
        clear_status();
    }
    else if (credentials_changed)
    {
        metadb_handle_ptr track;
        if (playback_control::get()->get_now_playing(track) && playback_control::get()->is_playing() &&
            !current_.active)
        {
            begin_track(track);
        }
        else
        {
            publish_status();
        }
    }
    else
    {
        publish_status();
    }
    condition_.notify_all();
}

void scrobbler::submit_now()
{
    std::size_t count = 0;
    {
        std::lock_guard lock(mutex_);
        if (!enabled_)
        {
            FB2K_console_formatter() << "teal.fm: Cached tracks are not submitted while disabled.\n";
            return;
        }
        count = queue_.size();
        if (count == 0)
        {
            FB2K_console_formatter() << "teal.fm: Cache is empty. Nothing to submit.\n";
            return;
        }
        for (auto &item : queue_)
        {
            item.attempts = 0;
            item.next_attempt = 0;
        }
        save_queue_locked();
    }
    condition_.notify_all();
    FB2K_console_formatter() << "teal.fm: Submitting " << count << " tracks.\n";
}

std::size_t scrobbler::pending_count() const
{
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void scrobbler::load_queue()
{
    queue_ = play_queue::from_json(cfg_queue.get().get_ptr(), teal::unixNowSeconds());
}

void scrobbler::save_queue_locked()
{
    cfg_queue = play_queue::to_json(queue_, log_line).c_str();
}

void scrobbler::on_play_result(std::string rkey, upload_policy::play_outcome outcome, std::string message)
{
    std::string log_text;
    {
        std::lock_guard lock(mutex_);
        const auto it = std::find_if(queue_.begin(), queue_.end(),
                                     [&](const play_queue::queue_item &item) { return item.rkey == rkey; });
        if (it == queue_.end())
        {
            return;
        }
        if (outcome == upload_policy::play_outcome::success)
        {
            queue_.erase(it);
            save_queue_locked();
            log_text = "Submission succeeded.";
        }
        else if (outcome == upload_policy::play_outcome::drop)
        {
            queue_.erase(it);
            save_queue_locked();
            log_text = "Submission failed: " + message;
        }
        else
        {
            it->attempts += 1;
            const int delay = play_queue::retry_delay_seconds(it->attempts);
            it->next_attempt = teal::unixNowSeconds() + delay;
            save_queue_locked();
            log_text = "Submission failed: " + message + " (retrying in " + std::to_string(delay) + " seconds)";
        }
    }
    condition_.notify_all();
    log_line(log_text);
}

void scrobbler::worker_main()
{
    FoobarHttpTransport transport(*abort_);
    teal::Client client(transport, "", "", client_agent_);
    std::uint64_t seen_generation = std::numeric_limits<std::uint64_t>::max();
    std::unordered_map<std::string, std::int64_t> recently_sent;

    for (;;)
    {
        status_job job;
        play_queue::queue_item item;
        bool has_job = false;
        bool has_item = false;
        bool enabled = false;
        bool has_credentials = false;
        {
            std::unique_lock lock(mutex_);
            const auto has_selectable_play = [&] {
                const std::int64_t now = teal::unixNowSeconds();
                for (const auto &candidate : queue_)
                {
                    if (candidate.next_attempt > now)
                    {
                        continue;
                    }
                    const auto sent = recently_sent.find(candidate.rkey);
                    if (sent == recently_sent.end() || now - sent->second >= 5)
                    {
                        return true;
                    }
                }
                return false;
            };
            const auto credentials_usable = [&] {
                return !handle_.empty() && !password_.empty() && !client.authFailed();
            };
            condition_.wait_for(lock, std::chrono::seconds(1), [&] {
                if (stopping_)
                    return true;
                const bool usable = credentials_usable();
                if (status_job_.has_value() && usable)
                    return true;
                if (!usable || !enabled_)
                    return false;
                return has_selectable_play();
            });
            if (stopping_)
            {
                break;
            }
            if (credentials_generation_ != seen_generation)
            {
                client.setCredentials(handle_, password_);
                seen_generation = credentials_generation_;
            }
            enabled = enabled_;
            has_credentials = credentials_usable();
            if (has_credentials && status_job_.has_value())
            {
                job = std::move(*status_job_);
                status_job_.reset();
                has_job = true;
            }
            else if (has_credentials && enabled)
            {
                const std::int64_t now = teal::unixNowSeconds();
                for (auto &candidate : queue_)
                {
                    if (candidate.next_attempt > now)
                    {
                        continue;
                    }
                    const auto sent = recently_sent.find(candidate.rkey);
                    if (sent != recently_sent.end() && now - sent->second < 5)
                    {
                        continue;
                    }
                    item = candidate;
                    recently_sent[candidate.rkey] = now;
                    has_item = true;
                    break;
                }
                for (auto it = recently_sent.begin(); it != recently_sent.end();)
                {
                    if (now - it->second > 60)
                    {
                        it = recently_sent.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
        }

        if (has_job)
        {
            if (job.clear)
            {
                try
                {
                    client.clearStatus();
                }
                catch (const std::exception &error)
                {
                    log_line(std::string("Now playing notification failed: ") + error.what());
                }
            }
            else if (enabled)
            {
                try
                {
                    client.submitStatus(job.status);
                }
                catch (const std::exception &error)
                {
                    log_line(std::string("Now playing notification failed: ") + error.what());
                }
            }
            continue;
        }

        if (!has_item)
        {
            continue;
        }

        log_line("Submitting track...");
        try
        {
            client.submitPlayRecord(item.rkey, item.record_json);
            fb2k::inMainThread(
                [this, rkey = item.rkey] { on_play_result(rkey, upload_policy::play_outcome::success, {}); });
        }
        catch (const teal::ApiError &error)
        {
            // A vanished account is not the record's fault, so hold the queue instead of dropping it.
            const upload_policy::play_outcome outcome =
                upload_policy::classify_failure(error.retryable(), error.code());
            std::string message = error.code().empty() ? error.what() : error.code() + ": " + error.what();
            fb2k::inMainThread([this, rkey = item.rkey, outcome, message = std::move(message)] {
                on_play_result(rkey, outcome, message);
            });
        }
        catch (const std::exception &error)
        {
            std::string message = error.what();
            fb2k::inMainThread([this, rkey = item.rkey, message = std::move(message)] {
                on_play_result(rkey, upload_policy::play_outcome::retry, message);
            });
        }
    }
}

play_callback_static_factory_t<scrobbler> g_scrobbler_factory;
