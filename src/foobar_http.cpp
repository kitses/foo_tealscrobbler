#include "foobar_http.h"

#include <cctype>
#include <stdexcept>
#include <string>

FoobarHttpTransport::FoobarHttpTransport(abort_callback &abort) : abort_(abort)
{
}

teal::HttpResponse FoobarHttpTransport::send(const teal::HttpRequest &request)
{
    http_request::ptr http = http_client::get()->create_request(request.method.c_str());
    for (const auto &[name, value] : request.headers)
    {
        http->add_header(name.c_str(), value.c_str());
    }
    if (request.method == "POST")
    {
        http_request_post_v2::ptr post;
        if (!http->service_query_t(post))
        {
            throw std::runtime_error("The foobar2000 HTTP client does not support raw POST bodies");
        }
        if (!request.body.empty())
        {
            post->set_post_data(request.body.data(), request.body.size(), "application/json");
        }
    }

    file::ptr stream = http->run_ex(request.url.c_str(), abort_);

    http_reply::ptr reply;
    if (!stream->service_query_t(reply))
    {
        throw std::runtime_error("The foobar2000 HTTP client returned no reply information");
    }
    pfc::string8 status_line;
    reply->get_status(status_line);
    const std::string status_text = status_line.get_ptr();
    int status = 0;
    for (std::size_t i = 0; i + 2 < status_text.size(); ++i)
    {
        if (std::isdigit(static_cast<unsigned char>(status_text[i])) &&
            std::isdigit(static_cast<unsigned char>(status_text[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(status_text[i + 2])))
        {
            status = (status_text[i] - '0') * 100 + (status_text[i + 1] - '0') * 10 + (status_text[i + 2] - '0');
            if (status >= 100 && status <= 599)
            {
                break;
            }
            status = 0;
        }
    }

    std::string body;
    char buffer[64 * 1024];
    for (;;)
    {
        const t_size read = stream->read(buffer, sizeof(buffer), abort_);
        if (read == 0)
        {
            break;
        }
        body.append(buffer, read);
    }
    return teal::HttpResponse{status, std::move(body)};
}
