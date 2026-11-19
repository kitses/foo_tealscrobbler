#pragma once

#include "teal_client.h"

#include <SDK/foobar2000.h>

class FoobarHttpTransport final : public teal::HttpTransport {
public:
    explicit FoobarHttpTransport(abort_callback& abort);

    teal::HttpResponse send(const teal::HttpRequest& request) override;

private:
    abort_callback& abort_;
};
