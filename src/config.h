#pragma once

#include <SDK/foobar2000.h>

#include <optional>
#include <string>
#include <string_view>

extern cfg_bool cfg_enabled;
extern cfg_string cfg_handle;
extern cfg_string cfg_password;
extern cfg_bool cfg_media_library_only;
extern cfg_bool cfg_dynamic_sources;
extern cfg_string cfg_artist;
extern cfg_string cfg_album;
extern cfg_string cfg_title;
extern cfg_string cfg_mb_track;
extern cfg_string cfg_queue;

std::optional<std::string> get_app_password();
void set_app_password(std::string_view value);
std::string protect_secret(std::string_view value);
std::optional<std::string> unprotect_secret(std::string_view value);
