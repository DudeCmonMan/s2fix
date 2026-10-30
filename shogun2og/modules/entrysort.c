#include "../s2module.h"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../entrysort.h"
#include "../checks/entrysort.h"

// The engine's per-entry sort recomputes both distances on every comparison; entrysort.h computes each object's keys
// once. Every recursive call fully sorts its range, so replacing any call is equivalent.
#define ENTRY_SORT_RVA 0x23abe0

typedef struct {
    LONG64 calls, objects, ticks, allocation_failures;
} EntrySortStats;

__declspec(dllexport) volatile EntrySortStats entry_sort_stats;
__declspec(dllexport) volatile LONG sort_enabled = 1;

static const BYTE entry_sort_prologue[] = {0x83, 0xec, 0x08, 0x53, 0x55};
static void (*original_entry_sort)(BYTE **first, BYTE **last, int ideal, const float *camera);
static const S2Api *api;

static void entry_sort_hook(BYTE **first, BYTE **last, int ideal, const float *camera) {
    if (!*api->fix_enabled || !sort_enabled) {
        original_entry_sort(first, last, ideal, camera);
        return;
    }
    LARGE_INTEGER started, ended;
    QueryPerformanceCounter(&started);
    if (!sort_entries(first, last - first, camera)) {
        InterlockedIncrement64(&entry_sort_stats.allocation_failures);
        original_entry_sort(first, last, ideal, camera);
        return;
    }
    QueryPerformanceCounter(&ended);
    InterlockedIncrement64(&entry_sort_stats.calls);
    InterlockedAdd64(&entry_sort_stats.objects, last - first);
    InterlockedAdd64(&entry_sort_stats.ticks, ended.QuadPart - started.QuadPart);
}

static void report(BOOL start) {
    if (start) {
        memset((void *)&entry_sort_stats, 0, sizeof entry_sort_stats);
        return;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    api->log_line("  entrysort: %lld sorts of %lld objects in %.2f ms while on; %lld fell back", entry_sort_stats.calls, entry_sort_stats.objects,
                  1000.0 * entry_sort_stats.ticks / frequency.QuadPart, entry_sort_stats.allocation_failures);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, entrysort_checks);
    if (mismatch) return mismatch;
    api = host;
    const char *error = api->hook(api->engine + ENTRY_SORT_RVA, entry_sort_prologue, sizeof entry_sort_prologue, (const void *)entry_sort_hook,
                                  (void **)&original_entry_sort);
    if (!error) api->register_switch("entrysort", &sort_enabled, report);
    return error;
}
