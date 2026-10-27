#include "../s2module.h"
#include "../checks/texturecache.h"
#include <stdint.h>
#include <string.h>
#include <intrin.h>

// The HUD's texture lookup (thiscall, 0x113ef7e0: a string-keyed open-addressing map, called per UI image quad per
// frame) hashes the name and probes on every call; its result is cached per key object.
#define STRING_MAP_FIND_RVA 0x13ef7e0
#define STRING_EQUAL_RVA 0xee2c0
#define MAP_CAPACITY_OFFSET 0x4
#define MAP_ENTRY_SIZE 0x48
#define MAP_VALUE_OFFSET 0xc
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
// 0 always runs the engine's lookup; exported, and a switch, so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG cache_enabled = 1;

typedef struct {
    const BYTE *map;
    const void *key, *entries;
    uint32_t index;
} CachedFind;

typedef BYTE *(__attribute__((thiscall)) *MapFindFn)(BYTE *map, const void *key);
typedef BYTE(__attribute__((thiscall)) *StringEqualFn)(const void *key, const void *entry);

static const BYTE map_find_prologue[] = {0x53, 0x55, 0x56, 0x57, 0xff, 0x74, 0x24, 0x14};
static const BYTE string_equal_prologue[] = {0xff, 0x74, 0x24, 0x04, 0x51};
static volatile LONG *fix_enabled;
static MapFindFn original_map_find;
static StringEqualFn string_equal;
static __thread CachedFind find_cache[CACHE_SLOTS];

// Keys are unique, so an entry still holding an equal key is the one the full lookup returns. The entry array and its size
// must be unchanged so the cached index still points into the live table.
static BYTE *cached_map_find(BYTE *map, const void *key) {
    const BYTE *entries = *(BYTE *const *)map;
    uint32_t capacity = *(const uint32_t *)(map + MAP_CAPACITY_OFFSET);
    CachedFind *cached = &find_cache[((uintptr_t)key >> 2) % CACHE_SLOTS];
    if (cached->map == map && cached->key == key && cached->entries == entries && cached->index < capacity &&
        string_equal(key, entries + (size_t)cached->index * MAP_ENTRY_SIZE)) {
        InterlockedIncrement64(&lookup_stats.hits);
        return (BYTE *)entries + (size_t)cached->index * MAP_ENTRY_SIZE + MAP_VALUE_OFFSET;
    }
    BYTE *value = original_map_find(map, key);
    if (value) *cached = (CachedFind){map, key, entries, (uint32_t)((value - MAP_VALUE_OFFSET - entries) / MAP_ENTRY_SIZE)};
    return value;
}

static BYTE *__attribute__((thiscall)) map_find_hook(BYTE *map, const void *key) {
    if (!*fix_enabled || !cache_enabled) return original_map_find(map, key);
    if (lookup_mode == MODE_VERIFY) {
        BYTE *value = original_map_find(map, key);
        if (cached_map_find(map, key) != value) InterlockedIncrement64(&lookup_stats.mismatches);
        InterlockedIncrement64(&lookup_stats.calls[0]);
        return value;
    }
    static LONG call_number;
    int engine_path = lookup_mode == MODE_ALTERNATE && (InterlockedIncrement(&call_number) & 1);
    uint64_t started = __rdtsc();
    BYTE *value = engine_path ? original_map_find(map, key) : cached_map_find(map, key);
    InterlockedAdd64(&lookup_stats.cycles[engine_path], __rdtsc() - started);
    InterlockedIncrement64(&lookup_stats.calls[engine_path]);
    return value;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, texturecache_checks);
    if (mismatch) return mismatch;
    fix_enabled = api->fix_enabled;
    string_equal = (StringEqualFn)(api->engine + STRING_EQUAL_RVA);
    if (memcmp(api->engine + STRING_EQUAL_RVA, string_equal_prologue, sizeof string_equal_prologue)) return "the key comparison differs from the analysed build";
    const char *error = api->hook(api->engine + STRING_MAP_FIND_RVA, map_find_prologue, sizeof map_find_prologue, (const void *)map_find_hook,
                                  (void **)&original_map_find);
    if (!error) api->register_switch("texturecache", &cache_enabled, NULL);
    return error;
}
