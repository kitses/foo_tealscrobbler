#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace teal {

struct HttpRequest {
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;
};

class ApiError : public std::runtime_error {
public:
    ApiError(std::string message, int status, std::string code, bool retryable);

    int status() const noexcept { return status_; }
    const std::string& code() const noexcept { return code_; }
    bool retryable() const noexcept { return retryable_; }

private:
    int status_;
    std::string code_;
    bool retryable_;
};

struct Artist {
    std::string name;
    std::string mbid;
};

struct PlayView {
    std::string trackName;
    std::string releaseName;
    std::string recordingMbId;
    std::string isrc;
    std::string musicServiceUri;
    std::string submissionClientAgent;
    std::string playedTime;
    std::optional<std::int64_t> durationSeconds;
    std::vector<Artist> artists;
};

struct PlayRecord {
    PlayView view;
    std::string rkey;
};

struct StatusRecord {
    PlayView item;
    std::string time;
    std::string expiry;
};

std::string utcNowIso();
std::string utcIsoFromEpochSeconds(std::int64_t seconds);
std::int64_t unixNowSeconds();
std::string makeTid();
std::string makeTid(std::int64_t unixMicros, std::uint16_t clockId);
std::string normalizeMbid(std::string_view value);
std::string buildPlayRecordJson(const PlayRecord& play);
std::string buildStatusRecordJson(const StatusRecord& status);

class Client {
public:
    Client(HttpTransport& transport, std::string identifier, std::string password, std::string clientAgent);

    void setCredentials(std::string identifier, std::string password);
    void resetSession();

    void submitPlay(const PlayRecord& play);
    void submitPlayRecord(const std::string& rkey, const std::string& recordJson);
    void submitStatus(const StatusRecord& status);
    void clearStatus();

    const std::string& did() const noexcept { return did_; }
    const std::string& pds() const noexcept { return pds_; }
    bool authFailed() const noexcept { return !authError_.empty(); }

private:
    void ensureSession();
    void authenticate();
    void createSession();
    void refreshSession();
    std::string resolveDid(const std::string& identifier);
    std::string resolvePds(const std::string& did);
    void putRecord(const std::string& collection, const std::string& rkey, const std::string& recordJson, bool allowSwapRetry = false);
    std::optional<std::string> getStatusCid();
    std::string xrpcPost(const std::string& method, const std::string& bodyJson, bool auth, bool allowAuthRetry = true);
    std::string xrpcGet(const std::string& method, const std::vector<std::pair<std::string, std::string>>& query, bool auth, bool allowAuthRetry = true);
    std::string request(const HttpRequest& request, bool auth, bool allowAuthRetry);
    HttpResponse sendHttp(const HttpRequest& request);
    void handleAuthFailure();

    HttpTransport& transport_;
    std::string identifier_;
    std::string password_;
    std::string clientAgent_;
    std::string did_;
    std::string pds_;
    std::string accessJwt_;
    std::string refreshJwt_;
    std::string statusCid_;
    std::string authError_;
};

}
