#include "../s2module.h"
#include "../checks/campappearance.h"
#include <intrin.h>
#include <stdint.h>
#include <string.h>

// Campaign map. Every frame each character's appearance pick (0x108e50c0, through 0x1068d6e0) walks its appearance set
// for the entry that fits (0x10b67f60, thiscall set; age, season, rank, scale; ret 0x10), copying and comparing each
// entry's season name: 1,643 walks a frame on an idle map. The walk reads only the set (flags at +0..3; a hash table with
// bucket count +0xc, buckets +0x10, size +0x14) and its entries, which are database records, so its answer depends on
// the set and the four arguments. This keeps the last answer per (set, arguments), on the main thread only, and returns
// it while the set's flags and table are unchanged. The switch campappearanceverify runs the engine's walk beside every
// cached answer and counts any difference, which would also show an entry changing. Follows Apply Fixes.
#define WALK_RVA 0xb67f60
#define SET_FLAGS 0x0
#define SET_BUCKET_COUNT 0xc
#define SET_BUCKETS 0x10
#define SET_SIZE 0x14
#define CACHE_BITS 12
#define REPORT_MS 10000

typedef BYTE *(__attribute__((thiscall)) *WalkFn)(BYTE *set, int32_t age, uint32_t season, int32_t rank, float scale);

typedef struct {
    BYTE *set, *result;
    uint32_t flags, bucket_count, buckets, size;
    int32_t age, rank;
    uint32_t season, scale;
} Entry;

// Plain counters: they are only written on the main thread.
typedef struct {
    LONG64 walks, hits, mismatches;
} Stats;

__declspec(dllexport) volatile LONG appearance_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats appearance_stats;

static const BYTE walk_prologue[] = {0x83, 0xec, 0x48, 0x8b, 0x44, 0x24, 0x4c};
static const S2Api *api;
static WalkFn original_walk;
static Entry cache[1 << CACHE_BITS];

static BYTE *__attribute__((thiscall)) walk_hook(BYTE *set, int32_t age, uint32_t season, int32_t rank, float scale) {
    if (!*api->fix_enabled || !appearance_enabled || __readfsdword(0x24) != *api->main_thread) return original_walk(set, age, season, rank, scale);
    appearance_stats.walks++;
    // The walk clamps the age to 0 and compares the scale as a float: equal bits give the same answer.
    if (age < 0) age = 0;
    uint32_t scale_bits;
    memcpy(&scale_bits, &scale, 4);
    uint32_t flags, bucket_count = *(uint32_t *)(set + SET_BUCKET_COUNT), buckets = *(uint32_t *)(set + SET_BUCKETS), size = *(uint32_t *)(set + SET_SIZE);
    memcpy(&flags, set + SET_FLAGS, 4);
    uint32_t hash = ((uint32_t)(uintptr_t)set >> 3 ^ (uint32_t)age * 0x9e3779b1u ^ season * 0x85ebca6bu ^ (uint32_t)rank * 0xc2b2ae35u ^ scale_bits) * 0x27d4eb2fu;
    Entry *entry = &cache[hash >> (32 - CACHE_BITS)];
    if (entry->set == set && entry->age == age && entry->season == season && entry->rank == rank && entry->scale == scale_bits &&
        entry->flags == flags && entry->bucket_count == bucket_count && entry->buckets == buckets && entry->size == size) {
        appearance_stats.hits++;
        if (verify && original_walk(set, age, season, rank, scale) != entry->result) appearance_stats.mismatches++;
        return entry->result;
    }
    BYTE *result = original_walk(set, age, season, rank, scale);
    *entry = (Entry){set, result, flags, bucket_count, buckets, size, age, rank, season, scale_bits};
    return result;
}

static DWORD WINAPI report_loop(LPVOID unused) {
    (void)unused;
    Stats last = {0};
    for (;; Sleep(REPORT_MS)) {
        Stats now = appearance_stats;
        LONG64 walks = now.walks - last.walks;
        if (walks)
            api->log_line("campappearance: %lld walks a second, %.1f%% from the cache%s", walks * 1000 / REPORT_MS, 100.0 * (double)(now.hits - last.hits) / (double)walks,
                          verify ? "" : "; verify off");
        if (verify && walks) api->log_line("campappearance: %lld cached answers differed from the engine's walk", now.mismatches - last.mismatches);
        last = now;
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, campappearance_checks);
    if (mismatch) return mismatch;
    api = host;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    const char *error = api->hook(api->engine + WALK_RVA, walk_prologue, sizeof walk_prologue, (const void *)walk_hook, (void **)&original_walk);
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("campappearance", &appearance_enabled, NULL);
    api->register_switch("campappearanceverify", &verify, NULL);
    return NULL;
}
