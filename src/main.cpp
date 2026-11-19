#include "scrobbler.h"
#include "version.h"

#include <SDK/foobar2000.h>

DECLARE_COMPONENT_VERSION("Tealscrobbler", TEAL_VERSION,
                          "Tealscrobbler\n\n"
                          "https://teal.fm\n"
                          "https://github.com/kitses/foo_tealscrobbler\n\n"
                          "Yarked out by Kit Sylvester\n"
                          "Ripped off from foo_audioscrobbler by Florian Heidenreich");

VALIDATE_COMPONENT_FILENAME("foo_tealscrobbler.dll");

namespace
{

class teal_initquit : public initquit
{
  public:
    void on_init() override
    {
        if (scrobbler *app = scrobbler::instance())
        {
            app->start();
        }
    }

    void on_quit() override
    {
        if (scrobbler *app = scrobbler::instance())
        {
            app->shutdown();
        }
    }
};

} // namespace

static initquit_factory_t<teal_initquit> g_teal_initquit;
