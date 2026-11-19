#include "teal_did.h"

#include "teal_client.h"
#include "teal_text.h"

namespace teal
{
namespace
{

constexpr std::string_view kPdsServiceFragment = "#atproto_pds";

std::string encode_path_segments(std::string_view path)
{
    std::string out;
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        if (!out.empty())
        {
            out.push_back('/');
        }
        out += detail::percent_encode(path.substr(start, end - start));
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return out;
}

} // namespace

namespace detail
{

std::string did_document_url(const std::string &did)
{
    if (starts_with(did, "did:plc:"))
    {
        return "https://plc.directory/" + did;
    }
    if (starts_with(did, "did:web:"))
    {
        std::string rest = percent_decode(std::string_view(did).substr(8));
        const std::size_t colon = rest.find(':');
        const std::string host = colon == std::string::npos ? rest : rest.substr(0, colon);
        std::string path = colon == std::string::npos ? std::string() : rest.substr(colon + 1);
        for (char &c : path)
        {
            if (c == ':')
            {
                c = '/';
            }
        }
        if (host.empty())
        {
            throw ApiError("Invalid did:web identifier", 0, "InvalidDid", false);
        }
        if (path.empty())
        {
            return "https://" + host + "/.well-known/did.json";
        }
        return "https://" + host + "/" + encode_path_segments(path) + "/did.json";
    }
    throw ApiError("Unsupported DID method. Use a handle or a did:plc/did:web identifier.", 0, "UnsupportedDid", false);
}

std::string extract_pds(const json &document)
{
    if (!document.is_object())
    {
        throw ApiError("DID document is not a JSON object", 0, "InvalidDidDocument", false);
    }
    const auto service = document.find("service");
    if (service == document.end() || !service->is_array())
    {
        throw ApiError("DID document has no ATProto PDS service", 0, "NoPds", false);
    }
    for (const auto &item : *service)
    {
        if (!item.is_object())
        {
            continue;
        }
        const auto id = item.find("id");
        const auto endpoint = item.find("serviceEndpoint");
        if (id == item.end() || !id->is_string() || endpoint == item.end() || !endpoint->is_string())
        {
            continue;
        }
        const std::string id_text = id->get<std::string>();
        if (id_text.size() < kPdsServiceFragment.size() ||
            id_text.compare(id_text.size() - kPdsServiceFragment.size(), kPdsServiceFragment.size(),
                            kPdsServiceFragment) != 0)
        {
            continue;
        }
        std::string pds = endpoint->get<std::string>();
        if (!starts_with(pds, "https://") && !starts_with(pds, "http://"))
        {
            throw ApiError("PDS service endpoint is not an HTTP URL", 0, "InvalidPds", false);
        }
        while (!pds.empty() && pds.back() == '/')
        {
            pds.pop_back();
        }
        return pds;
    }
    throw ApiError("DID document has no ATProto PDS service", 0, "NoPds", false);
}

} // namespace detail
} // namespace teal
