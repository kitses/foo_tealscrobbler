#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace play_queue
{

struct queue_item
{
    std::string rkey;
    std::string record_json;
    int attempts = 0;
    std::int64_t next_attempt = 0;
};

using log_sink = void (*)(const std::string &);

int retry_delay_seconds(int attempts);

std::string to_json(std::vector<queue_item> const &items, log_sink log);

std::vector<queue_item> from_json(std::string_view text, std::int64_t now);

} // namespace play_queue
