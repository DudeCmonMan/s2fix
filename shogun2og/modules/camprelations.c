#include "../s2module.h"
#include "../checks/camprelations.h"
#include <intrin.h>
#include <stdint.h>
#include <stdlib.h>

// Campaign map. The faction-record lookup (0x107f90e0, thiscall table, key) scans an array of 2,020-byte records (count
// at table +0x8, array at +0xc, id at record +0x4) for the first record whose id is the key, so every step is a cache
// miss. Every frame each character model's update (0x10898ea0) asks whether the player can see it, through every
// faction that is friendly to the player (0x105caee0), and the friendly test (0x105ca920) looks the same record up three
// times: on an idle map that was 59% of the main thread. This remembers where each lookup found its record. A repeat
// returns that record only while the table still has the same array and count and the record still has the key;
// otherwise the engine's scan runs again. Keys are unique within a table, so the record found is the first match; the
// switch camprelationsverify runs the engine's scan beside every lookup and counts any difference. Follows Apply Fixes.
#define SEARCH_RVA 0x7f90e0
#define RECORD_BYTES 0x7e4
#define TABLE_COUNT 0x8
#define TABLE_ARRAY 0xc
#define RECORD_KEY 0x4
#define CACHE_BITS 16
#define TEB_TLS_SLOTS 0xe10
#define TEB_TLS_EXPANSION 0xf94
#define TEB_SLOT_COUNT 64
#define REPORT_MS 10000

typedef BYTE *(__attribute__((thiscall)) *SearchFn)(BYTE *table, uint32_t key);

typedef struct {
    BYTE *table, *array;
    uint32_t key, count, index;
} Entry;

// Plain counters: a locked 64-bit add on every lookup would cost more than it measures; another thread's lookup can
// lose a count.
typedef struct {
    LONG64 lookups, hits, misses, scanned, not_found, mismatches;
} Stats;

__declspec(dllexport) volatile LONG relations_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats relation_stats;

static const BYTE search_prologue[] = {0x51, 0x53, 0x55, 0x8b, 0xe9, 0x56, 0x33, 0xf6, 0x57};
static const S2Api *api;
static SearchFn original_search;
// Each thread keeps its own table, so no lookup ever waits on another thread. Not __thread: with this toolchain that is
// emulated TLS (a locked winpthreads lookup and two last-error calls per access), 1.5 ms a frame at 20 million lookups
// a second. The TEB's own slot is one load and leaves the last error alone.
static DWORD cache_slot = TLS_OUT_OF_INDEXES;

static Entry *thread_cache(void) {
    if (cache_slot < TEB_SLOT_COUNT) return (Entry *)__readfsdword(TEB_TLS_SLOTS + cache_slot * 4);
    void **expansion = (void **)__readfsdword(TEB_TLS_EXPANSION);
    return expansion ? expansion[cache_slot - TEB_SLOT_COUNT] : NULL;
}

static Entry *new_thread_cache(void) {
    DWORD error = GetLastError();
    Entry *cache = calloc((size_t)1 << CACHE_BITS, sizeof *cache);
    if (cache && !TlsSetValue(cache_slot, cache)) {
        free(cache);
        cache = NULL;
    }
    SetLastError(error);
    return cache;
}

static Entry *entry_for(BYTE *table, uint32_t key) {
    Entry *cache = thread_cache();
    if (!cache && !(cache = new_thread_cache())) return NULL;
    uint32_t hash = ((uint32_t)(uintptr_t)table >> 2 ^ key * 0x9e3779b1u) * 0x85ebca6bu;
    return &cache[hash >> (32 - CACHE_BITS)];
}

static BYTE *__attribute__((thiscall)) search_hook(BYTE *table, uint32_t key) {
    if (!*api->fix_enabled || !relations_enabled) return original_search(table, key);
    BYTE *array = *(BYTE **)(table + TABLE_ARRAY);
    uint32_t count = *(uint32_t *)(table + TABLE_COUNT);
    relation_stats.lookups++;
    Entry *entry = entry_for(table, key);
    BYTE *result = NULL;
    if (entry && entry->table == table && entry->key == key && entry->array == array && entry->count == count && entry->index < count) {
        BYTE *record = array + (size_t)entry->index * RECORD_BYTES;
        if (*(uint32_t *)(record + RECORD_KEY) == key) {
            result = record;
            relation_stats.hits++;
        }
    }
    if (!result || verify) {
        BYTE *scanned = original_search(table, key);
        if (result && scanned != result) relation_stats.mismatches++;
        if (!result) {
            result = scanned;
            relation_stats.misses++;
            // The scan returns the array's first record both when that record matches and when nothing does.
            BOOL found = count && *(uint32_t *)(scanned + RECORD_KEY) == key;
            uint32_t index = (uint32_t)((scanned - array) / RECORD_BYTES);
            relation_stats.scanned += found ? index + 1 : count;
            if (found && entry) *entry = (Entry){table, array, key, count, index};
            else relation_stats.not_found++;
        }
    }
    return result;
}

static DWORD WINAPI report_loop(LPVOID unused) {
    (void)unused;
    Stats last = {0};
    for (;; Sleep(REPORT_MS)) {
        Stats now = relation_stats;
        LONG64 lookups = now.lookups - last.lookups, misses = now.misses - last.misses;
        if (verify && lookups)
            api->log_line("camprelations: %lld lookups a second, %.1f%% from the cache; misses scanned %.1f records on average, %lld found nothing",
                          lookups * 1000 / REPORT_MS, 100.0 * (double)(now.hits - last.hits) / (double)lookups,
                          misses ? (double)(now.scanned - last.scanned) / (double)misses : 0.0, now.not_found - last.not_found);
        if (verify && lookups) api->log_line("camprelations: %lld cached results differed from the engine's scan", now.mismatches - last.mismatches);
        last = now;
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, camprelations_checks);
    if (mismatch) return mismatch;
    api = host;
    if ((cache_slot = TlsAlloc()) == TLS_OUT_OF_INDEXES) return "no TLS slot left";
    const char *error = api->hook(api->engine + SEARCH_RVA, search_prologue, sizeof search_prologue, (const void *)search_hook, (void **)&original_search);
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("camprelations", &relations_enabled, NULL);
    api->register_switch("camprelationsverify", &verify, NULL);
    return NULL;
}
