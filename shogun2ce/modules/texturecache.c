#include "../s2module.h"
#include "../checks/texturecache.h"
#include <stdint.h>
#include <intrin.h>

// The HUD's texture lookup (sub_1818ac950, a string-keyed open-addressing map, called per UI quad per frame) hashes the
// name and probes on every call; its result is cached per key object.
#define STRING_MAP_FIND_RVA 0x18ac950
#define STRING_EQUAL_RVA 0x19b6a0
#define MAP_ENTRY_SIZE 0x60
#define MAP_VALUE_OFFSET 0x18
#define CACHE_SLOTS 4096
#define MODE_PRODUCTION 0
#define MODE_VERIFY 1
#define MODE_ALTERNATE 2

typedef struct {
    LONG64 calls[2], cycles[2], mismatches, hits;
} HookStats;

// 0 runs only the new code; 1 runs both and counts differing results, returning the engine's; 2 alternates calls and times both.
__declspec(dllexport) volatile LONG lookup_mode = MODE_PRODUCTION;
__declspec(dllexport) volatile HookStats lookup_stats;

typedef struct {
    const BYTE *map;
    const void *key, *entries;
    uint32_t index;
} CachedFind;

typedef BYTE *(*MapFindFn)(BYTE *map, const void *key);
typedef uint64_t (*StringEqualFn)(const void *a, const void *b);

static const BYTE map_find_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
static BYTE *engine;
static volatile LONG *fix_enabled;
static MapFindFn original_map_find;
static StringEqualFn string_equal;
static __thread CachedFind find_cache[CACHE_SLOTS];

// Keys are unique, so an entry still holding an equal key is the one the full lookup returns. The entry array and its size
// must be unchanged so the cached index still points into the live table.
static BYTE *cached_map_find(BYTE *map, const void *key) {
    const BYTE *entries = *(BYTE *const *)map;
    uint32_t count = *(const uint32_t *)(map + 8);
    CachedFind *cached = &find_cache[((uintptr_t)key >> 3) % CACHE_SLOTS];
    if (cached->map == map && cached->key == key && cached->entries == entries && cached->index < count &&
        (BYTE)string_equal(key, entries + (size_t)cached->index * MAP_ENTRY_SIZE)) {
        InterlockedIncrement64(&lookup_stats.hits);
        return (BYTE *)entries + (size_t)cached->index * MAP_ENTRY_SIZE + MAP_VALUE_OFFSET;
    }
    BYTE *value = original_map_find(map, key);
    if (value) *cached = (CachedFind){map, key, entries, (uint32_t)((value - MAP_VALUE_OFFSET - entries) / MAP_ENTRY_SIZE)};
    return value;
}

static BYTE *map_find_hook(BYTE *map, const void *key) {
    if (!*fix_enabled) return original_map_find(map, key);
    if (lookup_mode == MODE_VERIFY) {
        BYTE *value = original_map_find(map, key);
        if (cached_map_find(map, key) != value) InterlockedIncrement64(&lookup_stats.mismatches);
        InterlockedIncrement64(&lookup_stats.calls[0]);
        return value;
    }
    static LONG64 call_number;
    int engine_path = lookup_mode == MODE_ALTERNATE && (InterlockedIncrement64(&call_number) & 1);
    uint64_t started = __rdtsc();
    BYTE *value = engine_path ? original_map_find(map, key) : cached_map_find(map, key);
    InterlockedAdd64(&lookup_stats.cycles[engine_path], __rdtsc() - started);
    InterlockedIncrement64(&lookup_stats.calls[engine_path]);
    return value;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, texturecache_checks);
    if (mismatch) return mismatch;
    engine = api->engine;
    fix_enabled = api->fix_enabled;
    string_equal = (StringEqualFn)(engine + STRING_EQUAL_RVA);
    return api->hook(engine + STRING_MAP_FIND_RVA, map_find_prologue, sizeof map_find_prologue, (const void *)map_find_hook, (void **)&original_map_find);
}
