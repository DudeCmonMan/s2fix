#pragma once
#include <windows.h>
#include <intrin.h>
#include <stdlib.h>

// A per-thread pointer in a TlsAlloc slot, read straight from the TEB. Not __thread: with this toolchain that can be
// emulated TLS (a locked winpthreads lookup and two last-error calls per access), which cost the original game's
// campaign cache 1.5 ms a frame at 20 million lookups a second. The TEB's own slot is one load and leaves the last error
// alone.
#define TEB_TLS_SLOTS 0x1480
#define TEB_TLS_EXPANSION 0x1780
#define TEB_SLOT_COUNT 64

static inline void *teb_slot_get(DWORD slot) {
    if (slot < TEB_SLOT_COUNT) return (void *)__readgsqword(TEB_TLS_SLOTS + slot * 8);
    void **expansion = (void **)__readgsqword(TEB_TLS_EXPANSION);
    return expansion ? expansion[slot - TEB_SLOT_COUNT] : NULL;
}

// The thread's zeroed block of bytes, made on its first use; NULL if it can't be. The game's code reads the last error
// after calls that end in a fix, so it is put back.
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
