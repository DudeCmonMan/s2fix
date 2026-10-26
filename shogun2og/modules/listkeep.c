#include "../s2module.h"
#include <stdint.h>

// A render list (0x114da700's `this`) is a vector of 312-byte items {capacity, count, data}, an array of per-thread
// vectors at +0xc {capacity, count +0x10, data +0x14} that the parallel append fills (one per pool thread), the merge's
// offsets at +0x18 {capacity, count +0x1c, data +0x20} and the needs-merge flag at +0x24. The node's clear (0x114bfce0)
// clears lists A, +0xd4 and B with 0x114bf490, which frees every per-thread vector and empties the array, so in every
// frame the next parallel append rebuilds the array under the list's lock and each thread's vector regrows from one item,
// copying all its items at each doubling: 12,900 item copies a frame in a battle (measured 2026-10-25), more than the
// appends themselves. This clear empties the vectors and keeps their buffers, the array and the offsets' size; the
// offsets are zeroed, as the append's rebuild leaves them.
#define CLEAR_RVA 0x14bf490
#define DESTROY_VECTOR_RVA 0x103870
#define PUSH_BACK_RVA 0x98b40
// A per-thread vector over this many items (1.2 MB) is freed as the engine frees it, so one large frame keeps no memory.
#define MAX_KEPT_ITEMS 4096

typedef void(__attribute__((thiscall)) *ClearFn)(BYTE *list);
typedef void (*DestroyVectorFn)(DWORD *vector);
typedef void(__attribute__((thiscall)) *PushBackFn)(DWORD *vector, const DWORD *value);

// 0 runs the engine's clear; exported, and a switch.
__declspec(dllexport) volatile LONG keep_enabled = 1;

static const BYTE clear_prologue[] = {0x51, 0x56, 0x57, 0x8b, 0xf9};
static const S2Api *api;
static ClearFn original_clear;

static void __attribute__((thiscall)) clear_hook(BYTE *list) {
    if (!*api->fix_enabled || !keep_enabled) {
        original_clear(list);
        return;
    }
    *(DWORD *)(list + 0x4) = 0;
    DWORD threads = *(DWORD *)(list + 0x10);
    DWORD *vectors = *(DWORD **)(list + 0x14);
    for (DWORD i = 0; i < threads; i++) {
        DWORD *vector = vectors + 3 * i;
        if (vector[0] > MAX_KEPT_ITEMS) {
            ((DestroyVectorFn)(api->engine + DESTROY_VECTOR_RVA))(vector);
            vector[0] = vector[2] = 0;
        }
        vector[1] = 0;
    }
    // Without per-thread vectors, the offsets end as the engine's clear leaves them: one zero.
    DWORD *offsets = (DWORD *)(list + 0x18);
    if (!threads) {
        static const DWORD zero = 0;
        offsets[1] = 0;
        ((PushBackFn)(api->engine + PUSH_BACK_RVA))(offsets, &zero);
    } else {
        for (DWORD i = 0; i < offsets[1]; i++) ((DWORD *)(uintptr_t)offsets[2])[i] = 0;
    }
    list[0x24] = 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    const char *error = api->hook(api->engine + CLEAR_RVA, clear_prologue, sizeof clear_prologue, (const void *)clear_hook, (void **)&original_clear);
    if (error) return error;
    api->register_switch("listkeep", &keep_enabled, NULL);
    return NULL;
}
