#include "../s2module.h"

// With the threads on one CCD, the overhaul's workers waited for CPUs behind idle-spinning helpers while main waited
// for them; this sets the flags the pools read: workers above normal priority, and every pool spinning briefly.
#define POLL_MS 10
// The host loads the fixes one after another, so the pools are looked up until all are found or this much time passed.
#define LOOKUP_MS 30000

__declspec(dllexport) volatile LONG contention_enabled = 1;

static const struct {
    const char *module, *flag;
} pools[] = {{"overhaul.dll", "overhaul_worker_priority"}, {"walksplit.dll", "short_spin_enabled"}, {"batchsplit.dll", "short_spin_enabled"}};
#define POOL_COUNT (int)(sizeof pools / sizeof *pools)

static const S2Api *api;

static DWORD WINAPI follow_switch(LPVOID unused) {
    (void)unused;
    volatile LONG *flags[POOL_COUNT] = {0};
    int found = 0;
    BOOL logged = FALSE;
    for (DWORD waited = 0;; waited += POLL_MS, Sleep(POLL_MS)) {
        for (int i = 0; i < POOL_COUNT && !logged; i++) {
            if (flags[i]) continue;
            HMODULE module = GetModuleHandleA(pools[i].module);
            if ((flags[i] = module ? (volatile LONG *)(void *)GetProcAddress(module, pools[i].flag) : NULL)) found++;
        }
        if (!logged && (found == POOL_COUNT || waited >= LOOKUP_MS)) {
            api->log_line("contention: %d of %d helper pools found (overhaul %s, walksplit %s, batchsplit %s)", found, POOL_COUNT, flags[0] ? "yes" : "no",
                          flags[1] ? "yes" : "no", flags[2] ? "yes" : "no");
            logged = TRUE;
            if (!found) return 0;
        }
        for (int i = 0; i < POOL_COUNT; i++)
            if (flags[i] && *flags[i] != contention_enabled) *flags[i] = contention_enabled;
    }
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
