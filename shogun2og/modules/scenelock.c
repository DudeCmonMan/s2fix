#include "../s2module.h"
#include <stdint.h>

// The Variant node's locked add (thiscall, 0x114b9e10, node vtbl+0x88) holds the critical section at node + 0x204 for
// a soldier's whole emission. When the main thread and TBB workers call it from one parallel loop, waiters with the
// engine's spin count of 10000 still fall asleep in the kernel and the holder pays the wake-up; a longer spin keeps them
// awake through the queue. What the lock protects and how it is taken are unchanged.
#define LOCKED_ADD_RVA 0x14b9e10
#define NODE_LOCK_OFFSET 0x204

typedef uintptr_t(__attribute__((thiscall)) *LockedAddFn)(BYTE *node, void *a1, void *a2, void *a3, void *a4, void *a5, void *a6, void *a7, void *a8,
                                                          void *a9, void *a10);

// 0 leaves the engine's spin count; exported, and a switch, so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG spin_count = 200000;
__declspec(dllexport) volatile LONG longer_spin_enabled = 1;
__declspec(dllexport) CRITICAL_SECTION *volatile node_lock;
__declspec(dllexport) volatile ULONG_PTR engine_spin_count;

static const BYTE locked_add_prologue[] = {0x83, 0xec, 0x18, 0x56, 0x8b, 0xf1};
static LockedAddFn original_locked_add;
static volatile LONG *fix_enabled;
static LONG applied_spin_count = -1;

static uintptr_t __attribute__((thiscall)) locked_add_hook(BYTE *node, void *a1, void *a2, void *a3, void *a4, void *a5, void *a6, void *a7, void *a8,
                                                          void *a9, void *a10) {
    CRITICAL_SECTION *lock = (CRITICAL_SECTION *)(node + NODE_LOCK_OFFSET);
    LONG wanted = *fix_enabled && longer_spin_enabled ? spin_count : 0;
    // Only the thread holding the lock changes its spin count, so no two threads set it at once.
    if (lock != node_lock || wanted != applied_spin_count) {
        EnterCriticalSection(lock);
        if (lock != node_lock) {
            node_lock = lock;
            engine_spin_count = lock->SpinCount;
        }
        // The field's high bits are flags, so the engine's value is restored as it was rather than through the API.
        if (wanted) SetCriticalSectionSpinCount(lock, (DWORD)wanted);
        else lock->SpinCount = engine_spin_count;
        applied_spin_count = wanted;
        LeaveCriticalSection(lock);
    }
    return original_locked_add(node, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    fix_enabled = api->fix_enabled;
    const char *error = api->hook(api->engine + LOCKED_ADD_RVA, locked_add_prologue, sizeof locked_add_prologue, (const void *)locked_add_hook,
                                  (void **)&original_locked_add);
    if (!error) api->register_switch("scenelock", &longer_spin_enabled, NULL);
    return error;
}
