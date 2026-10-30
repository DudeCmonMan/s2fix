#include "../s2module.h"
#include "../checks/uiprefetch.h"
#include <stdint.h>
#include <intrin.h>
#include <xmmintrin.h>

// The UI update recurses over every component each frame, mostly waiting on each child's first loads; entering a node
// prefetches the lines its children's updates read first. Only prefetches; what runs is unchanged.
#define UPDATE_RVA 0xf948e0
#define FRAME_STAMP_RVA 0x17cc340
#define CHILD_COUNT_OFFSET 0x9c
#define CHILDREN_OFFSET 0xa0
#define MAX_PREFETCHED_CHILDREN 64

typedef uintptr_t(__attribute__((thiscall)) *UpdateFn)(BYTE *component, uint32_t time_ms);

typedef struct {
    LONG64 cycles[2], frames[2];
} Stats;

__declspec(dllexport) volatile Stats stats;
__declspec(dllexport) volatile LONG prefetch_enabled = 1;
// 1 prefetches only on even frames, timing the tree update both ways.
__declspec(dllexport) volatile LONG alternate_frames;

static const BYTE update_prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x64};
static UpdateFn original_update;
static volatile LONG *fix_enabled;
static volatile uint32_t *frame_stamp;
static int depth, prefetching;

static void prefetch_children(BYTE *component) {
    uint32_t count = *(uint32_t *)(component + CHILD_COUNT_OFFSET);
    BYTE **children = *(BYTE ***)(component + CHILDREN_OFFSET);
    if (count > MAX_PREFETCHED_CHILDREN) count = MAX_PREFETCHED_CHILDREN;
    for (uint32_t i = 0; i < count; i++) {
        const char *child = (const char *)children[i];
        _mm_prefetch(child, _MM_HINT_T0);
        _mm_prefetch(child + 0x80, _MM_HINT_T0);
        _mm_prefetch(child + 0x200, _MM_HINT_T0);
        _mm_prefetch(child + 0x300, _MM_HINT_T0);
    }
}

static uintptr_t __attribute__((thiscall)) update_hook(BYTE *component, uint32_t time_ms) {
    if (depth++) {
        if (prefetching) prefetch_children(component);
        uintptr_t result = original_update(component, time_ms);
        depth--;
        return result;
    }
    static uint32_t last_stamp, frame_number;
    if (*frame_stamp != last_stamp) {
        last_stamp = *frame_stamp;
        frame_number++;
    }
    prefetching = *fix_enabled && prefetch_enabled && !(alternate_frames && (frame_number & 1));
    uint64_t started = __rdtsc();
    if (prefetching) prefetch_children(component);
    uintptr_t result = original_update(component, time_ms);
    stats.cycles[prefetching] += __rdtsc() - started;
    stats.frames[prefetching]++;
    depth--;
    return result;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, uiprefetch_checks);
    if (mismatch) return mismatch;
    fix_enabled = api->fix_enabled;
    frame_stamp = (volatile uint32_t *)(api->engine + FRAME_STAMP_RVA);
    const char *error = api->hook(api->engine + UPDATE_RVA, update_prologue, sizeof update_prologue, (const void *)update_hook, (void **)&original_update);
    if (!error) api->register_switch("uiprefetch", &prefetch_enabled, NULL);
    return error;
}
