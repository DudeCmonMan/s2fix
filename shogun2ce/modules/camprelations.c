#include "../s2module.h"
#include "../checks/camprelations.h"
#include "tebslot.h"
#include <stdint.h>

// Campaign map. The faction-record lookup (sub_180a1b170: table, key) scans an array of 3,368-byte records (count at
// table +0xc, array at +0x10, key at record +0x8) for the first record holding the key, so every step is a cache miss.
// Every frame each character model's update (sub_180ae0510) asks whether the player can see it, through every faction
// friendly to the player (sub_18075f070), and the friendly test (sub_18075ead0) looks the same record up three times.
// In the original game that was 59% of the main thread on an idle map. This remembers where each lookup found its
// record. A repeat returns that record only while the table still has the same array and count and the record still
// holds the key; otherwise the scan runs. The lookup is a 66-byte leaf whose first 14 bytes end in a short branch, so it
// can't be called through a trampoline: this replaces it, and misses run the same scan here (the offline check compares
// it with the engine's). Keys are unique within a table, so the record found is the first match; the switch
// camprelationsverify scans beside every cached answer and counts any difference. Follows Apply Fixes.
#define SEARCH_RVA 0xa1b170
#define RECORD_BYTES 0xd28
#define TABLE_COUNT 0xc
#define TABLE_ARRAY 0x10
#define RECORD_KEY 0x8
#define CACHE_BITS 16
#define REPORT_MS 10000

typedef struct {
    BYTE *table, *array, *key;
    uint32_t count, index;
} Entry;

// Plain counters: a locked 64-bit add on every lookup would cost more than it measures; another thread's lookup can
// lose a count.
typedef struct {
    LONG64 lookups, hits, misses, scanned, not_found, mismatches;
} Stats;

__declspec(dllexport) volatile LONG relations_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats relation_stats;

static const BYTE search_prologue[] = {0x44, 0x8b, 0x51, 0x0c, 0x33, 0xc0, 0x4c, 0x8b, 0xc9, 0x45, 0x85, 0xd2, 0x74, 0x2b};
static const S2Api *api;
// Never called: the copied prologue ends in a short branch.
static void *unusable_trampoline;
// Each thread keeps its own table, so no lookup ever waits on another thread.
static DWORD cache_slot = TLS_OUT_OF_INDEXES;

// The engine's scan: the first record holding the key, or the array's first record when none does.
static BYTE *scan(BYTE *array, uint32_t count, BYTE *key) {
    for (uint32_t i = 0; i < count; i++) {
        BYTE *record = array + (size_t)i * RECORD_BYTES;
        if (*(BYTE **)(record + RECORD_KEY) == key) return record;
    }
    return array;
}

static Entry *entry_for(BYTE *table, BYTE *key) {
    Entry *cache = teb_slot_block(cache_slot, sizeof(Entry) << CACHE_BITS);
    if (!cache) return NULL;
    uint64_t hash = ((uint64_t)(uintptr_t)table >> 4 ^ (uint64_t)(uintptr_t)key * 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull;
    return &cache[hash >> (64 - CACHE_BITS)];
}

static BYTE *search_hook(BYTE *table, BYTE *key) {
    BYTE *array = *(BYTE **)(table + TABLE_ARRAY);
    uint32_t count = *(uint32_t *)(table + TABLE_COUNT);
    if (!*api->fix_enabled || !relations_enabled) return scan(array, count, key);
    relation_stats.lookups++;
    Entry *entry = entry_for(table, key);
    BYTE *result = NULL;
    if (entry && entry->table == table && entry->key == key && entry->array == array && entry->count == count && entry->index < count) {
        BYTE *record = array + (size_t)entry->index * RECORD_BYTES;
        if (*(BYTE **)(record + RECORD_KEY) == key) {
            result = record;
            relation_stats.hits++;
        }
    }
    if (!result || verify) {
        BYTE *scanned = scan(array, count, key);
        if (result && scanned != result) relation_stats.mismatches++;
        if (!result) {
            result = scanned;
            relation_stats.misses++;
            // The scan returns the array's first record both when that record matches and when nothing does.
            BOOL found = count && *(BYTE **)(scanned + RECORD_KEY) == key;
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
        if (lookups)
            api->log_line("camprelations: %lld lookups a second, %.1f%% from the cache; misses scanned %.1f records on average, %lld found nothing%s",
                          lookups * 1000 / REPORT_MS, 100.0 * (double)(now.hits - last.hits) / (double)lookups,
                          misses ? (double)(now.scanned - last.scanned) / (double)misses : 0.0, now.not_found - last.not_found,
                          verify ? "" : "; verify off");
        if (verify && lookups) api->log_line("camprelations: %lld cached results differed from the scan", now.mismatches - last.mismatches);
        last = now;
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, camprelations_checks);
    if (mismatch) return mismatch;
    api = host;
    if ((cache_slot = TlsAlloc()) == TLS_OUT_OF_INDEXES) return "no TLS slot left";
    const char *error = api->hook(api->engine + SEARCH_RVA, search_prologue, sizeof search_prologue, (const void *)search_hook, &unusable_trampoline);
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("camprelations", &relations_enabled, NULL);
    api->register_switch("camprelationsverify", &verify, NULL);
    return NULL;
}
