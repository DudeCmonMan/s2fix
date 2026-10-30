#include "../s2module.h"
#include "../checks/cailookup.h"
#include "tebslot.h"
#include <intrin.h>
#include <stdint.h>
#include <string.h>

// Campaign end turn. The AI rates how one faction stands with another (sub_180de2140: unused, faction A, faction B, the
// list of factions, a weight source, the AI context) by averaging, over every other faction C, half of A's value for C
// and half of C's value for B, then blending that average with A's value for B by a weight. Each value is a record in a
// faction's table (count at +0x194, array at +0x198; 24-byte records naming the other faction at +8, the value at
// +0x10), found by a linear scan the compiler copied inline, so one rating scans two tables per faction and an end turn's
// cost grows with the cube of the faction count. This replaces the rating with the same arithmetic in the same order
// (baseline x86-64 has no fused multiply-add, so each step rounds as the engine's does), and remembers where each record
// was. A repeat uses that record only while the table still has the same array and count and the record still names the
// faction; otherwise it scans. A table names each faction once, so the record found is the first. The switch
// cailookupverify runs the engine's rating beside each one and counts any result that differs in a bit. Follows Apply
// Fixes.
#define AVERAGE_RVA 0xde2140
#define TABLE_RVA 0xdc36c0
#define SKIP_RVA 0xc9be70
#define TABLE_COUNT 0x194
#define TABLE_ARRAY 0x198
#define RECORD_BYTES 0x18
#define RECORD_FACTION 0x8
#define RECORD_VALUE 0x10
#define LIST_COUNT 0x10
#define LIST_ARRAY 0x18
#define WEIGHT_SLOT (0x148 / 8)
#define CACHE_BITS 16
#define REPORT_MS 10000

typedef float (*AverageFn)(BYTE *unused, BYTE *a, BYTE *b, BYTE *list, BYTE *weights, BYTE *context);
typedef BYTE *(*TableFn)(BYTE *context, BYTE *faction);
typedef uint8_t (*SkipFn)(BYTE *faction);
typedef float (*WeightFn)(BYTE *weights);

typedef struct {
    BYTE *table, *array, *faction;
    uint32_t count, index;
} Entry;

// Plain counters: a locked 64-bit add on every lookup would cost more than it measures; another thread's lookup can
// lose a count.
typedef struct {
    LONG64 ratings, ticks, lookups, hits, mismatches;
} Stats;

__declspec(dllexport) volatile LONG lookup_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats lookup_stats;

static const BYTE average_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
static const S2Api *api;
static AverageFn original_average;
static TableFn table_of;
static SkipFn skipped;
// Each thread keeps its own table, so no lookup ever waits on another thread.
static DWORD cache_slot = TLS_OUT_OF_INDEXES;

// The engine's inline scan: the first record naming the faction, or NULL.
static BYTE *scan(BYTE *array, uint32_t count, BYTE *faction) {
    for (uint32_t i = 0; i < count; i++) {
        BYTE *record = array + (size_t)i * RECORD_BYTES;
        if (*(BYTE **)(record + RECORD_FACTION) == faction) return record;
    }
    return NULL;
}

static BYTE *find(BYTE *table, BYTE *faction) {
    BYTE *array = *(BYTE **)(table + TABLE_ARRAY);
    uint32_t count = *(uint32_t *)(table + TABLE_COUNT);
    lookup_stats.lookups++;
    Entry *cache = teb_slot_block(cache_slot, sizeof(Entry) << CACHE_BITS);
    if (!cache) return scan(array, count, faction);
    uint64_t hash = ((uint64_t)(uintptr_t)table >> 4 ^ (uint64_t)(uintptr_t)faction * 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull;
    Entry *entry = &cache[hash >> (64 - CACHE_BITS)];
    if (entry->table == table && entry->faction == faction && entry->array == array && entry->count == count) {
        BYTE *record = array + (size_t)entry->index * RECORD_BYTES;
        if (*(BYTE **)(record + RECORD_FACTION) == faction) {
            lookup_stats.hits++;
            return record;
        }
    }
    BYTE *record = scan(array, count, faction);
    if (record) *entry = (Entry){table, array, faction, count, (uint32_t)((record - array) / RECORD_BYTES)};
    return record;
}

static float value(BYTE *record) { return *(float *)(record + RECORD_VALUE); }

static float weight(BYTE *weights) { return (*(WeightFn **)weights)[WEIGHT_SLOT](weights); }

static float average(BYTE *a, BYTE *b, BYTE *list, BYTE *weights, BYTE *context) {
    BYTE *table_a = table_of(context, a);
    float sum = 0.0f;
    uint32_t counted = 0;
    // The engine reads the list's end again after every faction.
    for (BYTE **at = *(BYTE ***)(list + LIST_ARRAY); at != *(BYTE ***)(list + LIST_ARRAY) + *(uint32_t *)(list + LIST_COUNT); at++) {
        if (*at == a || *at == b || skipped(*at)) continue;
        float c_to_b = value(find(table_of(context, *at), b));
        float a_to_c = value(find(table_a, *at));
        sum += a_to_c * 0.5f + c_to_b * 0.5f;
        counted++;
    }
    float rest = (1.0f - weight(weights)) * (sum / (float)counted);
    float a_to_b = value(find(table_a, b));
    return weight(weights) * a_to_b + rest;
}

static float average_hook(BYTE *unused, BYTE *a, BYTE *b, BYTE *list, BYTE *weights, BYTE *context) {
    uint64_t start = __rdtsc();
    float result;
    if (!*api->fix_enabled || !lookup_enabled) {
        result = original_average(unused, a, b, list, weights, context);
    } else {
        result = average(a, b, list, weights, context);
        if (verify) {
            float engine = original_average(unused, a, b, list, weights, context);
            if (memcmp(&engine, &result, sizeof result)) lookup_stats.mismatches++;
        }
    }
    lookup_stats.ratings++;
    lookup_stats.ticks += __rdtsc() - start;
    return result;
}

static DWORD WINAPI report_loop(LPVOID unused) {
    (void)unused;
    Stats last = {0};
    LARGE_INTEGER frequency, then, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&then);
    uint64_t tsc_then = __rdtsc();
    for (;;) {
        Sleep(REPORT_MS);
        Stats stats = lookup_stats;
        QueryPerformanceCounter(&now);
        uint64_t tsc_now = __rdtsc();
        double tsc_per_ms = (double)(tsc_now - tsc_then) / ((double)(now.QuadPart - then.QuadPart) * 1000.0 / (double)frequency.QuadPart);
        LONG64 ratings = stats.ratings - last.ratings, lookups = stats.lookups - last.lookups;
        if (verify && ratings)
            api->log_line("cailookup: %lld ratings a second taking %.1f ms a second; %lld lookups a second, %.1f%% from the cache",
                          ratings * 1000 / REPORT_MS, (double)(stats.ticks - last.ticks) / tsc_per_ms * 1000.0 / REPORT_MS,
                          lookups * 1000 / REPORT_MS, lookups ? 100.0 * (double)(stats.hits - last.hits) / (double)lookups : 0.0);
        if (verify && ratings) api->log_line("cailookup: %lld ratings differed from the engine's", stats.mismatches - last.mismatches);
        last = stats;
        then = now;
        tsc_then = tsc_now;
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, cailookup_checks);
    if (mismatch) return mismatch;
    api = host;
    if ((cache_slot = TlsAlloc()) == TLS_OUT_OF_INDEXES) return "no TLS slot left";
    table_of = (TableFn)(api->engine + TABLE_RVA);
    skipped = (SkipFn)(api->engine + SKIP_RVA);
    const char *error = api->hook(api->engine + AVERAGE_RVA, average_prologue, sizeof average_prologue, (const void *)average_hook, (void **)&original_average);
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("cailookup", &lookup_enabled, NULL);
    api->register_switch("cailookupverify", &verify, NULL);
    return NULL;
}
