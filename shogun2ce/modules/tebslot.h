#pragma once
#include <windows.h>
#include <intrin.h>
#include <stdlib.h>

// A per-thread pointer in a TEB slot: __thread is emulated TLS here, which locks and clobbers the last error.
#define TEB_TLS_SLOTS 0x1480
#define TEB_TLS_EXPANSION 0x1780
#define TEB_SLOT_COUNT 64

static inline void *teb_slot_get(DWORD slot) {
    if (slot < TEB_SLOT_COUNT) return (void *)__readgsqword(TEB_TLS_SLOTS + slot * 8);
    void **expansion = (void **)__readgsqword(TEB_TLS_EXPANSION);
    return expansion ? expansion[slot - TEB_SLOT_COUNT] : NULL;
}

// Made on first use; the last error is put back, as the game reads it after calls that end in a fix.
static inline void *teb_slot_block(DWORD slot, size_t bytes) {
    void *block = teb_slot_get(slot);
    if (block) return block;
    DWORD error = GetLastError();
    block = calloc(1, bytes);
    if (block && !TlsSetValue(slot, block)) {
        free(block);
        block = NULL;
    }
    SetLastError(error);
    return block;
}
