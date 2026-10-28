#include "../s2module.h"
#include "../checks/campappearance.h"
#include "tebslot.h"
#include <stdint.h>
#include <string.h>

// Campaign map. Every frame each character's appearance pick walks its appearance set for the entry that fits
// (sub_180e40590: set, age, season, rank, scale), copying and comparing each entry's season name. The walk reads only the
// set (flags at +0..3; a hash table with bucket count +0x14, buckets +0x18, size +0x20) and its entries (age bound
// +0x30, rank bound +0x34, season name +0x50, scale bound +0xb0), which are database records, so its answer depends on
// the set and the four arguments. This keeps the last answer per (set, arguments), in a table per thread, and returns
// it while the set's flags and table are unchanged. The switch campappearanceverify runs the engine's walk beside every
// cached answer and counts any difference, which would also show an entry changing. Follows Apply Fixes.
#define WALK_RVA 0xe40590
#define SET_FLAGS 0x0
#define SET_BUCKET_COUNT 0x14
#define SET_BUCKETS 0x18
#define SET_SIZE 0x20
#define CACHE_BITS 12
#define REPORT_MS 10000

typedef BYTE *(*WalkFn)(BYTE *set, int32_t age, uint32_t season, int32_t rank, float scale);

typedef struct {
    BYTE *set, *result, *buckets;
    uint32_t flags, bucket_count, size;
    int32_t age, rank;
    uint32_t season, scale;
} Entry;

// Plain counters: another thread's walk can lose a count.
typedef struct {
    LONG64 walks, hits, mismatches;
} Stats;

__declspec(dllexport) volatile LONG appearance_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats appearance_stats;

static const BYTE walk_prologue[] = {0x48, 0x8b, 0xc4, 0x44, 0x89, 0x48, 0x20, 0x55, 0x57, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa9};
static const S2Api *api;
static WalkFn original_walk;
static DWORD cache_slot = TLS_OUT_OF_INDEXES;

static BYTE *walk_hook(BYTE *set, int32_t age, uint32_t season, int32_t rank, float scale) {
    Entry *cache = *api->fix_enabled && appearance_enabled ? teb_slot_block(cache_slot, sizeof(Entry) << CACHE_BITS) : NULL;
    if (!cache) return original_walk(set, age, season, rank, scale);
    appearance_stats.walks++;
    // The walk clamps the age to 0 (sub_180166080, a max) and compares the scale as a float: equal bits give the same answer.
    if (age < 0) age = 0;
    uint32_t scale_bits, flags, bucket_count = *(uint32_t *)(set + SET_BUCKET_COUNT), size = *(uint32_t *)(set + SET_SIZE);
    BYTE *buckets = *(BYTE **)(set + SET_BUCKETS);
    memcpy(&scale_bits, &scale, 4);
    memcpy(&flags, set + SET_FLAGS, 4);
    uint64_t hash = ((uint64_t)(uintptr_t)set >> 4 ^ (uint64_t)(uint32_t)age * 0x9e3779b97f4a7c15ull ^ (uint64_t)season * 0x85ebca6bu ^
                     (uint64_t)(uint32_t)rank * 0xc2b2ae35u ^ scale_bits) *
                    0xff51afd7ed558ccdull;
    Entry *entry = &cache[hash >> (64 - CACHE_BITS)];
    if (entry->set == set && entry->age == age && entry->season == season && entry->rank == rank && entry->scale == scale_bits &&
        entry->flags == flags && entry->bucket_count == bucket_count && entry->buckets == buckets && entry->size == size) {
        appearance_stats.hits++;
        if (verify && original_walk(set, age, season, rank, scale) != entry->result) appearance_stats.mismatches++;
        return entry->result;
    }
    BYTE *result = original_walk(set, age, season, rank, scale);
    *entry = (Entry){set, result, buckets, flags, bucket_count, size, age, rank, season, scale_bits};
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
    if ((cache_slot = TlsAlloc()) == TLS_OUT_OF_INDEXES) return "no TLS slot left";
    const char *error = api->hook(api->engine + WALK_RVA, walk_prologue, sizeof walk_prologue, (const void *)walk_hook, (void **)&original_walk);
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("campappearance", &appearance_enabled, NULL);
    api->register_switch("campappearanceverify", &verify, NULL);
    return NULL;
}
