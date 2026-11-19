#pragma once

#include "teal_client.h"

#include <SDK/foobar2000.h>

#include <string>

namespace track_metadata
{

std::string trim(std::string value);
std::string os_version();
std::string architecture();
bool same_stream_title(const teal::PlayView &left, const teal::PlayView &right);
double threshold_for(double duration);
std::string format_field(metadb_handle_ptr track, const char *pattern);
std::string make_client_agent();

} // namespace track_metadata
