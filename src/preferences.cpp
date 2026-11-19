#include "config.h"
#include "resource.h"
#include "scrobbler.h"

#include <SDK/coreDarkMode.h>
#include <SDK/foobar2000.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace
{

constexpr GUID guid_preferences_page = {0x504dec1d, 0x9a53, 0x4ad7, {0x8c, 0x0a, 0x50, 0x75, 0xd9, 0x15, 0x83, 0xc6}};

class teal_preferences_instance : public preferences_page_instance
{
  public:
    teal_preferences_instance(fb2k::hwnd_t parent, preferences_page_callback::ptr callback)
        : callback_(std::move(callback))
    {
        hwnd_ = CreateDialogParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_TEAL_PREFERENCES), parent,
                                   dialog_proc, reinterpret_cast<LPARAM>(this));
        if (hwnd_ == nullptr)
        {
            throw std::runtime_error("Could not create the teal.fm preferences dialog");
        }
    }

    ~teal_preferences_instance()
    {
        if (hwnd_ != nullptr && IsWindow(hwnd_))
        {
            DestroyWindow(hwnd_);
        }
    }

    t_uint32 get_state() override
    {
        t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
        if (has_changed())
        {
            state |= preferences_state::changed;
        }
        return state;
    }

    fb2k::hwnd_t get_wnd() override
    {
        return hwnd_;
    }

    void apply() override
    {
        cfg_enabled = get_check(IDC_ENABLED);
        cfg_handle = get_text(IDC_HANDLE).c_str();
        const std::string password = get_text(IDC_PASSWORD);
        const auto stored_password = get_app_password();
        if (!stored_password.has_value() && password.empty())
        {
            FB2K_console_formatter()
                << "teal.fm: the stored app password could not be decrypted; leaving it in place.\n";
        }
        else if (password != stored_password.value_or(std::string()))
        {
            set_app_password(password);
        }
        cfg_media_library_only = get_check(IDC_MEDIA_LIBRARY_ONLY);
        cfg_dynamic_sources = get_check(IDC_DYNAMIC_SOURCES);
        cfg_artist = get_text(IDC_ARTIST).c_str();
        cfg_album = get_text(IDC_ALBUM).c_str();
        cfg_title = get_text(IDC_TITLE).c_str();
        cfg_mb_track = get_text(IDC_MB_TRACK).c_str();
        if (scrobbler *app = scrobbler::instance())
        {
            app->settings_changed();
        }
        if (callback_.is_valid())
        {
            callback_->on_state_changed();
        }
        update_status();
    }

    void reset() override
    {
        set_check(IDC_ENABLED, true);
        set_text(IDC_HANDLE, "");
        set_text(IDC_PASSWORD, "");
        set_check(IDC_MEDIA_LIBRARY_ONLY, false);
        set_check(IDC_DYNAMIC_SOURCES, true);
        set_text(IDC_ARTIST, "[%artist%]");
        set_text(IDC_ALBUM, "[%album%]");
        set_text(IDC_TITLE, "[%title%]");
        set_text(IDC_MB_TRACK, "[%musicbrainz_trackid%]");
        notify_changed();
    }

  private:
    static INT_PTR CALLBACK dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto *self = reinterpret_cast<teal_preferences_instance *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_INITDIALOG)
        {
            self = reinterpret_cast<teal_preferences_instance *>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr)
        {
            return self->handle_message(hwnd, message, wparam);
        }
        return FALSE;
    }

    INT_PTR handle_message(HWND hwnd, UINT message, WPARAM wparam)
    {
        switch (message)
        {
        case WM_INITDIALOG:
            hwnd_ = hwnd;
            on_init();
            return FALSE;
        case WM_COMMAND: {
            const WORD id = LOWORD(wparam);
            const WORD code = HIWORD(wparam);
            if (id == IDC_SUBMIT && code == BN_CLICKED)
            {
                submit();
                return TRUE;
            }
            if (code == EN_CHANGE || code == BN_CLICKED)
            {
                notify_changed();
            }
            return FALSE;
        }
        case WM_TIMER:
            update_status();
            return TRUE;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            return FALSE;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            return FALSE;
        default:
            return FALSE;
        }
    }

    void on_init()
    {
        dark_.AddDialogWithControls(hwnd_);
        set_check(IDC_ENABLED, cfg_enabled.get());
        set_text(IDC_HANDLE, cfg_handle.get().get_ptr());
        const auto stored_password = get_app_password();
        if (stored_password.has_value())
        {
            set_text(IDC_PASSWORD, *stored_password);
        }
        else
        {
            set_text(IDC_PASSWORD, "");
            FB2K_console_formatter()
                << "teal.fm: the stored app password could not be decrypted; please re-enter it.\n";
        }
        set_check(IDC_MEDIA_LIBRARY_ONLY, cfg_media_library_only.get());
        set_check(IDC_DYNAMIC_SOURCES, cfg_dynamic_sources.get());
        set_text(IDC_ARTIST, cfg_artist.get().get_ptr());
        set_text(IDC_ALBUM, cfg_album.get().get_ptr());
        set_text(IDC_TITLE, cfg_title.get().get_ptr());
        set_text(IDC_MB_TRACK, cfg_mb_track.get().get_ptr());
        update_status();
        SetTimer(hwnd_, 1, 1000, nullptr);
    }

    void submit()
    {
        submit_hold_until_ = GetTickCount64() + 2000;
        if (scrobbler *app = scrobbler::instance())
        {
            app->submit_now();
        }
        update_status();
    }

    void update_status()
    {
        if (hwnd_ == nullptr)
        {
            return;
        }
        const std::size_t count = scrobbler::instance() != nullptr ? scrobbler::instance()->pending_count() : 0;
        const std::string text =
            "Your cache contains " + std::to_string(count) + " tracks. Check the console for status information.";
        set_text(IDC_STATUS, text);
        bool enabled = count != 0;
        if (submit_hold_until_ != 0)
        {
            if (GetTickCount64() < submit_hold_until_)
            {
                enabled = false;
            }
            else
            {
                submit_hold_until_ = 0;
            }
        }
        EnableWindow(GetDlgItem(hwnd_, IDC_SUBMIT), enabled);
    }

    void notify_changed()
    {
        if (callback_.is_valid())
        {
            callback_->on_state_changed();
        }
    }

    bool password_changed() const
    {
        const std::string text = get_text(IDC_PASSWORD);
        const auto stored = get_app_password();
        if (!stored.has_value())
        {
            return !text.empty();
        }
        return text != *stored;
    }

    bool has_changed()
    {
        return get_check(IDC_ENABLED) != cfg_enabled.get() || get_text(IDC_HANDLE) != cfg_handle.get().get_ptr() ||
               password_changed() || get_check(IDC_MEDIA_LIBRARY_ONLY) != cfg_media_library_only.get() ||
               get_check(IDC_DYNAMIC_SOURCES) != cfg_dynamic_sources.get() ||
               get_text(IDC_ARTIST) != cfg_artist.get().get_ptr() || get_text(IDC_ALBUM) != cfg_album.get().get_ptr() ||
               get_text(IDC_TITLE) != cfg_title.get().get_ptr() ||
               get_text(IDC_MB_TRACK) != cfg_mb_track.get().get_ptr();
    }

    std::string get_text(int id) const
    {
        if (hwnd_ == nullptr)
        {
            return {};
        }
        pfc::string8 value;
        uGetDlgItemText(hwnd_, static_cast<UINT>(id), value);
        return value.get_ptr();
    }

    void set_text(int id, const std::string &value)
    {
        if (hwnd_ != nullptr)
        {
            uSetWindowText(GetDlgItem(hwnd_, id), value.c_str());
        }
    }

    bool get_check(int id) const
    {
        return hwnd_ != nullptr && SendDlgItemMessageW(hwnd_, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    void set_check(int id, bool checked)
    {
        if (hwnd_ != nullptr)
        {
            SendDlgItemMessageW(hwnd_, id, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    preferences_page_callback::ptr callback_;
    HWND hwnd_ = nullptr;
    fb2k::CCoreDarkModeHooks dark_;
    ULONGLONG submit_hold_until_ = 0;
};

class teal_preferences_page : public preferences_page_v3
{
  public:
    const char *get_name() override
    {
        return "teal.fm";
    }

    GUID get_guid() override
    {
        return guid_preferences_page;
    }

    GUID get_parent_guid() override
    {
        return guid_tools;
    }

    preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override
    {
        return fb2k::service_new<teal_preferences_instance>(parent, callback);
    }
};

} // namespace

static preferences_page_factory_t<teal_preferences_page> g_teal_preferences_page;
