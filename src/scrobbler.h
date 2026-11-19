#pragma once

#include "play_queue.h"
#include "teal_client.h"
#include "upload_policy.h"

#include <SDK/foobar2000.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class scrobbler : public play_callback_static
{
  public:
    static scrobbler *instance();

    scrobbler();
    ~scrobbler();

    unsigned get_flags() override;

    void on_playback_starting(play_control::t_track_command command, bool paused) override;
    void on_playback_new_track(metadb_handle_ptr track) override;
    void on_playback_stop(play_control::t_stop_reason reason) override;
    void on_playback_seek(double time) override;
    void on_playback_pause(bool state) override;
    void on_playback_edited(metadb_handle_ptr track) override;
    void on_playback_dynamic_info(const file_info &info) override;
    void on_playback_dynamic_info_track(const file_info &info) override;
    void on_playback_time(double time) override;
    void on_volume_change(float new_value) override;

    void start();
    void shutdown();

    void settings_changed();
    void submit_now();
    std::size_t pending_count() const;

  private:
    struct status_job
    {
        bool clear = false;
        teal::StatusRecord status;
    };

    struct track_state
    {
        bool active = false;
        bool submitted = false;
        metadb_handle_ptr handle;
        teal::PlayView view;
        double duration = 0.0;
        double threshold = 0.0;
        double elapsed = 0.0;
        double last_time = -1.0;
        std::string played_time;
    };

    void begin_track(metadb_handle_ptr track);
    void reset_track();
    void finish_track(play_control::t_stop_reason reason);
    void read_settings();
    bool build_view(teal::PlayView &view) const;
    void publish_status();
    void clear_status();
    void maybe_submit();
    void enqueue_play();
    void worker_main();
    void on_play_result(std::string rkey, upload_policy::play_outcome outcome, std::string message);
    void save_queue_locked();
    void load_queue();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool started_ = false;
    bool stopping_ = false;
    std::unique_ptr<abort_callback_impl> abort_;

    std::vector<play_queue::queue_item> queue_;
    std::optional<status_job> status_job_;
    bool enabled_ = true;
    bool media_library_only_ = false;
    bool dynamic_sources_ = true;
    std::string handle_;
    std::string password_;
    std::string client_agent_;
    std::uint64_t credentials_generation_ = 0;

    track_state current_;
};
