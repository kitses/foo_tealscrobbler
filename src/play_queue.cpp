#include "play_queue.h"

#include "json.hpp"

#include <algorithm>

namespace play_queue
{

using json = nlohmann::json;

int retry_delay_seconds(int attempts)
{
    if (attempts <= 1)
        return 60;
    if (attempts == 2)
        return 120;
    if (attempts == 3)
        return 300;
    if (attempts == 4)
        return 600;
    return 900;
}

std::string to_json(std::vector<queue_item> const &items, log_sink log)
{
    json array = json::array();
    for (const auto &item : items)
    {
        try
        {
            json entry = json::object();
            entry["rkey"] = item.rkey;
            entry["record"] = json::parse(item.record_json);
            entry["attempts"] = item.attempts;
            entry["next"] = item.next_attempt;
            array.push_back(std::move(entry));
        }
        catch (const json::exception &)
        {
            log("A queued play could not be written to the saved cache.");
        }
    }
    return array.dump();
}

std::vector<queue_item> from_json(std::string_view text, std::int64_t now)
{
    json parsed;
    try
    {
        parsed = json::parse(text);
    }
    catch (const json::exception &)
    {
        return {};
    }
    if (!parsed.is_array())
    {
        return {};
    }
    std::vector<queue_item> queue;
    for (const auto &entry : parsed)
    {
        if (!entry.is_object())
        {
            continue;
        }
        const auto rkey = entry.find("rkey");
        const auto record = entry.find("record");
        if (rkey == entry.end() || !rkey->is_string() || record == entry.end() || !record->is_object())
        {
            continue;
        }
        const auto type = record->find("$type");
        const auto track_name = record->find("trackName");
        if (type == record->end() || !type->is_string() || type->get<std::string>() != "fm.teal.feed.play" ||
            track_name == record->end() || !track_name->is_string() || track_name->get<std::string>().empty())
        {
            continue;
        }
        queue_item item;
        item.rkey = rkey->get<std::string>();
        item.record_json = record->dump();
        if (const auto attempts = entry.find("attempts"); attempts != entry.end() && attempts->is_number_integer())
        {
            item.attempts = attempts->get<int>();
        }
        if (const auto next = entry.find("next"); next != entry.end() && next->is_number_integer())
        {
            const std::int64_t ceiling = now + 900;
            item.next_attempt = std::min(next->get<std::int64_t>(), ceiling);
        }
        queue.push_back(std::move(item));
    }
    while (queue.size() > 2000)
    {
        queue.erase(queue.begin());
    }
    return queue;
}

} // namespace play_queue
