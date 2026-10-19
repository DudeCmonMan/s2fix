#include "../s2module.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The record step's per-queue list sort (std::sort instantiation sub_181aff120, called only from sub_181b15ab0) orders
// render items by mesh, word + 0x00, word + 0x98, float + 0x94 and material, but not by the state nibble at + 0xa0. With
// instancing on, the record loop after it starts a new instancing group whenever that nibble differs from the previous
// item's, so tied items with alternating nibbles were drawn in groups of ~1.5. std::sort leaves the order of tied items
// unspecified, and the list's input order already varies with how TBB filled the per-thread lists, so this regroups each
// run of items whose five key fields are bit-identical by nibble, keeping their relative order otherwise. Each run starts
// with the nibble of the item before it, so a boundary where only word + 0x00 changes does not break a group either.
#define QUEUE_SORT_RVA 0x1aff120
#define FRAME_STAMP_RVA 0x2ead830
#define ITEM_SIZE 0xb0
#define NIBBLES 16
#define VERIFIED_SKIPS 64

typedef void (*QueueSortFn)(BYTE *first, BYTE *last, int64_t ideal, uint64_t predicate);

typedef struct {
    LONG64 sorts, items, regrouped_runs, moved_items;
} TieGroupStats;

typedef struct {
    LONG64 frames, sorts, skipped_sorts, skipped_regroups, ticks;
} SortOnceStats;

__declspec(dllexport) volatile TieGroupStats tie_group_stats;
// 0 leaves the engine's tie order; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG enabled = 1;
// List C is sorted in each of a frame's six records; after the first it is already in order, and the engine's introsort is
// then the identity (UP2 4.6), so sort_once skips the sort and the regroup when a scan finds nothing to change.
__declspec(dllexport) volatile LONG sort_once = 1;

static const BYTE queue_sort_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57};
static const S2Api *api;
static QueueSortFn original_queue_sort;
static volatile LONG *fix_enabled;
static volatile uint64_t *frame_stamp;
static uint64_t last_stamp;
static SortOnceStats sort_once_stats[2];
static LONG64 verified_skips, differing_skips;
// The engine's sort recurses through the hooked entry; only the outermost call regroups.
static __thread int sort_depth;
static __thread BYTE *scratch;
static __thread size_t scratch_items;

static int same_keys(const BYTE *a, const BYTE *b) {
    return !memcmp(a + 0x78, b + 0x78, 8) && !memcmp(a, b, 8) && !memcmp(a + 0x94, b + 0x94, 12) && !memcmp(a + 0x80, b + 0x80, 4);
}

static DWORD nibble(const BYTE *item) {
    return *(const DWORD *)(item + 0xa0) & 0xf;
}

// The engine's comparator: unsigned compares, and equal or unordered values fall through to the group.
static int list_c_less(const BYTE *a, const BYTE *b) {
    uint64_t mesh_a = *(const uint64_t *)(a + 0x78), mesh_b = *(const uint64_t *)(b + 0x78);
    if (mesh_a != mesh_b) return mesh_a < mesh_b;
    uint64_t pose_a = *(const uint64_t *)a, pose_b = *(const uint64_t *)b;
    if (pose_a != pose_b) return pose_a < pose_b;
    uint64_t block_a = *(const uint64_t *)(a + 0x98), block_b = *(const uint64_t *)(b + 0x98);
    if (block_a != block_b) return block_a < block_b;
    float value_a = *(const float *)(a + 0x94), value_b = *(const float *)(b + 0x94);
    if (value_a < value_b) return 1;
    if (value_a > value_b) return 0;
    return *(const uint32_t *)(a + 0x80) < *(const uint32_t *)(b + 0x80);
}

// A NaN breaks the comparator's ordering, and the introsort may then move items of a non-decreasing range.
static int already_sorted(const BYTE *first, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const BYTE *item = first + i * ITEM_SIZE;
        if (isnan(*(const float *)(item + 0x94)) || (i && list_c_less(item, item - ITEM_SIZE))) return 0;
    }
    return 1;
}

static int already_grouped(const BYTE *first, size_t count, DWORD lead) {
    int last_rank = -1;
    for (size_t i = 0; i < count; i++) {
        DWORD value = nibble(first + i * ITEM_SIZE);
        int rank = value == lead ? -1 : (int)value;
        if (rank < last_rank) return 0;
        last_rank = rank;
    }
    return 1;
}

static void verify_skip(const BYTE *first, size_t bytes, int64_t ideal, uint64_t predicate) {
    BYTE *copy = malloc(bytes);
    memcpy(copy, first, bytes);
    original_queue_sort(copy, copy + bytes, ideal, predicate);
    if (memcmp(copy, first, bytes) && !differing_skips++) api->log_line("sortonce: the engine sort reordered a list the scan found sorted");
    verified_skips++;
    free(copy);
}

// A stable counting sort of one tie run by nibble, with the lead nibble first.
static void regroup(BYTE *first, size_t count, DWORD lead) {
    size_t offsets[NIBBLES] = {0};
    for (size_t i = 0; i < count; i++) offsets[nibble(first + i * ITEM_SIZE)]++;
    size_t total = offsets[lead];
    offsets[lead] = 0;
    for (size_t n = 0; n < NIBBLES; n++) {
        if (n == lead) continue;
        size_t size = offsets[n];
        offsets[n] = total;
        total += size;
    }
    for (size_t i = 0; i < count; i++) {
        const BYTE *item = first + i * ITEM_SIZE;
        memcpy(scratch + offsets[nibble(item)]++ * ITEM_SIZE, item, ITEM_SIZE);
    }
    memcpy(first, scratch, count * ITEM_SIZE);
}

static void queue_sort_hook(BYTE *first, BYTE *last, int64_t ideal, uint64_t predicate) {
    if (sort_depth || !enabled || !*fix_enabled) {
        original_queue_sort(first, last, ideal, predicate);
        return;
    }
    LARGE_INTEGER started, finished;
    QueryPerformanceCounter(&started);
    int mode = sort_once != 0;
    SortOnceStats *stats = &sort_once_stats[mode];
    if (*frame_stamp != last_stamp) {
        last_stamp = *frame_stamp;
        stats->frames++;
    }
    stats->sorts++;
    size_t count = (size_t)(last - first) / ITEM_SIZE;
    sort_depth = 1;
    if (mode && already_sorted(first, count)) {
        stats->skipped_sorts++;
        if (verified_skips < VERIFIED_SKIPS) verify_skip(first, last - first, ideal, predicate);
    } else {
        original_queue_sort(first, last, ideal, predicate);
    }
    sort_depth = 0;
    tie_group_stats.sorts++;
    tie_group_stats.items += count;
    for (size_t start = 0, end; start < count; start = end) {
        int mixed = 0;
        for (end = start + 1; end < count && same_keys(first + start * ITEM_SIZE, first + end * ITEM_SIZE); end++)
            mixed |= nibble(first + end * ITEM_SIZE) != nibble(first + start * ITEM_SIZE);
        if (!mixed) continue;
        DWORD lead = nibble(first + (start ? start - 1 : 0) * ITEM_SIZE);
        if (mode && already_grouped(first + start * ITEM_SIZE, end - start, lead)) {
            stats->skipped_regroups++;
            continue;
        }
        if (end - start > scratch_items) {
            scratch_items = (end - start) * 2;
            scratch = realloc(scratch, scratch_items * ITEM_SIZE);
        }
        regroup(first + start * ITEM_SIZE, end - start, lead);
        tie_group_stats.regrouped_runs++;
        tie_group_stats.moved_items += end - start;
    }
    QueryPerformanceCounter(&finished);
    stats->ticks += finished.QuadPart - started.QuadPart;
}

static void report(BOOL start) {
    if (start) {
        memset(sort_once_stats, 0, sizeof sort_once_stats);
        return;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    for (int mode = 0; mode < 2; mode++) {
        const SortOnceStats *stats = &sort_once_stats[mode];
        double frames = stats->frames ? stats->frames : 1;
        api->log_line("  sortonce %s: %.1f list C sorts per frame, %.1f skipped, %.1f regroups skipped | %.3f ms per frame in the sort hook (%llu frames)",
                      mode ? "on " : "off", stats->sorts / frames, stats->skipped_sorts / frames, stats->skipped_regroups / frames,
                      1000.0 * stats->ticks / frequency.QuadPart / frames, (unsigned long long)stats->frames);
    }
    api->log_line("  sortonce: %lld skips checked against the engine sort, %lld differed", (long long)verified_skips, (long long)differing_skips);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *s2) {
    api = s2;
    fix_enabled = api->fix_enabled;
    frame_stamp = (volatile uint64_t *)(api->engine + FRAME_STAMP_RVA);
    const char *error = api->hook(api->engine + QUEUE_SORT_RVA, queue_sort_prologue, sizeof queue_sort_prologue, (const void *)queue_sort_hook,
                                  (void **)&original_queue_sort);
    if (!error) api->register_switch("sortonce", &sort_once, report);
    return error;
}
