// Load the supplier without a static or DLL pthread import, so FreeLibrary
// exercises real process-detach cleanup in a process that stays alive.
#include <windows.h>
#include <pthread.h>
#include <stdio.h>

struct clock_api {
    pthread_t (*self)(void);
    int (*acquire)(pthread_t, clockid_t *);
    BOOL measure_clock;
};

static DWORD WINAPI acquire_on_native_thread(void *context)
{
    const struct clock_api *api = context;
    clockid_t clock = 0;
    pthread_t self = api->self();
    if (!self) return 1;
    return !api->measure_clock || (api->acquire(self, &clock) == 0 && clock < 0) ? 0 : 1;
}

static int run_cycles(BOOL measure_clock, long *delta)
{
    const wchar_t supplier[] = L"libwinpthread-1.dll";
    DWORD before, after;
    if (GetModuleHandleW(supplier) ||
        !GetProcessHandleCount(GetCurrentProcess(), &before)) {
        fputs("FAIL N28 unload: invalid initial module or handle count\n", stderr);
        return 1;
    }
    for (unsigned i = 0; i < 8; ++i) {
        HMODULE module = LoadLibraryExW(supplier, NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
        if (!module) {
            fputs("FAIL N28 unload: could not load adjacent supplier\n", stderr);
            return 1;
        }
        struct clock_api api = {
            (pthread_t (*)(void))(void *)GetProcAddress(module, "pthread_self"),
            (int (*)(pthread_t, clockid_t *))(void *)GetProcAddress(module, "pthread_getcpuclockid"),
            measure_clock,
        };
        HANDLE target = NULL;
        if (api.self && api.acquire)
            target = CreateThread(NULL, 0, acquire_on_native_thread, &api, 0, NULL);
        DWORD code = 1;
        if (!target || WaitForSingleObject(target, 15000) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(target, &code) || code != 0 || !CloseHandle(target)) {
            fputs("FAIL N28 unload: native target did not complete the selected case\n", stderr);
            return 1;
        }
        // Leave the signaled clock slot in the supplier. A later clock read
        // would reap it and would no longer test library detach.
        if (!FreeLibrary(module) || GetModuleHandleW(supplier)) {
            fputs("FAIL N28 unload: supplier did not unload\n", stderr);
            return 1;
        }
    }
    if (!GetProcessHandleCount(GetCurrentProcess(), &after)) {
        fputs("FAIL N28 unload: invalid final handle count\n", stderr);
        return 1;
    }
    *delta = (long)after - (long)before;
    return 0;
}

int main(void)
{
    long reference, measured;
    // Both groups attach and retire native threads. Compare handle deltas to
    // isolate the clock table from the supplier's existing non-clock lifetime.
    if (run_cycles(FALSE, &reference) || run_cycles(TRUE, &measured)) return 1;
    printf("N28 unload handle deltas: without clock %ld, with clock %ld\n", reference, measured);
    if (reference != measured) {
        fputs("FAIL N28 unload: the clock path changed handle balance\n", stderr);
        return 1;
    }
    puts("PASS N28 shared supplier: unload released all retained clock handles");
    return 0;
}
