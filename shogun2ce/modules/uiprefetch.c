#include "../s2module.h"
#include "../checks/uiprefetch.h"
#include <stdint.h>
#include <intrin.h>
#include <xmmintrin.h>

// The UI update (sub_18147f560, the components' vtbl+0x70) recurses over every component each frame and mostly waits on
// the first loads of each child. Entering a node prefetches the lines its children's updates read first (vtable, child
// list and state fields, controller), so a sibling's lines arrive while the earlier siblings' subtrees run. Only
// prefetches; what runs is unchanged.
#define UPDATE_RVA 0x147f560
#define FRAME_STAMP_RVA 0x2ead830
#define MAX_PREFETCHED_CHILDREN 64

typedef void (*UpdateFn)(BYTE *component, uint64_t dt_bits);

typedef struct {
    LONG64 cycles[2], frames[2];
} Stats;

__declspec(dllexport) volatile Stats stats;
// 0 never prefetches; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG prefetch_enabled = 1;
// 1 prefetches only on even frames and times the whole tree update both ways, for a same-session comparison.
__declspec(dllexport) volatile LONG alternate_frames;

static const BYTE update_prologue[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00};
static UpdateFn original_update;
static volatile LONG *fix_enabled;
static volatile uint64_t *frame_stamp;
static int depth, prefetching;

static void prefetch_children(BYTE *component) {
    uint32_t count = *(uint32_t *)(component + 0x134);
    BYTE **children = *(BYTE ***)(component + 0x138);
    if (count > MAX_PREFETCHED_CHILDREN) count = MAX_PREFETCHED_CHILDREN;
    for (uint32_t i = 0; i < count; i++) {
        const char *child = (const char *)children[i];
        _mm_prefetch(child, _MM_HINT_T0);
        _mm_prefetch(child + 0x100, _MM_HINT_T0);
        _mm_prefetch(child + 0x140, _MM_HINT_T0);
        _mm_prefetch(child + 0x5c0, _MM_HINT_T0);
    }
}

static void update_hook(BYTE *component, uint64_t dt_bits) {
    if (depth++) {
        if (prefetching) prefetch_children(component);
        original_update(component, dt_bits);
        depth--;
        return;
    }
    static uint64_t last_stamp, frame_number;
    if (*frame_stamp != last_stamp) {
        last_stamp = *frame_stamp;
        frame_number++;
    }
    prefetching = *fix_enabled && prefetch_enabled && !(alternate_frames && (frame_number & 1));
    uint64_t started = __rdtsc();
    if (prefetching) prefetch_children(component);
    original_update(component, dt_bits);
    stats.cycles[prefetching] += __rdtsc() - started;
    stats.frames[prefetching]++;
    depth--;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, uiprefetch_checks);
    if (mismatch) return mismatch;
    fix_enabled = api->fix_enabled;
    frame_stamp = (volatile uint64_t *)(api->engine + FRAME_STAMP_RVA);
    return api->hook(api->engine + UPDATE_RVA, update_prologue, sizeof update_prologue, (const void *)update_hook, (void **)&original_update);
}
