#include <dlfcn.h>
#include <stdio.h>

typedef int (*probe_fn_t)(void);

static int run_probe(const char* path, const char* symbol, int expected) {
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        printf("dynlink-dlopen: FAIL open %s: %s\n", path, dlerror());
        return 1;
    }

    dlerror();
    probe_fn_t fn = (probe_fn_t)dlsym(handle, symbol);
    const char* err = dlerror();
    if (err) {
        printf("dynlink-dlopen: FAIL dlsym %s: %s\n", symbol, err);
        return 1;
    }

    int got = fn();
    printf("dynlink-dlopen: %s=%d\n", symbol, got);
    if (got != expected) {
        printf("dynlink-dlopen: FAIL expected=%d got=%d\n", expected, got);
        return 1;
    }
    return 0;
}

int main(void) {
    if (run_probe("/lib/libdyn_plugin.so", "dyn_plugin_total", 1043) != 0) {
        return 1;
    }
    if (run_probe("/lib/libdyn_cpp.so", "dyn_cpp_probe", 42) != 0) {
        return 1;
    }
    printf("dynlink-dlopen: PASS\n");
    return 0;
}
