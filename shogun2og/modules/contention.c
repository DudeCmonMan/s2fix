#include "../s2module.h"

// With the threads on one CCD, the overhaul's workers waited for CPUs behind idle-spinning helpers while main waited
// for them; this sets the flags the pools read: workers above normal priority, and every pool spinning briefly.
#define POLL_MS 10

__declspec(dllexport) volatile LONG contention_enabled = 1;

static const struct {
    const char *module, *flag;
} pools[] = {{"overhaul.dll", "overhaul_worker_priority"}, {"walksplit.dll", "short_spin_enabled"}, {"batchsplit.dll", "short_spin_enabled"}};
#define POOL_COUNT (int)(sizeof pools / sizeof *pools)

static const S2Api *api;

static DWORD WINAPI follow_switch(LPVOID unused) {
    (void)unused;
    // Every fix is installed before the first frame, whatever order the host loaded them in.
    while (!*api->main_thread) Sleep(POLL_MS);
    volatile LONG *flags[POOL_COUNT];
    int found = 0;
    for (int i = 0; i < POOL_COUNT; i++) {
        HMODULE module = GetModuleHandleA(pools[i].module);
        flags[i] = module ? (volatile LONG *)(void *)GetProcAddress(module, pools[i].flag) : NULL;
        found += flags[i] != NULL;
    }
    api->log_line("contention: %d of %d helper pools found (overhaul %s, walksplit %s, batchsplit %s)", found, POOL_COUNT, flags[0] ? "yes" : "no",
                  flags[1] ? "yes" : "no", flags[2] ? "yes" : "no");
    if (!found) return 0;
    for (;; Sleep(POLL_MS))
        for (int i = 0; i < POOL_COUNT; i++)
            if (flags[i] && *flags[i] != contention_enabled) *flags[i] = contention_enabled;
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    HANDLE thread = CreateThread(NULL, 0, follow_switch, NULL, 0, NULL);
    if (!thread) return "CreateThread failed";
    CloseHandle(thread);
    api->register_switch("contention", &contention_enabled, NULL);
    return NULL;
}
