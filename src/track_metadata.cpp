#include "track_metadata.h"

#include "version.h"

#include <algorithm>
#include <cctype>

namespace track_metadata
{

std::string trim(std::string value)
{
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto first = std::find_if(value.begin(), value.end(), not_space);
    const auto last = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (first >= last)
    {
        return {};
    }
    return std::string(first, last);
}

std::string os_version()
{
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (module != nullptr)
    {
        const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(module, "RtlGetVersion"));
        if (fn != nullptr)
        {
            OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (fn(&info) == 0)
            {
                return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
                       std::to_string(info.dwBuildNumber);
            }
        }
    }
    return "unknown";
}

std::string architecture()
{
#ifdef _WIN64
    return "x86_64";
#else
    return "x86";
#endif
}

bool same_stream_title(const teal::PlayView &left, const teal::PlayView &right)
{
    if (left.trackName != right.trackName || left.artists.size() != right.artists.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < left.artists.size(); ++i)
    {
        if (left.artists[i].name != right.artists[i].name)
        {
            return false;
        }
    }
    return true;
}

double threshold_for(double duration)
{
    if (!(duration > 0.0))
    {
        return 30.0;
    }
    if (duration < 120.0)
    {
        return duration;
    }
    return std::min(duration / 2.0, 240.0);
}

std::string format_field(metadb_handle_ptr track, const char *pattern)
{
    if (pattern == nullptr || *pattern == '\0')
    {
        return {};
    }
    titleformat_object::ptr script;
    if (!titleformat_compiler::get()->compile(script, pattern))
    {
        return {};
    }
    pfc::string8 out;
    playback_control::get()->playback_format_title_ex(track, nullptr, out, script, nullptr,
                                                      playback_control::display_level_all);
    return out.get_ptr();
}

std::string make_client_agent()
{
    return std::string("fm.tealscrobbler/") + TEAL_VERSION + " (Windows; " + os_version() + "; " + architecture() + ")";
}

} // namespace track_metadata
