#include "../s2module.h"
#include <stdint.h>
#include <intrin.h>

// Every frame the engine walks its list of Lua states (sub_18186a630) and calls lua_gc(L, LUA_GCSTEP, 1) on each. The
// states are coroutines of one Lua universe sharing a single global_State, so the one 12 MB heap's incremental collector
// was stepped once per state, ~64 times a frame, mostly re-sweeping a heap with little garbage. This steps each distinct
// heap once per frame, on its first state in list order. Allocation still drives the collector's own steps as usual.
// The full-collect path (flag 1) is unchanged.
#define GC_STATES_RVA 0x186a630
#define LUA_GC_RVA 0x12c8ea0
#define STATE_LIST_FIRST_RVA 0x2e74e20
#define STATE_LIST_END_RVA 0x2e74e28
#define LUA_GCCOLLECT 2
#define LUA_GCSTEP 5
#define MAX_HEAPS 16

typedef int (*LuaGcFn)(BYTE *state, int what, int data);

typedef struct {
    LONG64 passes, states, steps, skipped, heap_overflows, cycles;
} GcStats;

__declspec(dllexport) volatile GcStats gc_stats;
// 0 steps every state as the engine does; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG dedupe_enabled = 1;

static const BYTE gc_states_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x1d, 0xdf, 0xa7, 0x60, 0x01};
static BYTE *engine;
static volatile LONG *fix_enabled;
static LuaGcFn lua_gc;
// The copied prologue holds a RIP-relative load, so the trampoline cannot run; the hook reimplements the whole loop.
static void *unusable_trampoline;

static void gc_states_hook(BYTE full_collect) {
    uint64_t started = __rdtsc();
    BOOL dedupe = !full_collect && dedupe_enabled && *fix_enabled;
    BYTE *stepped_heaps[MAX_HEAPS];
    int heap_count = 0;
    for (BYTE *node = *(BYTE **)(engine + STATE_LIST_FIRST_RVA); node != *(BYTE **)(engine + STATE_LIST_END_RVA); node = *(BYTE **)(node + 8)) {
        BYTE *state = **(BYTE ***)(node + 0x10), *heap = *(BYTE **)(state + 0x20);
        gc_stats.states++;
        if (dedupe) {
            int seen = 0;
            while (seen < heap_count && stepped_heaps[seen] != heap) seen++;
            if (seen < heap_count) {
                gc_stats.skipped++;
                continue;
            }
            if (heap_count < MAX_HEAPS) stepped_heaps[heap_count++] = heap;
            else gc_stats.heap_overflows++;
        }
        lua_gc(state, full_collect ? LUA_GCCOLLECT : LUA_GCSTEP, full_collect ^ 1);
        gc_stats.steps++;
    }
    gc_stats.passes++;
    gc_stats.cycles += __rdtsc() - started;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    engine = api->engine;
    fix_enabled = api->fix_enabled;
    lua_gc = (LuaGcFn)(engine + LUA_GC_RVA);
    return api->hook(engine + GC_STATES_RVA, gc_states_prologue, sizeof gc_states_prologue, (const void *)gc_states_hook, &unusable_trampoline);
}
