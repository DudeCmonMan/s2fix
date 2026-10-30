#include "../s2module.h"
#include "../checks/camprelations.h"
#include <intrin.h>
#include <stdint.h>
#include <stdlib.h>

// Campaign map: the faction-record lookup scans big records several times per character per frame. This remembers each
// record found, reused while its table and key are unchanged (keys are unique per table, so it is the first match).
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

// Plain counters: a locked add per lookup would cost more than it measures; a count may be lost.
typedef struct {
    LONG64 lookups, hits, misses, scanned, not_found, mismatches;
} Stats;

__declspec(dllexport) volatile LONG relations_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats relation_stats;

static const BYTE search_prologue[] = {0x51, 0x53, 0x55, 0x8b, 0xe9, 0x56, 0x33, 0xf6, 0x57};
static const S2Api *api;
static SearchFn original_search;
// A table per thread, in a TEB slot: __thread is emulated TLS here, which locks and clobbers the last error.
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
