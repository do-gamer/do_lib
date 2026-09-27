#include <dlfcn.h>
#include <cstring>

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

int __attribute__((constructor)) lib_ctor ()
{
    utils::log("[+] Loading shared library do_lib\n");
    return 0;
}

int __attribute__((destructor)) lib_dtor()
{
    utils::log("[+] Unloading shared library do_lib\n");
    Darkorbit::get().uninstall();
    flash_stuff::uninstall();
    return 0;
}
