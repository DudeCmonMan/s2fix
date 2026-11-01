#include "../s2module.h"
#include "../checks/campthrottle.h"
#include <intrin.h>
#include <stdint.h>
#include <wchar.h>

// Campaign map: every frame, each building slot of the player's settlements on screen asks whether it can build
// something now. Each slot asks at most hz times a second (the ini) and reuses its last answer in between, so an answer
// can be up to 1/hz s old. It patches the check's two calls, so it wraps any hook on the check itself.
#define BUILD_CHECK_RVA 0x7818d0
#define SLOT_CALL_RVA 0x89fcc1
#define SETTLEMENT_CALL_RVA 0x89fd66

#define DEFAULT_HZ 30
#define SLOT_BITS 12
#define SLOTS (1u << SLOT_BITS)
#define PROBES 8
#define REPORT_MS 10000

typedef uintptr_t(__attribute__((thiscall)) *CheckFn)(BYTE *context);

typedef struct {
    BYTE *context;
    uint64_t checked;
    BYTE answer;
} Entry;

// Plain counters: they are only written on the main thread.
typedef struct {
    LONG64 asks, reused, differed;
} Stats;

__declspec(dllexport) volatile LONG throttle_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats throttle_stats;
__declspec(dllexport) uint32_t throttle_hz;

static const S2Api *api;
static CheckFn engine_check;
static Entry entries[SLOTS];
static uint64_t period;

static uint32_t hash_of(const BYTE *context) {
    return (uint32_t)((uintptr_t)context >> 3) * 2654435761u;
}

// A slot evicted from a full neighbourhood only asks the engine once more.
static Entry *entry_for(const BYTE *context) {
    uint32_t start = hash_of(context) >> (32 - SLOT_BITS);
    for (uint32_t probe = 0; probe < PROBES; probe++) {
        Entry *entry = &entries[(start + probe) & (SLOTS - 1)];
        if (entry->context == context || !entry->context) return entry;
    }
    return &entries[start];
}

static uintptr_t __attribute__((thiscall)) throttled_check(BYTE *context) {
    if (!throttle_enabled || !*api->fix_enabled || __readfsdword(0x24) != *api->main_thread) return engine_check(context);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uint64_t now = (uint64_t)counter.QuadPart;
    throttle_stats.asks++;
    Entry *entry = entry_for(context);
    if (entry->context != context) {
        entry->context = context;
        entry->answer = (BYTE)engine_check(context);
        // Starting each slot part-way through its period spreads the checks over the frames instead of one in every few.
        entry->checked = now - hash_of(context) % period;
        return entry->answer;
    }
    if (now - entry->checked >= period) {
        entry->answer = (BYTE)engine_check(context);
        entry->checked = now;
        return entry->answer;
    }
    throttle_stats.reused++;
    if (verify && (BYTE)engine_check(context) != entry->answer) throttle_stats.differed++;
    return entry->answer;
}

static DWORD WINAPI report_loop(LPVOID unused) {
    (void)unused;
    Stats last = {0};
    for (;; Sleep(REPORT_MS)) {
        Stats now = throttle_stats;
        LONG64 asks = now.asks - last.asks;
        if (verify && asks)
            api->log_line("campthrottle: %lld asks a second, %.1f%% answered from the last check; %lld of those differed from a fresh check", asks * 1000 / REPORT_MS,
                          100.0 * (double)(now.reused - last.reused) / (double)asks, now.differed - last.differed);
        last = now;
    }
    return 0;
}

// Whole numbers only: 7.5 or -1 keep the default rather than meaning 7 or off. FALSE when the ini's value was refused.
static BOOL read_hz(void) {
    throttle_hz = DEFAULT_HZ;
    wchar_t path[MAX_PATH], text[16] = L"";
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)read_hz, &self);
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return TRUE;
    for (int up = 0; up < 2; up++) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) *slash = 0;
    }
    wcscat(path, L"\\s2prof.ini");
    GetPrivateProfileStringW(L"campthrottle", L"hz", L"", text, sizeof text / sizeof *text, path);
    if (!text[0]) return TRUE;
    wchar_t *end;
    unsigned long value = wcstoul(text, &end, 10);
    if (text[0] < L'0' || text[0] > L'9' || *end) return FALSE;
    throttle_hz = value;
    return TRUE;
}

static const char *patch_call(BYTE *site, const BYTE *from, const BYTE *to) {
    BYTE expected[5] = {0xe8}, replacement[5] = {0xe8};
    *(LONG *)(expected + 1) = (LONG)(from - (site + 5));
    *(LONG *)(replacement + 1) = (LONG)(to - (site + 5));
    return api->patch(site, expected, replacement, sizeof expected);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    BOOL whole_number = read_hz();
    if (!throttle_hz) return "off ([campthrottle] hz=0 in s2prof.ini)";
    const char *error = S2_CHECK_ENGINE(host, campthrottle_checks);
    if (error) return error;
    api = host;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    period = (uint64_t)frequency.QuadPart / throttle_hz;
    if (!period) period = 1;
    BYTE *check = api->engine + BUILD_CHECK_RVA;
    engine_check = (CheckFn)(void *)check;
    error = patch_call(api->engine + SLOT_CALL_RVA, check, (const BYTE *)(void *)throttled_check);
    if (!error) error = patch_call(api->engine + SETTLEMENT_CALL_RVA, check, (const BYTE *)(void *)throttled_check);
    if (error) return error;
    if (!whole_number) api->log_line("campthrottle: hz in s2prof.ini is not a whole number; using %d", DEFAULT_HZ);
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("campthrottle", &throttle_enabled, NULL);
    api->register_switch("campthrottleverify", &verify, NULL);
    return NULL;
}
