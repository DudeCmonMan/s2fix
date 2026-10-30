#include "../s2module.h"
#include "../checks/listkeep.h"
#include <stdint.h>

// The Variant node's list clear frees every per-thread vector, so each frame's parallel appends regrow them item by
// item; this clear empties them and keeps their buffers, the state the append's rebuild leaves (offsets zeroed).
#define CLEAR_RVA 0x14bf490
#define DESTROY_VECTOR_RVA 0x103870
#define PUSH_BACK_RVA 0x98b40
// A per-thread vector over this many items is freed as the engine frees it, so one large frame keeps no memory.
#define MAX_KEPT_ITEMS 4096

typedef void(__attribute__((thiscall)) *ClearFn)(BYTE *list);
typedef void (*DestroyVectorFn)(DWORD *vector);
typedef void(__attribute__((thiscall)) *PushBackFn)(DWORD *vector, const DWORD *value);

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
    const char *mismatch = S2_CHECK_ENGINE(host, listkeep_checks);
    if (mismatch) return mismatch;
    api = host;
    const char *error = api->hook(api->engine + CLEAR_RVA, clear_prologue, sizeof clear_prologue, (const void *)clear_hook, (void **)&original_clear);
    if (error) return error;
    api->register_switch("listkeep", &keep_enabled, NULL);
    return NULL;
}
