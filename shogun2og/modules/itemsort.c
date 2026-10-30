#include "../s2module.h"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../renderitem.h"
#include "../checks/itemsort.h"

// The Variant node's item sorts (B and C re-sorted in every record pass) become stable radix sorts leaving an ordered
// list untouched. Each recursive call of the engine's sort fully sorts its range: replacing the outermost is enough.
#define SORT_A_RVA 0x14aba00
#define SORT_B_RVA 0x14ab800
#define SORT_C_RVA 0x14abc00

typedef void (*SortFn)(uint8_t *first, uint8_t *last, int ideal, int predicate);

typedef struct {
    LONG64 calls, items, ticks, cycles, permute_cycles, presorted, moved, merged, allocation_failures;
} ListStats;

__declspec(dllexport) volatile ListStats list_stats[LISTS];
__declspec(dllexport) volatile LONG sort_enabled = 1;
// Orders list C's ties by instance texture (+0x124): each change of it opens a new instancing group.
__declspec(dllexport) volatile LONG tie_grouping = 1;
// Merges a sorted list's short unsorted tail instead of radix-sorting the list.
__declspec(dllexport) volatile LONG tail_merge = 1;
// Counting-sorts over the distinct keys when there are at most FEW_KEYS_MAX.
__declspec(dllexport) volatile LONG few_keys = 1;

static const BYTE sort_prologue[] = {0x81, 0xec, 0x40, 0x01, 0x00, 0x00};
static const char *const list_names[LISTS] = {"list A (prepare)", "list B (record)", "list C (record)"};
static const S2Api *api;
static SortFn original_sorts[LISTS];
static __thread int sort_depth;

static void sort_list(ListKind list, uint8_t *first, uint8_t *last, int ideal, int predicate) {
    if (sort_depth || !*api->fix_enabled || !sort_enabled) {
        sort_depth++;
        original_sorts[list](first, last, ideal, predicate);
        sort_depth--;
        return;
    }
    LARGE_INTEGER started, ended;
    QueryPerformanceCounter(&started);
    uint64_t cycles = __builtin_ia32_rdtsc(), permuting = permute_cycles;
    int merged = 0;
    long long moved = sort_render_items(list, list == LIST_C && tie_grouping, first, last, tail_merge, few_keys, &merged);
    permuting = permute_cycles - permuting;
    cycles = __builtin_ia32_rdtsc() - cycles;
    volatile ListStats *stats = &list_stats[list];
    if (moved < 0) {
        InterlockedIncrement64(&stats->allocation_failures);
        sort_depth++;
        original_sorts[list](first, last, ideal, predicate);
        sort_depth--;
        moved = 0;
    }
    QueryPerformanceCounter(&ended);
    InterlockedIncrement64(&stats->calls);
    InterlockedAdd64(&stats->items, (last - first) / RENDER_ITEM_SIZE);
    InterlockedAdd64(&stats->ticks, ended.QuadPart - started.QuadPart);
    InterlockedAdd64(&stats->cycles, (LONG64)cycles);
    InterlockedAdd64(&stats->permute_cycles, (LONG64)permuting);
    InterlockedAdd64(&stats->moved, moved);
    if (!moved) InterlockedIncrement64(&stats->presorted);
    if (merged) InterlockedIncrement64(&stats->merged);
}

static void sort_a_hook(uint8_t *first, uint8_t *last, int ideal, int predicate) {
    sort_list(LIST_A, first, last, ideal, predicate);
}

static void sort_b_hook(uint8_t *first, uint8_t *last, int ideal, int predicate) {
    sort_list(LIST_B, first, last, ideal, predicate);
}

static void sort_c_hook(uint8_t *first, uint8_t *last, int ideal, int predicate) {
    sort_list(LIST_C, first, last, ideal, predicate);
}

static void report(BOOL start) {
    if (start) {
        memset((void *)list_stats, 0, sizeof list_stats);
        return;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    for (int list = 0; list < LISTS; list++) {
        volatile ListStats *stats = &list_stats[list];
        api->log_line("  itemsort %s: %lld sorts while on, %.0f items each, %lld already in order, %lld tails merged, %.0f items moved each, %.3f ms in "
                      "all, %.0f%% of it moving items%s",
                      list_names[list], stats->calls, stats->calls ? (double)stats->items / stats->calls : 0.0, stats->presorted, stats->merged,
                      stats->calls ? (double)stats->moved / stats->calls : 0.0, 1000.0 * stats->ticks / frequency.QuadPart,
                      stats->cycles ? 100.0 * stats->permute_cycles / stats->cycles : 0.0,
                      stats->allocation_failures ? "; some fell back (allocation failed)" : "");
    }
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, itemsort_checks);
    if (mismatch) return mismatch;
    api = host;
    const DWORD rvas[LISTS] = {SORT_A_RVA, SORT_B_RVA, SORT_C_RVA};
    const void *hooks[LISTS] = {(const void *)sort_a_hook, (const void *)sort_b_hook, (const void *)sort_c_hook};
    // All three prologues are checked before any is patched, so a mismatched build is left untouched.
    for (int list = 0; list < LISTS; list++)
        if (!s2_prologue_ready(api->engine + rvas[list], sort_prologue, sizeof sort_prologue)) return "a list sort differs from the analysed build";
    for (int list = 0; list < LISTS; list++) {
        const char *error = api->hook(api->engine + rvas[list], sort_prologue, sizeof sort_prologue, hooks[list], (void **)&original_sorts[list]);
        if (error) return error;
    }
    api->register_switch("itemsort", &sort_enabled, report);
    api->register_switch("tiegroup", &tie_grouping, report);
    api->register_switch("tailmerge", &tail_merge, report);
    api->register_switch("fewkeys", &few_keys, report);
    api->register_switch("inlinecopy", &item_copy_inline, NULL);
    return NULL;
}
