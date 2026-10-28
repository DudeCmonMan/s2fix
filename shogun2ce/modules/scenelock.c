#include "../s2module.h"
#include "../checks/scenelock.h"

// The Variant node's locked add (sub_181b13060) holds the critical section at node + 0x338 for a soldier's whole
// emission, and the main thread and up to 15 TBB workers call it from one parallel loop. With the engine's spin count
// of 10000, waiters still fall asleep in the kernel and the main thread pays the wake-up; a longer spin keeps them
// awake through the queue. What the lock protects and how it is taken are unchanged.
#define LOCKED_ADD_RVA 0x1b13060
#define SCENE_LOCK_OFFSET 0x338

typedef void (*LockedAddFn)(BYTE *scene, void *a2, void *a3, void *a4, void *a5, void *a6, void *a7, void *a8, void *a9, void *a10, void *a11);

// 0 leaves the engine's spin count; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG spin_count = 200000;
__declspec(dllexport) CRITICAL_SECTION *volatile scene_lock;
__declspec(dllexport) volatile ULONG_PTR engine_spin_count;

static const BYTE locked_add_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18};
static LockedAddFn original_locked_add;
static volatile LONG *fix_enabled;
static LONG applied_spin_count = -1;

static void locked_add_hook(BYTE *scene, void *a2, void *a3, void *a4, void *a5, void *a6, void *a7, void *a8, void *a9, void *a10, void *a11) {
    CRITICAL_SECTION *lock = (CRITICAL_SECTION *)(scene + SCENE_LOCK_OFFSET);
    LONG wanted = *fix_enabled ? spin_count : 0;
    // Only the thread holding the lock changes its spin count, so no two threads set it at once.
    if (lock != scene_lock || wanted != applied_spin_count) {
        EnterCriticalSection(lock);
        if (lock != scene_lock) {
            scene_lock = lock;
            engine_spin_count = lock->SpinCount;
        }
        // The field's high bits are flags, so the engine's value is restored as it was rather than through the API.
        if (wanted) SetCriticalSectionSpinCount(lock, (DWORD)wanted);
        else lock->SpinCount = engine_spin_count;
        applied_spin_count = wanted;
        LeaveCriticalSection(lock);
    }
    original_locked_add(scene, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, scenelock_checks);
    if (mismatch) return mismatch;
    fix_enabled = api->fix_enabled;
    return api->hook(api->engine + LOCKED_ADD_RVA, locked_add_prologue, sizeof locked_add_prologue, (const void *)locked_add_hook,
                     (void **)&original_locked_add);
}
