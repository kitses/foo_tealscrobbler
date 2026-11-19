#include "teal_client.h"

#include "json.hpp"
#include "teal_collections.h"
#include "teal_did.h"
#include "teal_text.h"

namespace teal
{
namespace
{

using json = nlohmann::json;

std::string normalize_identifier(std::string value)
{
    value = detail::trim(value);
    if (!value.empty() && value.front() == '@')
    {
        value.erase(value.begin());
    }
    return value;
}

json parse_json(const std::string &body)
{
    if (body.empty())
    {
        return json::object();
    }
    try
    {
        return json::parse(body);
    }
    catch (const json::exception &)
    {
        return json::object();
    }
}

std::string json_error_code(const json &body)
{
    if (body.is_object())
    {
        const auto it = body.find("error");
        if (it != body.end() && it->is_string())
        {
            return it->get<std::string>();
        }
    }
    return {};
}

std::string json_error_message(const json &body, int status)
{
    if (body.is_object())
    {
        const auto it = body.find("message");
        if (it != body.end() && it->is_string())
        {
            return it->get<std::string>();
        }
    }
    return "HTTP " + std::to_string(status);
}

bool is_auth_error(int status, const std::string &code)
{
    return status == 401 || code == "ExpiredToken" || code == "InvalidToken" || code == "AuthMissing";
}

bool is_retryable_status(int status)
{
    return status == 0 || status == 408 || status == 425 || status == 429 || status >= 500;
}

ApiError error_from_response(const HttpResponse &response)
{
    const json body = parse_json(response.body);
    std::string code = json_error_code(body);
    std::string message = json_error_message(body, response.status);
    return ApiError(std::move(message), response.status, std::move(code), is_retryable_status(response.status));
}

std::string make_url(const std::string &pds, const std::string &method,
                     const std::vector<std::pair<std::string, std::string>> &query)
{
    std::string url = pds;
    if (!url.empty() && url.back() == '/')
    {
        url.pop_back();
    }
    url += "/xrpc/";
    url += method;
    bool first = true;
    for (const auto &[key, value] : query)
    {
        url += first ? '?' : '&';
        first = false;
        url += detail::percent_encode(key);
        url += '=';
        url += detail::percent_encode(value);
    }
    return url;
}

} // namespace

ApiError::ApiError(std::string message, int status, std::string code, bool retryable)
    : std::runtime_error(std::move(message)), status_(status), code_(std::move(code)), retryable_(retryable)
{
}

std::string normalizeMbid(std::string_view value)
{
    const std::string text = detail::trim(value);
    if (text.empty())
    {
        return {};
    }
    if (detail::starts_with(text, "mbid:"))
    {
        return text;
    }
    return "mbid:" + text;
}

Client::Client(HttpTransport &transport, std::string identifier, std::string password, std::string clientAgent)
    : transport_(transport), identifier_(normalize_identifier(std::move(identifier))), password_(std::move(password)),
      clientAgent_(std::move(clientAgent))
{
}

void Client::setCredentials(std::string identifier, std::string password)
{
    identifier_ = normalize_identifier(std::move(identifier));
    password_ = std::move(password);
    did_.clear();
    pds_.clear();
    authError_.clear();
    resetSession();
}

void Client::resetSession()
{
    accessJwt_.clear();
    refreshJwt_.clear();
    statusCid_.clear();
}

void Client::ensureSession()
{
    if (!accessJwt_.empty())
    {
        return;
    }
    if (detail::trim(identifier_).empty() || password_.empty())
    {
        throw ApiError("No teal.fm account configured", 0, "NoCredentials", false);
    }
    authenticate();
}

void Client::authenticate()
{
    if (!authError_.empty())
    {
        throw ApiError(authError_, 0, "AuthFailed", false);
    }
    try
    {
        if (did_.empty())
        {
            did_ = resolveDid(identifier_);
        }
        if (pds_.empty())
        {
            pds_ = resolvePds(did_);
        }
        createSession();
    }
    catch (const ApiError &error)
    {
        if (!error.retryable())
        {
            authError_ = error.what();
        }
        throw;
    }
}

std::string Client::resolveDid(const std::string &identifier)
{
    if (detail::starts_with(identifier, "did:"))
    {
        return identifier;
    }
    const std::vector<std::string> bases = {
        "https://public.api.bsky.app",
        "https://bsky.social",
    };
    std::string last_error;
    bool last_retryable = false;
    for (const auto &base : bases)
    {
        HttpRequest request;
        request.method = "GET";
        request.url = base + "/xrpc/com.atproto.identity.resolveHandle?handle=" + detail::percent_encode(identifier);
        try
        {
            const HttpResponse response = sendHttp(request);
            const json body = parse_json(response.body);
            if (response.status >= 200 && response.status < 300)
            {
                const auto did = body.find("did");
                if (did != body.end() && did->is_string())
                {
                    const std::string value = did->get<std::string>();
                    if (detail::starts_with(value, "did:"))
                    {
                        return value;
                    }
                }
                throw ApiError("Handle resolver returned no DID", response.status, "InvalidDid", false);
            }
            if (response.status == 400 || response.status == 404)
            {
                const auto message = body.find("message");
                last_error =
                    message != body.end() && message->is_string() ? message->get<std::string>() : "handle not found";
                last_retryable = false;
                continue;
            }
            throw error_from_response(response);
        }
        catch (const ApiError &error)
        {
            if (error.retryable())
            {
                last_error = error.what();
                last_retryable = true;
                continue;
            }
            throw;
        }
    }
    throw ApiError("Could not resolve handle '" + identifier + "'" + (last_error.empty() ? "" : ": " + last_error), 0,
                   last_retryable ? "NetworkError" : "HandleNotFound", last_retryable);
}

std::string Client::resolvePds(const std::string &did)
{
    HttpRequest request;
    request.method = "GET";
    request.url = detail::did_document_url(did);
    const HttpResponse response = sendHttp(request);
    if (response.status < 200 || response.status >= 300)
    {
        throw error_from_response(response);
    }
    return detail::extract_pds(parse_json(response.body));
}

void Client::createSession()
{
    if (pds_.empty())
    {
        throw ApiError("No PDS endpoint available", 0, "NoPds", false);
    }
    json body = json::object();
    body["identifier"] = identifier_;
    body["password"] = password_;
    HttpRequest request;
    request.method = "POST";
    request.url = make_url(pds_, "com.atproto.server.createSession", {});
    request.body = body.dump();
    const HttpResponse response = sendHttp(request);
    const json parsed = parse_json(response.body);
    if (response.status < 200 || response.status >= 300)
    {
        const std::string code = json_error_code(parsed);
        if (code == "AuthFactorTokenRequired")
        {
            throw ApiError("Two-factor authentication is required. Use an app password instead of your main password.",
                           response.status, code, false);
        }
        if (response.status == 401 || code == "AuthenticationRequired" || code == "InvalidIdentifier")
        {
            throw ApiError(
                "Bad handle or app password. Please correct the handle/app password information you have provided.",
                response.status, code, false);
        }
        if (response.status == 403)
        {
            throw ApiError("This account is blocked from writing to this PDS (403). "
                           "Check that it is not deactivated or taken down.",
                           response.status, code, false);
        }
        throw error_from_response(response);
    }
    const auto access = parsed.find("accessJwt");
    const auto refresh = parsed.find("refreshJwt");
    const auto did = parsed.find("did");
    if (access == parsed.end() || !access->is_string() || refresh == parsed.end() || !refresh->is_string())
    {
        throw ApiError("Authentication response did not contain session tokens", response.status, "InvalidSession",
                       true);
    }
    accessJwt_ = access->get<std::string>();
    refreshJwt_ = refresh->get<std::string>();
    if (did != parsed.end() && did->is_string())
    {
        did_ = did->get<std::string>();
    }
    statusCid_.clear();
}

void Client::refreshSession()
{
    if (refreshJwt_.empty())
    {
        throw ApiError("No refresh token available", 0, "NoRefreshToken", false);
    }
    HttpRequest request;
    request.method = "POST";
    request.url = make_url(pds_, "com.atproto.server.refreshSession", {});
    request.headers.emplace_back("Authorization", "Bearer " + refreshJwt_);
    const HttpResponse response = sendHttp(request);
    const json parsed = parse_json(response.body);
    if (response.status < 200 || response.status >= 300)
    {
        throw error_from_response(response);
    }
    const auto access = parsed.find("accessJwt");
    const auto refresh = parsed.find("refreshJwt");
    if (access == parsed.end() || !access->is_string() || refresh == parsed.end() || !refresh->is_string())
    {
        throw ApiError("Refresh response did not contain session tokens", response.status, "InvalidSession", true);
    }
    accessJwt_ = access->get<std::string>();
    refreshJwt_ = refresh->get<std::string>();
    const auto did = parsed.find("did");
    if (did != parsed.end() && did->is_string())
    {
        const std::string updated = did->get<std::string>();
        if (updated != did_)
        {
            statusCid_.clear();
        }
        did_ = updated;
    }
}

void Client::handleAuthFailure()
{
    if (!refreshJwt_.empty())
    {
        try
        {
            refreshSession();
            return;
        }
        catch (const ApiError &error)
        {
            if (error.status() == 403)
            {
                authError_ = error.what();
                throw;
            }
            if (error.status() != 400 && error.status() != 401 && error.retryable())
            {
                throw;
            }
        }
    }
    accessJwt_.clear();
    refreshJwt_.clear();
    authenticate();
}

HttpResponse Client::sendHttp(const HttpRequest &request)
{
    HttpRequest outbound = request;
    outbound.headers.emplace_back("Accept", "application/json");
    outbound.headers.emplace_back("User-Agent", clientAgent_);
    try
    {
        return transport_.send(outbound);
    }
    catch (const ApiError &)
    {
        throw;
    }
    catch (const std::exception &error)
    {
        throw ApiError(std::string("Network error: ") + error.what(), 0, "NetworkError", true);
    }
}

std::string Client::request(const HttpRequest &base, bool auth, bool allowAuthRetry)
{
    HttpRequest outbound = base;
    if (auth)
    {
        if (accessJwt_.empty())
        {
            ensureSession();
        }
        outbound.headers.emplace_back("Authorization", "Bearer " + accessJwt_);
    }
    const HttpResponse response = sendHttp(outbound);
    const json parsed = parse_json(response.body);
    const std::string code = json_error_code(parsed);
    if (auth && allowAuthRetry && is_auth_error(response.status, code))
    {
        handleAuthFailure();
        return request(base, auth, false);
    }
    if (response.status >= 200 && response.status < 300)
    {
        return response.body;
    }
    throw error_from_response(response);
}

std::string Client::xrpcPost(const std::string &method, const std::string &bodyJson, bool auth, bool allowAuthRetry)
{
    if (auth)
    {
        ensureSession();
    }
    HttpRequest request;
    request.method = "POST";
    request.url = make_url(pds_, method, {});
    request.body = bodyJson;
    return this->request(request, auth, allowAuthRetry);
}

std::string Client::xrpcGet(const std::string &method, const std::vector<std::pair<std::string, std::string>> &query,
                            bool auth, bool allowAuthRetry)
{
    if (auth)
    {
        ensureSession();
    }
    HttpRequest request;
    request.method = "GET";
    request.url = make_url(pds_, method, query);
    return this->request(request, auth, allowAuthRetry);
}

void Client::submitPlay(const PlayRecord &play)
{
    if (detail::trim(play.view.trackName).empty())
    {
        throw std::invalid_argument("trackName must not be empty");
    }
    if (play.view.artists.empty())
    {
        throw std::invalid_argument("at least one artist is required");
    }
    submitPlayRecord(play.rkey.empty() ? makeTid() : play.rkey, buildPlayRecordJson(play));
}

void Client::submitPlayRecord(const std::string &rkey, const std::string &recordJson)
{
    ensureSession();
    putRecord(detail::kPlayCollection, rkey, recordJson, false);
}

std::optional<std::string> Client::getStatusCid()
{
    ensureSession();
    if (!statusCid_.empty())
    {
        return statusCid_;
    }
    try
    {
        const std::string response =
            xrpcGet("com.atproto.repo.getRecord",
                    {{"repo", did_}, {"collection", detail::kStatusCollection}, {"rkey", "self"}}, true);
        const json body = parse_json(response);
        const auto cid = body.find("cid");
        if (cid != body.end() && cid->is_string())
        {
            statusCid_ = cid->get<std::string>();
            return statusCid_;
        }
        return std::nullopt;
    }
    catch (const ApiError &error)
    {
        if (error.code() == "RecordNotFound")
        {
            return std::nullopt;
        }
        throw;
    }
}

void Client::putRecord(const std::string &collection, const std::string &rkey, const std::string &recordJson,
                       bool allowSwapRetry)
{
    ensureSession();
    const bool isStatus = collection == detail::kStatusCollection;
    json body = json::object();
    body["repo"] = did_;
    body["collection"] = collection;
    body["rkey"] = rkey;
    body["record"] = json::parse(recordJson);
    body["validate"] = false;
    if (isStatus)
    {
        const auto cid = getStatusCid();
        if (cid.has_value())
        {
            body["swapRecord"] = *cid;
        }
    }
    try
    {
        const std::string response = xrpcPost("com.atproto.repo.putRecord", body.dump(), true);
        if (isStatus)
        {
            const json parsed = parse_json(response);
            const auto cid = parsed.find("cid");
            statusCid_ = cid != parsed.end() && cid->is_string() ? cid->get<std::string>() : std::string();
        }
    }
    catch (const ApiError &error)
    {
        if (isStatus && error.code() == "InvalidSwap" && allowSwapRetry)
        {
            statusCid_.clear();
            putRecord(collection, rkey, recordJson, false);
            return;
        }
        throw;
    }
}

void Client::submitStatus(const StatusRecord &status)
{
    const std::string record = buildStatusRecordJson(status);
    putRecord(detail::kStatusCollection, "self", record, true);
}

void Client::clearStatus()
{
    StatusRecord status;
    status.time = utcNowIso();
    status.expiry = utcIsoFromEpochSeconds(unixNowSeconds() - 60);
    status.item.trackName = "";
    status.item.artists.clear();
    const std::string record = buildStatusRecordJson(status);
    putRecord(detail::kStatusCollection, "self", record, true);
}

} // namespace teal
