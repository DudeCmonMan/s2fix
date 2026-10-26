#include "../s2module.h"
#include <stdint.h>
#include <intrin.h>

// The record step's extras loop (sub_181b14c20) records indices 0..N-1 of one object (~25-30K a frame) one call at a
// time: object vtbl+0x158 -> thunk 0x1819d2d60 -> sub_1819d2440 (register the recorder in its bucket once, count the
// record) -> recorder vtbl+0x10 (append the index). With the generic thunk and a single-item recorder, every call after
// the first only adds 1 to the bucket's counter and appends its index, so those are done in bulk; the index vector still
// grows through the engine's own push_back, so its allocations are the same.
#define EXTRAS_LOOP_RVA 0x1b14c20
#define MERGE_RVA 0x1b4ca40
#define RECORD_THUNK_RVA 0x19d2d60
#define SINGLE_ITEM_RECORD_RVA 0x19d24c0
#define PUSH_BACK_RVA 0x2714f0
#define SCENE_CONTEXT_RVA 0x34578a8

typedef struct {
    uint32_t capacity, count, *data;
} IndexVector;

typedef uint32_t (*PassBitFn)(void *pass);
typedef BYTE (*FilterFn)(BYTE *object, uint32_t pass_bit);
typedef void (*RecordFn)(BYTE *object, uint32_t index, uint32_t flags);
typedef void (*MergeFn)(BYTE *list);
typedef void (*PushBackFn)(IndexVector *vector, const uint32_t *value);
typedef void (*ExtrasLoopFn)(BYTE *queue, void *pass);

// 0 runs the engine's loop; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG bulk_enabled = 1;
// 1 alternates the engine's loop and the bulk one call by call, timing both under the same conditions.
__declspec(dllexport) volatile LONG alternate_for_comparison;
__declspec(dllexport) volatile LONG64 loop_calls[2], loop_cycles[2];

static const BYTE extras_loop_prologue[] = {0x40, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x02, 0x48, 0x8b, 0xf1, 0x48, 0x8b, 0xca};
static BYTE *engine;
static volatile LONG *fix_enabled;
// The bulk record writes the engine's recorder and bucket count itself, which a parallel record on another thread
// (the overhaul's) must not: only main takes it.
static volatile LONG main_thread;

static BOOL on_main_thread(void) {
    LONG self = (LONG)GetCurrentThreadId();
    InterlockedCompareExchange(&main_thread, self, 0);
    return main_thread == self;
}
static ExtrasLoopFn original_extras_loop;

static void *method(void *object, int offset) {
    return (*(void ***)object)[offset / 8];
}

static void bulk_extras_loop(BYTE *queue, void *pass);

static void extras_loop_hook(BYTE *queue, void *pass) {
    if (!on_main_thread()) {
        original_extras_loop(queue, pass);
        return;
    }
    BOOL bulk = *fix_enabled && bulk_enabled && !(alternate_for_comparison && loop_calls[0] < loop_calls[1]);
    uint64_t started = __rdtsc();
    if (bulk) bulk_extras_loop(queue, pass);
    else original_extras_loop(queue, pass);
    loop_cycles[bulk] += __rdtsc() - started;
    loop_calls[bulk]++;
}

static void bulk_extras_loop(BYTE *queue, void *pass) {
    uint32_t pass_bit = ((PassBitFn)method(pass, 0x78))(pass);
    BYTE *object = *(BYTE **)(queue + 8);
    if (!object || !((FilterFn)method(object, 0x150))(object, pass_bit)) return;
    if (queue[-0x30]) ((MergeFn)(engine + MERGE_RVA))(queue - 0x60);
    uint32_t count = *(uint32_t *)(queue - 0x5c);
    if (!count) return;
    RecordFn record = (RecordFn)method(object, 0x158);
    BYTE *item = *(BYTE **)(object + 0x18), *recorder = *(BYTE **)(item + 0x38);
    if ((BYTE *)record != engine + RECORD_THUNK_RVA || method(recorder, 0x10) != engine + SINGLE_ITEM_RECORD_RVA) {
        for (uint32_t index = 0; index < count; index++) record(object, index, 0);
        return;
    }
    BYTE *scene = *(BYTE **)(engine + SCENE_CONTEXT_RVA);
    uint32_t *bucket_records = &(*(uint32_t **)(scene + 0x70))[*(uint32_t *)(item + 0x40)], before = *bucket_records;
    record(object, 0, 0);
    // A parallel record (the overhaul's) kept the first record from the engine: the rest go the same way.
    if (*bucket_records != before + 1) {
        for (uint32_t index = 1; index < count; index++) record(object, index, 0);
        return;
    }
    *bucket_records += count - 1;
    IndexVector *indices = (IndexVector *)(recorder + 0x18);
    PushBackFn push_back = (PushBackFn)(engine + PUSH_BACK_RVA);
    for (uint32_t index = 1; index < count; index++) {
        if (indices->count < indices->capacity) indices->data[indices->count++] = index;
        else push_back(indices, &index);
    }
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    engine = api->engine;
    fix_enabled = api->fix_enabled;
    return api->hook(engine + EXTRAS_LOOP_RVA, extras_loop_prologue, sizeof extras_loop_prologue, (const void *)extras_loop_hook,
                     (void **)&original_extras_loop);
}
