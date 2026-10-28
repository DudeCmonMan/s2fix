#include "../s2module.h"
#include "../checks/listkeep.h"
#include <stdint.h>

// The Variant node's clear (0x181b1a9e0, end of frame) empties its pose list and lists A, B and C. Each list body is
// {capacity, count, items}, the per-thread sublists at +0x10 {capacity, thread count +0x14, 16-byte {capacity, count,
// items} at +0x18} that the parallel append fills, the merge's offsets at +0x20 {capacity, count +0x24, data +0x28} and
// the needs-merge flag at +0x30. The clear frees every sublist buffer and empties the sublist vector, so in every frame
// the next parallel append rebuilds it under the list's lock and each thread's sublist regrows from one item, copying all
// its items at each doubling. This clear empties the sublists and keeps their buffers, the vector and the offsets' size,
// which is the state the append's rebuild leaves (offsets all zero) and the merge leaves (sublists empty, buffers kept).
#define CLEAR_RVA 0x1b1a9e0
#define FREE_RVA 0x19fe10
#define PUSH_OFFSET_RVA 0x2714f0
// A sublist over this many bytes is freed as the engine frees it, so one large frame keeps no memory.
#define MAX_KEPT_BYTES (1024 * 1024)

typedef void (*ClearFn)(BYTE *node);
typedef void (*FreeFn)(void *memory);
typedef void (*PushOffsetFn)(DWORD *vector, const DWORD *value);

typedef struct {
    DWORD capacity, count;
    BYTE *items;
} Sublist;

typedef struct {
    DWORD body, element_size;
} ListLayout;

// The pose list (16-byte rows), then lists A, B and C (176-byte render items), in the engine's order.
static const ListLayout lists[] = {{0x270, 0x10}, {0x100, 0xb0}, {0x168, 0xb0}, {0x1d0, 0xb0}};

// 0 runs the engine's clear; exported, and a switch.
__declspec(dllexport) volatile LONG keep_enabled = 1;

static const BYTE clear_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9,
                                      0xc7, 0x81, 0x6c, 0x02, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00};
static const S2Api *api;
static ClearFn original_clear;

static void keep_clear(BYTE *list, DWORD element_size) {
    *(DWORD *)(list + 0x4) = 0;
    DWORD threads = *(DWORD *)(list + 0x14);
    Sublist *sublists = *(Sublist **)(list + 0x18);
    for (DWORD i = 0; i < threads; i++) {
        if ((uint64_t)sublists[i].capacity * element_size > MAX_KEPT_BYTES) {
            ((FreeFn)(api->engine + FREE_RVA))(sublists[i].items);
            sublists[i].capacity = 0;
            sublists[i].items = NULL;
        }
        sublists[i].count = 0;
    }
    DWORD *offsets = (DWORD *)(list + 0x20);
    // Without sublists, the offsets end as the engine's clear leaves them: one zero.
    if (!threads) {
        static const DWORD zero = 0;
        offsets[1] = 0;
        ((PushOffsetFn)(api->engine + PUSH_OFFSET_RVA))(offsets, &zero);
    } else {
        DWORD *data = *(DWORD **)(list + 0x28);
        for (DWORD i = 0; i < offsets[1]; i++) data[i] = 0;
    }
    list[0x30] = 0;
}

static void clear_hook(BYTE *node) {
    if (!*api->fix_enabled || !keep_enabled) {
        original_clear(node);
        return;
    }
    *(DWORD *)(node + 0x26c) = 0x33;
    node[0x2e1] = 1;
    for (size_t i = 0; i < sizeof lists / sizeof *lists; i++) keep_clear(node + lists[i].body, lists[i].element_size);
    *(DWORD *)(node + 0x32c) = 0;
    *(uint64_t *)(node + 0x308) = 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, listkeep_checks);
    if (mismatch) return mismatch;
    api = host;
    const char *error = api->hook(api->engine + CLEAR_RVA, clear_prologue, sizeof clear_prologue, (const void *)clear_hook, (void **)&original_clear);
    if (error) return error;
    api->register_switch("listkeep", &keep_enabled, NULL);
    return NULL;
}
