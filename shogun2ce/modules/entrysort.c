#include "../s2module.h"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../entrysort.h"

// The engine's per-entry std::sort (MSVC _Sort_unchecked) over object pointers. Its comparator chases three pointers and
// recomputes both distances on every comparison; entrysort.h computes each object's keys once. Every recursive call of the
// original fully sorts its range, so replacing any call is equivalent.
#define ENTRY_SORT_RVA 0x331d50

typedef struct {
    LONG64 calls, objects, ticks, allocation_failures;
} EntrySortStats;

__declspec(dllexport) volatile EntrySortStats entry_sort_stats;

static const BYTE entry_sort_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};
static void (*original_entry_sort)(BYTE **first, BYTE **last, int64_t ideal, const float *camera);
static volatile LONG *fix_enabled;

static void entry_sort_hook(BYTE **first, BYTE **last, int64_t ideal, const float *camera) {
    if (!*fix_enabled) {
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

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    fix_enabled = api->fix_enabled;
    return api->hook(api->engine + ENTRY_SORT_RVA, entry_sort_prologue, sizeof entry_sort_prologue, (const void *)entry_sort_hook,
                     (void **)&original_entry_sort);
}
