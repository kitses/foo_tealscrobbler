#include "teal_client.h"

#include <atomic>
#include <chrono>
#include <ctime>

namespace teal
{
namespace
{

constexpr char kTidAlphabet[] = "234567abcdefghijklmnopqrstuvwxyz";

} // namespace

std::string utcIsoFromEpochSeconds(std::int64_t seconds)
{
    const std::time_t value = static_cast<std::time_t>(seconds);
    std::tm tm{};
    gmtime_s(&tm, &value);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::string utcNowIso()
{
    return utcIsoFromEpochSeconds(unixNowSeconds());
}

std::int64_t unixNowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string makeTid(std::int64_t unixMicros, std::uint16_t clockId)
{
    std::uint64_t value = ((static_cast<std::uint64_t>(unixMicros) & ((std::uint64_t{1} << 53) - 1)) << 10) |
                          (static_cast<std::uint64_t>(clockId) & 0x3FF);
    std::string result(13, '2');
    for (std::size_t i = result.size(); i-- > 0;)
    {
        result[i] = kTidAlphabet[value & 0x1F];
        value >>= 5;
    }
    return result;
}

std::string makeTid()
{
    static std::atomic<std::uint16_t> counter{0};
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    return makeTid(micros, counter.fetch_add(1, std::memory_order_relaxed));
}

} // namespace teal
