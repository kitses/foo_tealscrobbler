#include "../src/play_queue.h"
#include "../src/teal_client.h"
#include "../src/upload_policy.h"
#include "../vendor/json.hpp"

#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <string>

using json = nlohmann::json;
using teal::HttpRequest;
using teal::HttpResponse;

namespace
{

std::vector<std::string> g_queue_log;

void record_log(const std::string &message)
{
    g_queue_log.push_back(message);
}

struct FakeTransport : teal::HttpTransport
{
    std::function<HttpResponse(const HttpRequest &)> handler;
    HttpResponse send(const HttpRequest &request) override
    {
        return handler(request);
    }
};

HttpResponse json_response(int status, const json &body)
{
    return HttpResponse{status, body.dump()};
}

std::string header_value(const HttpRequest &request, const std::string &name)
{
    for (const auto &[key, value] : request.headers)
    {
        if (key == name)
        {
            return value;
        }
    }
    return {};
}

void test_tid_and_helpers()
{
    const std::string tid = teal::makeTid(1700000000000000LL, 7);
    assert(tid.size() == 13);
    for (char c : tid)
    {
        assert(std::string("234567abcdefghijklmnopqrstuvwxyz").find(c) != std::string::npos);
    }
    assert(teal::makeTid(1700000000000000LL, 7) == tid);
    assert(teal::normalizeMbid(" 1234 ") == "mbid:1234");
    assert(teal::normalizeMbid("mbid:1234") == "mbid:1234");
    assert(teal::normalizeMbid("") == "");
    assert(teal::utcIsoFromEpochSeconds(0) == "1970-01-01T00:00:00Z");
}

void test_auth_and_play_submission()
{
    FakeTransport transport;
    int create_calls = 0;
    int put_calls = 0;
    transport.handler = [&](const HttpRequest &request) {
        if (request.url.find("resolveHandle") != std::string::npos)
        {
            assert(request.method == "GET");
            return json_response(200, {{"did", "did:plc:alice"}});
        }
        if (request.url == "https://plc.directory/did:plc:alice")
        {
            return json_response(200, {{"service",
                                        {{{"id", "#atproto_pds"},
                                          {"type", "AtprotoPersonalDataServer"},
                                          {"serviceEndpoint", "https://pds.test/"}}}}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.createSession")
        {
            ++create_calls;
            const json body = json::parse(request.body);
            assert(body["identifier"] == "alice.test");
            assert(body["password"] == "app-password");
            assert(header_value(request, "User-Agent") == "fm.tealscrobbler/1.0.0");
            return json_response(200, {{"accessJwt", "access-1"},
                                       {"refreshJwt", "refresh-1"},
                                       {"did", "did:plc:alice"},
                                       {"handle", "alice.test"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.repo.putRecord")
        {
            ++put_calls;
            assert(header_value(request, "Authorization") == "Bearer access-1");
            const json body = json::parse(request.body);
            assert(body["repo"] == "did:plc:alice");
            assert(body["collection"] == "fm.teal.feed.play");
            assert(body["validate"] == false);
            assert(body["rkey"].get<std::string>().size() == 13);
            const json record = body["record"];
            assert(record["$type"] == "fm.teal.feed.play");
            assert(record["trackName"] == "Song");
            assert(record["artists"][0]["artistName"] == "Artist");
            assert(record["duration"] == 200);
            assert(record["recordingMbId"] == "mbid:abc");
            assert(record["musicServiceUri"] == "https://foobar2000.org");
            assert(record["submissionClientAgent"] == "fm.tealscrobbler/1.0.0");
            return json_response(200, {{"uri", "at://did:plc:alice/fm.teal.feed.play/abc"}, {"cid", "bafyplay"}});
        }
        assert(false);
        return HttpResponse{};
    };

    teal::Client client(transport, "alice.test", "app-password", "fm.tealscrobbler/1.0.0");
    teal::PlayRecord play;
    play.view.trackName = "Song";
    play.view.durationSeconds = 200;
    play.view.recordingMbId = "mbid:abc";
    play.view.musicServiceUri = "https://foobar2000.org";
    play.view.submissionClientAgent = "fm.tealscrobbler/1.0.0";
    play.view.playedTime = "2026-01-01T00:00:00Z";
    play.view.artists.push_back({"Artist", "mbid:def"});
    client.submitPlay(play);
    assert(create_calls == 1);
    assert(put_calls == 1);
    assert(client.did() == "did:plc:alice");
    assert(client.pds() == "https://pds.test");
}

void test_refresh_on_expired_access_token()
{
    FakeTransport transport;
    int refresh_calls = 0;
    int put_calls = 0;
    transport.handler = [&](const HttpRequest &request) {
        if (request.url.find("resolveHandle") != std::string::npos)
        {
            return json_response(200, {{"did", "did:plc:alice"}});
        }
        if (request.url == "https://plc.directory/did:plc:alice")
        {
            return json_response(200,
                                 {{"service", {{{"id", "#atproto_pds"}, {"serviceEndpoint", "https://pds.test"}}}}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.createSession")
        {
            return json_response(200,
                                 {{"accessJwt", "old-access"}, {"refreshJwt", "refresh-1"}, {"did", "did:plc:alice"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.refreshSession")
        {
            ++refresh_calls;
            assert(header_value(request, "Authorization") == "Bearer refresh-1");
            return json_response(200,
                                 {{"accessJwt", "new-access"}, {"refreshJwt", "refresh-2"}, {"did", "did:plc:alice"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.repo.putRecord")
        {
            ++put_calls;
            if (put_calls == 1)
            {
                assert(header_value(request, "Authorization") == "Bearer old-access");
                return json_response(401, {{"error", "ExpiredToken"}, {"message", "Token has expired"}});
            }
            assert(header_value(request, "Authorization") == "Bearer new-access");
            return json_response(200, {{"uri", "at://x"}, {"cid", "bafy"}});
        }
        assert(false);
        return HttpResponse{};
    };

    teal::Client client(transport, "alice.test", "app-password", "fm.tealscrobbler/1.0.0");
    teal::PlayRecord play;
    play.view.trackName = "Song";
    play.view.artists.push_back({"Artist", ""});
    client.submitPlay(play);
    assert(refresh_calls == 1);
    assert(put_calls == 2);
}

void test_status_swap_record_and_clear()
{
    FakeTransport transport;
    std::string status_cid;
    int get_calls = 0;
    int put_calls = 0;
    bool swapped_once = false;
    std::string last_swap;
    json last_record;
    transport.handler = [&](const HttpRequest &request) {
        if (request.url.find("resolveHandle") != std::string::npos)
        {
            return json_response(200, {{"did", "did:plc:alice"}});
        }
        if (request.url == "https://plc.directory/did:plc:alice")
        {
            return json_response(200,
                                 {{"service", {{{"id", "#atproto_pds"}, {"serviceEndpoint", "https://pds.test"}}}}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.createSession")
        {
            return json_response(200, {{"accessJwt", "access"}, {"refreshJwt", "refresh"}, {"did", "did:plc:alice"}});
        }
        if (request.url.find("com.atproto.repo.getRecord") != std::string::npos)
        {
            ++get_calls;
            assert(request.url.find("collection=fm.teal.actor.status") != std::string::npos);
            assert(request.url.find("rkey=self") != std::string::npos);
            if (status_cid.empty())
            {
                return json_response(400, {{"error", "RecordNotFound"}, {"message", "Could not locate record"}});
            }
            return json_response(200, {{"uri", "at://status"}, {"cid", status_cid}, {"value", json::object()}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.repo.putRecord")
        {
            ++put_calls;
            const json body = json::parse(request.body);
            assert(body["collection"] == "fm.teal.actor.status");
            assert(body["rkey"] == "self");
            last_swap = body.contains("swapRecord") ? body["swapRecord"].get<std::string>() : "";
            last_record = body["record"];
            if (!status_cid.empty())
            {
                assert(last_swap == status_cid);
            }
            else
            {
                assert(last_swap.empty());
            }
            if (put_calls == 2 && !swapped_once)
            {
                swapped_once = true;
                status_cid = "bafy-other";
                return json_response(400, {{"error", "InvalidSwap"}, {"message", "Swap record did not match"}});
            }
            status_cid = "bafy" + std::to_string(put_calls);
            return json_response(200, {{"uri", "at://status"}, {"cid", status_cid}});
        }
        assert(false);
        return HttpResponse{};
    };

    teal::Client client(transport, "alice.test", "app-password", "fm.tealscrobbler/1.0.0");
    teal::StatusRecord status;
    status.time = "2026-01-01T00:00:00Z";
    status.expiry = "2026-01-01T00:10:00Z";
    status.item.trackName = "Song";
    status.item.artists.push_back({"Artist", ""});
    client.submitStatus(status);
    assert(get_calls == 1);
    assert(put_calls == 1);
    assert(last_swap.empty());

    client.submitStatus(status);
    assert(put_calls == 3);
    assert(get_calls == 2);
    assert(last_swap == "bafy-other");
    assert(last_record["$type"] == "fm.teal.actor.status");

    client.clearStatus();
    assert(put_calls == 4);
    assert(last_record["item"]["trackName"] == "");
    assert(last_record["item"]["artists"].empty());
}

void test_refresh_failure_falls_back_to_password_login()
{
    FakeTransport transport;
    int resolve_calls = 0;
    int create_calls = 0;
    int refresh_calls = 0;
    int put_calls = 0;
    transport.handler = [&](const HttpRequest &request) {
        if (request.url.find("resolveHandle") != std::string::npos)
        {
            ++resolve_calls;
            return json_response(200, {{"did", "did:plc:alice"}});
        }
        if (request.url == "https://plc.directory/did:plc:alice")
        {
            return json_response(200,
                                 {{"service", {{{"id", "#atproto_pds"}, {"serviceEndpoint", "https://pds.test"}}}}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.createSession")
        {
            ++create_calls;
            return json_response(200, {{"accessJwt", "access-" + std::to_string(create_calls)},
                                       {"refreshJwt", "refresh-" + std::to_string(create_calls)},
                                       {"did", "did:plc:alice"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.refreshSession")
        {
            ++refresh_calls;
            assert(header_value(request, "Authorization") == "Bearer refresh-1");
            return json_response(401, {{"error", "ExpiredToken"}, {"message", "Refresh token expired"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.repo.putRecord")
        {
            ++put_calls;
            if (put_calls == 1)
            {
                assert(header_value(request, "Authorization") == "Bearer access-1");
                return json_response(401, {{"error", "ExpiredToken"}, {"message", "Access token expired"}});
            }
            assert(header_value(request, "Authorization") == "Bearer access-2");
            return json_response(200, {{"uri", "at://x"}, {"cid", "bafy"}});
        }
        assert(false);
        return HttpResponse{};
    };

    teal::Client client(transport, "alice.test", "app-password", "fm.tealscrobbler/1.0.0");
    teal::PlayRecord play;
    play.view.trackName = "Song";
    play.view.artists.push_back({"Artist", ""});
    client.submitPlay(play);
    assert(refresh_calls == 1);
    assert(create_calls == 2);
    assert(put_calls == 2);
    assert(resolve_calls == 1);
}

void test_transient_refresh_failure_preserves_session()
{
    FakeTransport transport;
    int create_calls = 0;
    int refresh_calls = 0;
    int put_calls = 0;
    transport.handler = [&](const HttpRequest &request) {
        if (request.url.find("resolveHandle") != std::string::npos)
        {
            return json_response(200, {{"did", "did:plc:alice"}});
        }
        if (request.url == "https://plc.directory/did:plc:alice")
        {
            return json_response(200,
                                 {{"service", {{{"id", "#atproto_pds"}, {"serviceEndpoint", "https://pds.test"}}}}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.createSession")
        {
            ++create_calls;
            return json_response(200,
                                 {{"accessJwt", "access-1"}, {"refreshJwt", "refresh-1"}, {"did", "did:plc:alice"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.refreshSession")
        {
            ++refresh_calls;
            return HttpResponse{0, ""};
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.repo.putRecord")
        {
            ++put_calls;
            if (put_calls == 1)
            {
                return json_response(401, {{"error", "ExpiredToken"}, {"message", "Access token expired"}});
            }
            assert(header_value(request, "Authorization") == "Bearer access-1");
            return json_response(200, {{"uri", "at://x"}, {"cid", "bafy"}});
        }
        assert(false);
        return HttpResponse{};
    };

    teal::Client client(transport, "alice.test", "app-password", "fm.tealscrobbler/1.0.0");
    teal::PlayRecord play;
    play.view.trackName = "Song";
    play.view.artists.push_back({"Artist", ""});
    bool caught = false;
    try
    {
        client.submitPlay(play);
    }
    catch (const teal::ApiError &error)
    {
        caught = true;
        assert(error.retryable());
    }
    assert(caught);
    assert(refresh_calls == 1);
    assert(create_calls == 1);
    assert(!client.authFailed());

    client.submitPlay(play);
    assert(create_calls == 1);
    assert(put_calls == 2);
}

void test_terminal_login_failure_is_latched()
{
    FakeTransport transport;
    int transport_calls = 0;
    int create_calls = 0;
    bool reject = true;
    transport.handler = [&](const HttpRequest &request) {
        ++transport_calls;
        if (request.url.find("resolveHandle") != std::string::npos)
        {
            return json_response(200, {{"did", "did:plc:alice"}});
        }
        if (request.url == "https://plc.directory/did:plc:alice")
        {
            return json_response(200,
                                 {{"service", {{{"id", "#atproto_pds"}, {"serviceEndpoint", "https://pds.test"}}}}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.createSession")
        {
            ++create_calls;
            if (reject)
            {
                return json_response(
                    401, {{"error", "AuthenticationRequired"}, {"message", "Invalid identifier or password"}});
            }
            return json_response(200,
                                 {{"accessJwt", "access-1"}, {"refreshJwt", "refresh-1"}, {"did", "did:plc:alice"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.repo.putRecord")
        {
            return json_response(200, {{"uri", "at://x"}, {"cid", "bafy"}});
        }
        assert(false);
        return HttpResponse{};
    };

    teal::Client client(transport, "alice.test", "wrong", "fm.tealscrobbler/1.0.0");
    teal::PlayRecord play;
    play.view.trackName = "Song";
    play.view.artists.push_back({"Artist", ""});

    bool caught = false;
    try
    {
        client.submitPlay(play);
    }
    catch (const teal::ApiError &error)
    {
        caught = true;
        assert(!error.retryable());
        assert(error.code() == "AuthenticationRequired");
        assert(std::string(error.what()).find("Bad handle or app password") != std::string::npos);
    }
    assert(caught);
    assert(client.authFailed());

    const int calls_after_failure = transport_calls;
    caught = false;
    try
    {
        client.submitPlay(play);
    }
    catch (const teal::ApiError &error)
    {
        caught = true;
        assert(error.code() == "AuthFailed");
    }
    assert(caught);
    assert(transport_calls == calls_after_failure);

    reject = false;
    client.setCredentials("alice.test", "app-password");
    assert(!client.authFailed());
    client.submitPlay(play);
    assert(create_calls == 2);
}

void test_forbidden_refresh_is_terminal()
{
    FakeTransport transport;
    int create_calls = 0;
    int put_calls = 0;
    transport.handler = [&](const HttpRequest &request) {
        if (request.url.find("resolveHandle") != std::string::npos)
        {
            return json_response(200, {{"did", "did:plc:alice"}});
        }
        if (request.url == "https://plc.directory/did:plc:alice")
        {
            return json_response(200,
                                 {{"service", {{{"id", "#atproto_pds"}, {"serviceEndpoint", "https://pds.test"}}}}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.createSession")
        {
            ++create_calls;
            return json_response(200,
                                 {{"accessJwt", "access-1"}, {"refreshJwt", "refresh-1"}, {"did", "did:plc:alice"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.refreshSession")
        {
            return json_response(403, {{"error", "AccountDeactivated"}, {"message", "Account deactivated"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.repo.putRecord")
        {
            ++put_calls;
            return json_response(401, {{"error", "ExpiredToken"}, {"message", "Access token expired"}});
        }
        assert(false);
        return HttpResponse{};
    };

    teal::Client client(transport, "alice.test", "app-password", "fm.tealscrobbler/1.0.0");
    teal::PlayRecord play;
    play.view.trackName = "Song";
    play.view.artists.push_back({"Artist", ""});
    bool caught = false;
    try
    {
        client.submitPlay(play);
    }
    catch (const teal::ApiError &error)
    {
        caught = true;
        assert(!error.retryable());
        assert(error.status() == 403);
        assert(std::string(error.what()) == "Account deactivated");
    }
    assert(caught);
    assert(put_calls == 1);
    assert(create_calls == 1);
    assert(client.authFailed());
}

void test_fully_qualified_pds_service_id()
{
    FakeTransport transport;
    int put_calls = 0;
    transport.handler = [&](const HttpRequest &request) {
        if (request.url.find("resolveHandle") != std::string::npos)
        {
            return json_response(200, {{"did", "did:plc:alice"}});
        }
        if (request.url == "https://plc.directory/did:plc:alice")
        {
            return json_response(200, {{"service",
                                        {{{"id", "#atproto_labeler"},
                                          {"type", "AtprotoLabeler"},
                                          {"serviceEndpoint", "https://labeler.test"}},
                                         {{"id", "did:plc:alice#atproto_pds"},
                                          {"type", "AtprotoPersonalDataServer"},
                                          {"serviceEndpoint", "https://pds.test/"}}}}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.server.createSession")
        {
            return json_response(200,
                                 {{"accessJwt", "access-1"}, {"refreshJwt", "refresh-1"}, {"did", "did:plc:alice"}});
        }
        if (request.url == "https://pds.test/xrpc/com.atproto.repo.putRecord")
        {
            ++put_calls;
            return json_response(200, {{"uri", "at://did:plc:alice/fm.teal.feed.play/abc"}, {"cid", "bafyplay"}});
        }
        assert(false);
        return HttpResponse{};
    };

    teal::Client client(transport, "alice.test", "app-password", "fm.tealscrobbler/1.0.0");
    teal::PlayRecord play;
    play.view.trackName = "Song";
    play.view.artists.push_back({"Artist", ""});
    client.submitPlay(play);
    assert(client.pds() == "https://pds.test");
    assert(client.did() == "did:plc:alice");
    assert(put_calls == 1);
}

void test_document_without_pds_service_is_rejected()
{
    FakeTransport transport;
    transport.handler = [&](const HttpRequest &request) {
        if (request.url.find("resolveHandle") != std::string::npos)
        {
            return json_response(200, {{"did", "did:plc:alice"}});
        }
        if (request.url == "https://plc.directory/did:plc:alice")
        {
            return json_response(200, {{"service",
                                        {{{"id", "#atproto_labeler"},
                                          {"type", "AtprotoLabeler"},
                                          {"serviceEndpoint", "https://labeler.test"}}}}});
        }
        assert(false);
        return HttpResponse{};
    };

    teal::Client client(transport, "alice.test", "app-password", "fm.tealscrobbler/1.0.0");
    teal::PlayRecord play;
    play.view.trackName = "Song";
    play.view.artists.push_back({"Artist", ""});
    bool caught = false;
    try
    {
        client.submitPlay(play);
    }
    catch (const teal::ApiError &error)
    {
        caught = true;
        assert(error.code() == "NoPds");
        assert(!error.retryable());
    }
    assert(caught);
}

void test_upload_policy()
{
    using upload_policy::classify_failure;
    using upload_policy::play_outcome;

    assert(classify_failure(true, "NetworkError") == play_outcome::retry);
    assert(classify_failure(true, "InvalidRecord") == play_outcome::retry);
    assert(classify_failure(false, "InvalidRecord") == play_outcome::drop);
    assert(classify_failure(false, "AccountDeactivated") == play_outcome::drop);
    assert(classify_failure(false, "") == play_outcome::drop);
    assert(classify_failure(false, "NoCredentials") == play_outcome::retry);
    assert(classify_failure(false, "AuthFailed") == play_outcome::retry);

    assert(play_queue::retry_delay_seconds(0) == 60);
    assert(play_queue::retry_delay_seconds(1) == 60);
    assert(play_queue::retry_delay_seconds(2) == 120);
    assert(play_queue::retry_delay_seconds(3) == 300);
    assert(play_queue::retry_delay_seconds(4) == 600);
    assert(play_queue::retry_delay_seconds(5) == 900);
    assert(play_queue::retry_delay_seconds(500) == 900);
}

void test_play_queue_round_trip()
{
    using play_queue::queue_item;

    g_queue_log.clear();
    std::vector<queue_item> queue;
    queue.push_back({"3kaaaaaaaaaaa", R"({"$type":"fm.teal.feed.play","trackName":"Song"})", 2, 1000});
    queue.push_back({"3kbbbbbbbbbbb", R"({"$type":"fm.teal.feed.play","trackName":"Other"})", 0, 0});

    const std::string text = play_queue::to_json(queue, record_log);
    const std::vector<queue_item> restored = play_queue::from_json(text, 5000);
    assert(g_queue_log.empty());
    assert(restored.size() == 2);
    assert(restored[0].rkey == queue[0].rkey);
    assert(restored[1].rkey == queue[1].rkey);
    assert(restored[0].attempts == 2);
    assert(restored[0].next_attempt == 1000);
    assert(json::parse(restored[0].record_json) == json::parse(queue[0].record_json));

    const std::string future =
        R"([{"rkey":"3kccccccccccc","record":{"$type":"fm.teal.feed.play","trackName":"S"},"next":999999}])";
    const std::vector<queue_item> clamped = play_queue::from_json(future, 1000);
    assert(clamped.size() == 1);
    assert(clamped[0].next_attempt == 1900);

    const std::string mixed = R"([
        {"rkey":"a","record":{"$type":"fm.teal.feed.play","trackName":"Keep"}},
        {"rkey":"b","record":{"$type":"fm.teal.actor.status","trackName":"WrongType"}},
        {"rkey":"c","record":{"$type":"fm.teal.feed.play","trackName":""}},
        {"rkey":"d","record":{"$type":"fm.teal.feed.play"}},
        {"record":{"$type":"fm.teal.feed.play","trackName":"NoRkey"}},
        42
    ])";
    const std::vector<queue_item> kept = play_queue::from_json(mixed, 0);
    assert(kept.size() == 1);
    assert(kept[0].rkey == "a");

    assert(play_queue::from_json("not json", 0).empty());
    assert(play_queue::from_json("", 0).empty());

    g_queue_log.clear();
    std::vector<queue_item> broken;
    broken.push_back({"3kddddddddddd", "{not json", 0, 0});
    broken.push_back({"3keeeeeeeeeee", R"({"$type":"fm.teal.feed.play","trackName":"Ok"})", 0, 0});
    assert(play_queue::from_json(play_queue::to_json(broken, record_log), 0).size() == 1);
    assert(g_queue_log.size() == 1);

    std::vector<queue_item> many;
    for (int i = 0; i < 2005; ++i)
    {
        many.push_back({"rkey" + std::to_string(i), R"({"$type":"fm.teal.feed.play","trackName":"S"})", 0, 0});
    }
    const std::vector<queue_item> capped = play_queue::from_json(play_queue::to_json(many, record_log), 0);
    assert(capped.size() == 2000);
    assert(capped.front().rkey == "rkey5");
    assert(capped.back().rkey == "rkey2004");
}

} // namespace

int main()
{
    test_tid_and_helpers();
    test_auth_and_play_submission();
    test_refresh_on_expired_access_token();
    test_status_swap_record_and_clear();
    test_refresh_failure_falls_back_to_password_login();
    test_transient_refresh_failure_preserves_session();
    test_terminal_login_failure_is_latched();
    test_forbidden_refresh_is_terminal();
    test_fully_qualified_pds_service_id();
    test_document_without_pds_service_is_rejected();
    test_upload_policy();
    test_play_queue_round_trip();
    std::cout << "teal client tests passed\n";
    return 0;
}
