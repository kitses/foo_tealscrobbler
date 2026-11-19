#include "teal_client.h"

#include "json.hpp"
#include "teal_collections.h"

namespace teal
{

using json = nlohmann::json;

namespace
{

json play_view_json(const PlayView &view)
{
    json object = json::object();
    object["trackName"] = view.trackName;
    if (!view.releaseName.empty())
    {
        object["releaseName"] = view.releaseName;
    }
    if (!view.recordingMbId.empty())
    {
        object["recordingMbId"] = view.recordingMbId;
    }
    if (!view.isrc.empty())
    {
        object["isrc"] = view.isrc;
    }
    if (!view.musicServiceUri.empty())
    {
        object["musicServiceUri"] = view.musicServiceUri;
    }
    if (!view.submissionClientAgent.empty())
    {
        object["submissionClientAgent"] = view.submissionClientAgent;
    }
    if (!view.playedTime.empty())
    {
        object["playedTime"] = view.playedTime;
    }
    if (view.durationSeconds.has_value() && *view.durationSeconds > 0)
    {
        object["duration"] = *view.durationSeconds;
    }
    json artists = json::array();
    for (const auto &artist : view.artists)
    {
        if (artist.name.empty())
        {
            continue;
        }
        json entry = json::object();
        entry["artistName"] = artist.name;
        if (!artist.mbid.empty())
        {
            entry["artistMbId"] = artist.mbid;
        }
        artists.push_back(std::move(entry));
    }
    object["artists"] = std::move(artists);
    return object;
}

} // namespace

std::string buildPlayRecordJson(const PlayRecord &play)
{
    json record = play_view_json(play.view);
    record["$type"] = detail::kPlayCollection;
    return record.dump();
}

std::string buildStatusRecordJson(const StatusRecord &status)
{
    json record = json::object();
    record["$type"] = detail::kStatusCollection;
    record["time"] = status.time;
    record["expiry"] = status.expiry;
    record["item"] = play_view_json(status.item);
    return record.dump();
}

} // namespace teal
