#pragma once

#include <string_view>

namespace upload_policy
{

enum class play_outcome
{
    success,
    retry,
    drop,
};

inline play_outcome classify_failure(bool retryable, std::string_view code)
{
    const bool terminal = !retryable && code != "NoCredentials" && code != "AuthFailed";
    return terminal ? play_outcome::drop : play_outcome::retry;
}

} // namespace upload_policy
