#include "../s2module.h"
#include "../checks/camplabels.h"
#include <intrin.h>
#include <stdint.h>
#include <wchar.h>

// Campaign map: while the camera moves, every settlement label on screen is laid out again each frame, and each one
// asks whether its settlement shows the unrest warning, which rebuilds the settlement's whole economy. The label uses
// the answer only for the player's own settlements (viewer edi == owner esi at the call), so for the others the call is
// skipped. The player's own reuse their last answer for up to 1/hz s (the ini); the engine's refresh of a single label
// always asks afresh, and its answer is the one reused.
#define TEST_CALL_RVA 0x112cc44
#define TEST_RVA 0x8b9cd0
#define REFRESH_CALL_RVA 0x11240a6
#define LAYOUT_RVA 0x112cad0

#define DEFAULT_HZ 30
#define SLOT_BITS 12
#define SLOTS (1u << SLOT_BITS)
#define PROBES 8
#define REPORT_MS 10000

typedef uintptr_t(__attribute__((thiscall)) *TestFn)(BYTE *settlement);
typedef void(__attribute__((thiscall)) *LayoutFn)(BYTE *labels, BYTE *label, float fade);

typedef struct {
    BYTE *settlement;
    uint64_t checked;
    BYTE answer;
} Entry;

// Plain counters: they are only written on the main thread.
typedef struct {
    LONG64 asked, reused, refreshed, differed;
} Stats;

__declspec(dllexport) volatile LONG labels_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats label_stats;
__declspec(dllexport) volatile LONG labels_skipped;
__declspec(dllexport) uint32_t labels_hz;
volatile LONG *camplabels_fix_enabled;
const BYTE *camplabels_engine_test;
uintptr_t(__attribute__((thiscall)) *camplabels_own_test)(BYTE *settlement);

void camplabels_stub(void);
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _camplabels_stub\n"
        "_camplabels_stub:\n"
        "  mov eax, dword ptr [_camplabels_fix_enabled]\n"
        "  cmp dword ptr [eax], 0\n"
        "  je 1f\n"
        "  cmp dword ptr [_labels_enabled], 0\n"
        "  je 1f\n"
        "  cmp edi, esi\n"
        "  je 2f\n"
        "  inc dword ptr [_labels_skipped]\n"
        "  xor eax, eax\n"
        "  ret\n"
        "1:\n"
        "  jmp dword ptr [_camplabels_engine_test]\n"
        "2:\n"
        "  jmp dword ptr [_camplabels_own_test]\n"
        ".att_syntax\n");

static const S2Api *api;
static LayoutFn engine_layout;
static Entry entries[SLOTS];
static uint64_t period;
static int refreshing;

// Every bit mixed: a multiplicative hash of settlements laid out at a fixed stride gives places in the period that bunch.
static uint32_t hash_of(const BYTE *settlement) {
    uint32_t hash = (uint32_t)(uintptr_t)settlement;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    return hash ^ hash >> 16;
}

// A settlement evicted from a full neighbourhood only asks the engine once more.
static Entry *entry_for(const BYTE *settlement) {
    uint32_t start = hash_of(settlement) >> (32 - SLOT_BITS);
    for (uint32_t probe = 0; probe < PROBES; probe++) {
        Entry *entry = &entries[(start + probe) & (SLOTS - 1)];
        if (entry->settlement == settlement || !entry->settlement) return entry;
    }
    return &entries[start];
}

static uintptr_t __attribute__((thiscall)) own_test(BYTE *settlement) {
    TestFn engine_test = (TestFn)(void *)camplabels_engine_test;
    if (!period || __readfsdword(0x24) != *api->main_thread) return engine_test(settlement);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uint64_t now = (uint64_t)counter.QuadPart;
    label_stats.asked++;
    Entry *entry = entry_for(settlement);
    if (refreshing || entry->settlement != settlement || now - entry->checked >= period) {
        // Each settlement keeps its own place in the period, so checks that fall due in one frame, as when the camera
        // starts moving, spread over the frames again instead of staying together.
        entry->checked = now - (now + hash_of(settlement)) % period;
        entry->settlement = settlement;
        entry->answer = (BYTE)engine_test(settlement);
        label_stats.refreshed += refreshing != 0;
        return entry->answer;
    }
    label_stats.reused++;
    if (verify && (BYTE)engine_test(settlement) != entry->answer) label_stats.differed++;
    return entry->answer;
}

static void __attribute__((thiscall)) refresh_label(BYTE *labels, BYTE *label, float fade) {
    BOOL main = __readfsdword(0x24) == *api->main_thread;
    refreshing += main;
    engine_layout(labels, label, fade);
    refreshing -= main;
}

static DWORD WINAPI report_loop(LPVOID unused) {
    (void)unused;
    Stats last = {0};
    LONG last_skipped = 0;
    for (;; Sleep(REPORT_MS)) {
        Stats now = label_stats;
        LONG skipped = labels_skipped;
        LONG64 asked = now.asked - last.asked;
        if (verify && (asked || skipped != last_skipped))
            api->log_line("camplabels: %lld label checks a second for other factions' settlements, skipped; %lld for the player's, "
                          "%.1f%% answered from the last check, %lld by a single-label refresh; %lld reused answers differed from a fresh check",
                          (LONG64)(skipped - last_skipped) * 1000 / REPORT_MS, asked * 1000 / REPORT_MS,
                          asked ? 100.0 * (double)(now.reused - last.reused) / (double)asked : 0.0, now.refreshed - last.refreshed,
                          now.differed - last.differed);
        last = now;
        last_skipped = skipped;
    }
    return 0;
}

// Whole numbers only: 7.5 or -1 keep the default rather than meaning 7 or off. FALSE when the ini's value was refused.
static BOOL read_hz(void) {
    labels_hz = DEFAULT_HZ;
    wchar_t path[MAX_PATH], text[16] = L"";
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)read_hz, &self);
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return TRUE;
    for (int up = 0; up < 2; up++) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) *slash = 0;
    }
    wcscat(path, L"\\s2prof.ini");
    GetPrivateProfileStringW(L"camplabels", L"hz", L"", text, sizeof text / sizeof *text, path);
    if (!text[0]) return TRUE;
    wchar_t *end;
    unsigned long value = wcstoul(text, &end, 10);
    if (text[0] < L'0' || text[0] > L'9' || *end) return FALSE;
    labels_hz = value;
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
    const char *error = S2_CHECK_ENGINE(host, camplabels_checks);
    if (error) return error;
    api = host;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    camplabels_fix_enabled = api->fix_enabled;
    camplabels_engine_test = api->engine + TEST_RVA;
    camplabels_own_test = own_test;
    engine_layout = (LayoutFn)(void *)(api->engine + LAYOUT_RVA);
    period = 0;
    if (labels_hz) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        period = (uint64_t)frequency.QuadPart / labels_hz;
        if (!period) period = 1;
    }
    error = patch_call(api->engine + TEST_CALL_RVA, camplabels_engine_test, (const BYTE *)(void *)camplabels_stub);
    if (!error && period) error = patch_call(api->engine + REFRESH_CALL_RVA, api->engine + LAYOUT_RVA, (const BYTE *)(void *)refresh_label);
    if (error) return error;
    if (!whole_number) api->log_line("camplabels: hz in s2prof.ini is not a whole number; using %d", DEFAULT_HZ);
    if (!period) api->log_line("camplabels: hz=0, so the player's own settlements ask every time; the others are still skipped");
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("camplabels", &labels_enabled, NULL);
    api->register_switch("camplabelsverify", &verify, NULL);
    return NULL;
}
