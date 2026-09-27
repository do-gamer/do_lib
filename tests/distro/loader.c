// Loads DarkTanos.so (resolving a JNI entry point) and checks that libdo_lib.so's
// dlopen interposer works when preloaded. Built against an old glibc so it runs everywhere.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("FAIL DarkTanos.so: %s\n", dlerror()); return 1; }
    if (!dlsym(h, "Java_eu_darkbot_api_DarkTanos_getVersion")) { printf("FAIL JNI symbol missing\n"); return 1; }
    int (*get_version)(void *, void *) = (int (*)(void *, void *))dlsym(h, "Java_eu_darkbot_api_DarkTanos_getVersion");
    printf("OK   DarkTanos.so loaded, api version %d\n", get_version(NULL, NULL));

    // with LD_PRELOAD=libdo_lib.so this dlopen goes through the interposer
    const char *preload = getenv("LD_PRELOAD");
    void *m = dlopen("libm.so.6", RTLD_NOW);
    if (!m) { printf("FAIL dlopen through interposer: %s\n", dlerror()); return 1; }
    printf("OK   libdo_lib.so %s, dlopen(libm) works\n", preload ? "preloaded" : "not preloaded");
    return 0;
}
