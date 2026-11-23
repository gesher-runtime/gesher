// Load the supplier without a static or DLL pthread import, so FreeLibrary
// exercises real process-detach cleanup in a process that stays alive.
#include <windows.h>
#include <pthread.h>
#include <stdio.h>

struct clock_api {
    pthread_t (*self)(void);
    int (*acquire)(pthread_t, clockid_t *);
};

static DWORD WINAPI acquire_on_native_thread(void *context)
{
    const struct clock_api *api = context;
    clockid_t clock = 0;
    return api->acquire(api->self(), &clock) == 0 && clock < 0 ? 0 : 1;
}

int main(void)
{
    const wchar_t supplier[] = L"libwinpthread-1.dll";
    DWORD before, after = 0;
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
        };
        HANDLE target = NULL;
        if (api.self && api.acquire)
            target = CreateThread(NULL, 0, acquire_on_native_thread, &api, 0, NULL);
        DWORD code = 1;
        if (!target || WaitForSingleObject(target, 15000) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(target, &code) || code != 0 || !CloseHandle(target)) {
            fputs("FAIL N28 unload: could not acquire a real target clock and retire its thread\n", stderr);
            return 1;
        }
        // Leave the signaled clock slot in the supplier. A later clock read
        // would reap it and would no longer test library detach.
        if (!FreeLibrary(module) || GetModuleHandleW(supplier)) {
            fputs("FAIL N28 unload: supplier did not unload\n", stderr);
            return 1;
        }
    }
    if (!GetProcessHandleCount(GetCurrentProcess(), &after) || before != after) {
        fprintf(stderr, "FAIL N28 unload: process handles changed from %lu to %lu\n",
                (unsigned long)before, (unsigned long)after);
        return 1;
    }
    puts("PASS N28 shared supplier: unload released all retained clock handles");
    return 0;
}
