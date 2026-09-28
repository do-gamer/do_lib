#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#include "utils.h"
#include "flash_stuff.h"
#include "darkorbit.h"


// Interposes libc's dlopen via LD_PRELOAD; must stay exported with -fvisibility=hidden.
extern "C" __attribute__((visibility("default"))) void *dlopen(const char *filename, int flags)
{
    static auto *original = reinterpret_cast<void *(*)(const char *, int)>(dlsym(RTLD_NEXT, "dlopen"));

    auto *r = original(filename, flags);

    // Install flash hooks
    if (r && filename && strstr(filename, "libpepflashplayer.so"))
    {
        if (!flash_stuff::install())
        {
            utils::log("[!] Failed to install flash hooks\n");
        }
    }

    return r;
}

// Every browser process loads us; per-process load/unload lines only with TANOS_DEBUG=1
static const bool g_debug = getenv("TANOS_DEBUG") != nullptr;

int __attribute__((constructor)) lib_ctor ()
{
    if (g_debug)
        utils::log("[debug] do_lib loaded (pid {})\n", getpid());
    return 0;
}

int __attribute__((destructor)) lib_dtor()
{
    if (g_debug)
        utils::log("[debug] do_lib unloading (pid {})\n", getpid());

    // Only the flash process has hooks to restore; other browser processes (e.g. helpers
    // exiting after startup) have nothing to uninstall
    if (flash_stuff::installed())
    {
        Darkorbit::get().uninstall();
        flash_stuff::uninstall();
    }
    return 0;
}
