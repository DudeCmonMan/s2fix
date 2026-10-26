#include "../s2module.h"
#include <stdint.h>
#include <string.h>
#include <intrin.h>

// On every game update the engine walks its list of Lua states (cdecl, 0x1127a1c0) and calls lua_gc(L, LUA_GCSTEP, 1)
// on each. The states are all lua_newthread coroutines of the one lua_newstate, sharing a single global_State, so the
// one heap's incremental collector is stepped once per state, mostly re-sweeping a heap with little garbage. This steps
// each distinct heap once per walk, on its first state in list order. Allocation still drives the collector's own steps.
// The full-collect path (flag 1) is unchanged.
#define GC_STATES_RVA 0x127a1c0
#define LUA_GC_RVA 0xea2b20
#define STATE_LIST_FIRST_RVA 0x17bf050
#define STATE_LIST_END_RVA 0x17bf054
#define NODE_NEXT_OFFSET 0x4
#define NODE_HOLDER_OFFSET 0x8
#define STATE_GLOBAL_OFFSET 0x10
#define LUA_GCCOLLECT 2
#define LUA_GCSTEP 5
#define MAX_HEAPS 16

typedef int (*LuaGcFn)(BYTE *state, int what, int data);
typedef void (*GcStatesFn)(BYTE full_collect);

typedef struct {
    LONG64 passes, states, steps, skipped, heap_overflows, cycles;
} GcStats;

__declspec(dllexport) volatile GcStats gc_stats;
// 0 steps every state as the engine does; exported, and a switch, so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG dedupe_enabled = 1;

static const BYTE gc_states_prologue[] = {0x56, 0x8b, 0x35, 0x50, 0xf0, 0x7b, 0x11};
static const BYTE lua_gc_prologue[] = {0x8b, 0x44, 0x24, 0x08, 0x53, 0x56, 0x57, 0x8b};
static BYTE *engine;
static const S2Api *api;
static LuaGcFn lua_gc;
static GcStatesFn original_gc_states;

static void gc_states_hook(BYTE full_collect) {
    if (full_collect || !dedupe_enabled || !*api->fix_enabled) {
        original_gc_states(full_collect);
        return;
    }
    uint64_t started = __rdtsc();
    BYTE *stepped_heaps[MAX_HEAPS];
    int heap_count = 0;
    for (BYTE *node = *(BYTE **)(engine + STATE_LIST_FIRST_RVA); node != *(BYTE **)(engine + STATE_LIST_END_RVA);
         node = *(BYTE **)(node + NODE_NEXT_OFFSET)) {
        BYTE *state = **(BYTE ***)(node + NODE_HOLDER_OFFSET), *heap = *(BYTE **)(state + STATE_GLOBAL_OFFSET);
        gc_stats.states++;
        int seen = 0;
        while (seen < heap_count && stepped_heaps[seen] != heap) seen++;
        if (seen < heap_count) {
            gc_stats.skipped++;
            continue;
        }
        if (heap_count < MAX_HEAPS) stepped_heaps[heap_count++] = heap;
        else gc_stats.heap_overflows++;
        lua_gc(state, LUA_GCSTEP, 1);
        gc_stats.steps++;
    }
    gc_stats.passes++;
    gc_stats.cycles += __rdtsc() - started;
}

static void report(BOOL start) {
    if (start) {
        memset((void *)&gc_stats, 0, sizeof gc_stats);
        return;
    }
    api->log_line("  luagc: %lld deduplicated walks: %lld states, %lld stepped, %lld skipped (%.1f states per walk)", gc_stats.passes, gc_stats.states,
                  gc_stats.steps, gc_stats.skipped, gc_stats.passes ? (double)gc_stats.states / gc_stats.passes : 0.0);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    engine = api->engine;
    lua_gc = (LuaGcFn)(engine + LUA_GC_RVA);
    if (memcmp(engine + LUA_GC_RVA, lua_gc_prologue, sizeof lua_gc_prologue)) return "lua_gc differs from the analysed build";
    const char *error = api->hook(engine + GC_STATES_RVA, gc_states_prologue, sizeof gc_states_prologue, (const void *)gc_states_hook,
                                  (void **)&original_gc_states);
    if (!error) api->register_switch("luagc", &dedupe_enabled, report);
    return error;
}
