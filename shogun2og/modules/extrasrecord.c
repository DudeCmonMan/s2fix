#include "../s2module.h"
#include "../checks/extrasrecord.h"
#include <stdint.h>
#include <intrin.h>

// The record step's extras loop records one object's indices one call at a time; with the generic thunk and a
// single-item recorder every call after the first only counts and appends its index, so those are done in bulk.
#define EXTRAS_LOOP_RVA 0x14bb6d0
#define MERGE_RVA 0x14e69b0
#define RECORD_THUNK_RVA 0x13d03c0
#define SINGLE_ITEM_RECORD_RVA 0x13cecf0
#define PUSH_BACK_RVA 0x98b40
#define DRAW_CONTEXT_RVA 0x1c9a930

typedef struct {
    uint32_t capacity, count, *data;
} IndexVector;

typedef uint32_t(__attribute__((thiscall)) *PassBitFn)(void *pass);
typedef BYTE(__attribute__((thiscall)) *FilterFn)(BYTE *object, uint32_t pass_bit);
typedef void(__attribute__((thiscall)) *RecordFn)(BYTE *object, uint32_t index, uint32_t flags);
typedef void(__attribute__((thiscall)) *MergeFn)(BYTE *list);
typedef void(__attribute__((thiscall)) *PushBackFn)(IndexVector *vector, const uint32_t *value);
typedef void(__attribute__((thiscall)) *ExtrasLoopFn)(BYTE *queue, void *pass);

__declspec(dllexport) volatile LONG bulk_enabled = 1;
__declspec(dllexport) volatile LONG64 bulk_loops, bulk_records;

static const BYTE extras_loop_prologue[] = {0x53, 0x8b, 0xd9, 0x8b, 0x4c, 0x24, 0x08};
static BYTE *engine;
static volatile LONG *fix_enabled;
static ExtrasLoopFn original_extras_loop;
// The bulk record writes the recorder and bucket count directly, so only main (the first caller) may take it.
static volatile LONG main_thread;

static BOOL on_main_thread(void) {
    LONG self = (LONG)GetCurrentThreadId();
    InterlockedCompareExchange(&main_thread, self, 0);
    return main_thread == self;
}

static void *method(void *object, int offset) {
    return (*(void ***)object)[offset / 4];
}

static void bulk_extras_loop(BYTE *queue, void *pass) {
    uint32_t pass_bit = ((PassBitFn)method(pass, 0x30))(pass);
    BYTE *object = *(BYTE **)(queue + 0x4);
    if (!object || !((FilterFn)method(object, 0xa4))(object, pass_bit)) return;
    if (queue[-0x1c]) ((MergeFn)(engine + MERGE_RVA))(queue - 0x40);
    uint32_t count = *(uint32_t *)(queue - 0x3c);
    if (!count) return;
    RecordFn record = (RecordFn)method(object, 0xa8);
    BYTE *item = *(BYTE **)(object + 0xc), *recorder = *(BYTE **)(item + 0x1c);
    if ((BYTE *)record != engine + RECORD_THUNK_RVA || method(recorder, 0x8) != engine + SINGLE_ITEM_RECORD_RVA) {
        for (uint32_t index = 0; index < count; index++) record(object, index, 0);
        return;
    }
    BYTE *context = *(BYTE **)(engine + DRAW_CONTEXT_RVA);
    uint32_t *bucket_records = &(*(uint32_t **)(context + 0x50))[*(uint32_t *)(item + 0x20)], before = *bucket_records;
    record(object, 0, 0);
    // Something else took the first record (a parallel renderer's record path): the rest go the same way.
    if (*bucket_records != before + 1) {
        for (uint32_t index = 1; index < count; index++) record(object, index, 0);
        return;
    }
    *bucket_records += count - 1;
    IndexVector *indices = (IndexVector *)(recorder + 0xc);
    PushBackFn push_back = (PushBackFn)(engine + PUSH_BACK_RVA);
    for (uint32_t index = 1; index < count; index++) {
        if (indices->count < indices->capacity) indices->data[indices->count++] = index;
        else push_back(indices, &index);
    }
    bulk_loops++;
    bulk_records += count;
}

static void __attribute__((thiscall)) extras_loop_hook(BYTE *queue, void *pass) {
    if (!*fix_enabled || !bulk_enabled || !on_main_thread()) original_extras_loop(queue, pass);
    else bulk_extras_loop(queue, pass);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, extrasrecord_checks);
    if (mismatch) return mismatch;
    engine = api->engine;
    fix_enabled = api->fix_enabled;
    const char *error = api->hook(engine + EXTRAS_LOOP_RVA, extras_loop_prologue, sizeof extras_loop_prologue, (const void *)extras_loop_hook,
                                  (void **)&original_extras_loop);
    if (!error) api->register_switch("extrasrecord", &bulk_enabled, NULL);
    return error;
}
