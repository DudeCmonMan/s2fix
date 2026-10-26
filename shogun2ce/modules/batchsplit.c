#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

// Splits the render-item task's large batches (scene vtable slot +0x100, sub_181b13fa0) into ordered chunks run on helper
// threads. The batch's setup runs once, then each chunk runs the batch's per-object loop over its own range. Chunk 0 appends
// straight into the scene's lists; later chunks append into private buffers that are copied into the lists in chunk order,
// so every list ends up with the same items in the same order as the serial loop. That copy is itself split: each
// chunk's place in each list is fixed first, then all workers copy chunks into place.
#define SCENE_BATCH_SLOT_RVA 0x23e4878
#define BATCH_RVA 0x1b13fa0
#define PER_OBJECT_RVA 0x1b11920
#define INSTANCING_RESERVE_RVA 0x307740
#define ITEM_SCALE_RVA 0x2174b10
#define LIST_APPEND_SLOT_RVA 0x23e4770
#define LIST_APPEND_RVA 0x1b0fb80
#define ENGINE_ALLOC_RVA 0x19fe50
#define ENGINE_FREE_RVA 0x19fe10
#define ENGINE_HEAP_RVA 0x2e3fd98
#define VARIANT_RIGID_SORT_RVA 0x3464f28
#define RENDER_ITEM_SIZE 0xb0
#define HELPERS 8
#define CHUNKS_PER_WORKER 4
#define MAX_CHUNKS ((HELPERS + 1) * CHUNKS_PER_WORKER)
#define MAX_LISTS 8
#define HELPER_SPIN_ROUNDS 20000

typedef void (*BatchFn)(BYTE *scene, const DWORD *handles, DWORD count, void *view, BYTE *a5, BYTE *a6, BYTE *a7, BYTE *a8, BYTE *a9, void *a10,
                        uint64_t a11);
typedef void (*PerObjectFn)(BYTE *object, DWORD part, void *view, void *view_again, BYTE *transform, BYTE *previous_transform, BYTE *extra,
                            BYTE *record, DWORD group, DWORD *flags, uint64_t zero11, float scale, uint64_t zero13, DWORD instance_slot,
                            uint64_t parallel, uint64_t zero16, uint64_t zero17, uint64_t a11, uint64_t zero19, float zero20);
typedef void (*InstancingReserveFn)(BYTE *instancing, void *views[2]);
typedef uint64_t (*AppendFn)(BYTE *list, const BYTE *item, uint64_t parallel);
typedef void *(*EngineAllocFn)(size_t size, void *heap);
typedef void (*EngineFreeFn)(void *pointer);

typedef struct {
    uint32_t capacity, count;
    BYTE *items;
} ItemList;

typedef struct {
    BYTE *list;
    uint32_t count, capacity, offset;
    BYTE *items;
} CapturedList;

typedef struct {
    CapturedList lists[MAX_LISTS];
    int list_count;
} ChunkOutput;

typedef struct {
    BYTE *scene;
    const DWORD *handles;
    void *view, *a10;
    BYTE *a5, *a6, *a7, *a8, *a9;
    uint64_t a11;
    DWORD count, chunk_items, initial_flags, instance_slot;
    LONG chunk_count;
    BOOL copying;
    volatile LONG done_chunks;
    unsigned int csr;
} Job;

typedef struct {
    LONG64 split_calls, split_items, chunks, serial_calls, instancing_calls, busy_fallbacks, captured_items, list_overflows, parallel_appends,
        split_ticks;
} SplitStats;

__declspec(dllexport) volatile SplitStats split_stats;
__declspec(dllexport) volatile LONG split_enabled = 1;
// Exported so they can be tuned while the game runs.
__declspec(dllexport) volatile LONG split_min_objects = 16, chunk_min_objects = 4;

static BYTE *engine;
static volatile LONG *fix_enabled;
static BatchFn original_batch;
static PerObjectFn per_object;
static InstancingReserveFn instancing_reserve;
static float item_scale;
static AppendFn original_append;
static EngineAllocFn engine_alloc;
static EngineFreeFn engine_free;
static DWORD capture_slot;
static ChunkOutput outputs[MAX_CHUNKS];
static Job job;
// High 32 bits: job generation; low 32 bits: next chunk to claim. Claims compare-and-swap the whole word, so a helper
// still finishing an old job can never take a chunk index from the next one.
static volatile LONG64 claim_word;
static volatile LONG busy;

static uint64_t append_hook(BYTE *list, const BYTE *item, uint64_t parallel) {
    ChunkOutput *output = TlsGetValue(capture_slot);
    if (!output) return original_append(list, item, parallel);
    if ((BYTE)parallel) InterlockedIncrement64(&split_stats.parallel_appends);
    CapturedList *captured = NULL;
    for (int i = 0; i < output->list_count && !captured; i++)
        if (output->lists[i].list == list) captured = &output->lists[i];
    if (!captured) {
        if (output->list_count == MAX_LISTS) {
            InterlockedIncrement64(&split_stats.list_overflows);
            return original_append(list, item, parallel);
        }
        captured = &output->lists[output->list_count++];
        captured->list = list;
        captured->count = 0;
    }
    if (captured->count == captured->capacity) {
        captured->capacity = captured->capacity ? captured->capacity * 2 : 256;
        captured->items = realloc(captured->items, (size_t)captured->capacity * RENDER_ITEM_SIZE);
    }
    memcpy(captured->items + (size_t)captured->count++ * RENDER_ITEM_SIZE, item, RENDER_ITEM_SIZE);
    return 0;
}

// The batch's loop. Flags carry "state changed since the last emitted item" bits into each item (+0x8c), so a later
// chunk's first item gets different flags than in the serial loop; only batches whose items' flags are never read are split.
static void run_objects(DWORD first, DWORD end) {
    DWORD flags = job.initial_flags, last_group = UINT32_MAX, last_material = UINT32_MAX;
    const DWORD *groups = (const DWORD *)job.a8;
    for (DWORD i = first; i < end; i++) {
        DWORD handle = job.handles[i], index = handle & 0xffff;
        BYTE *object = index < *(DWORD *)(job.scene + 0x34) ? (*(BYTE ***)(job.scene + 0x38))[index] : NULL;
        if (groups[i] != last_group) {
            flags |= 2;
            last_group = groups[i];
        }
        DWORD material = *(DWORD *)(object + 0x80);
        if (material != last_material || material == UINT32_MAX) {
            flags |= 4;
            last_material = material;
        }
        per_object(object, handle >> 16, job.view, job.view, job.a5 + i * 0x30, job.a6 + i * 0x30, job.a9 + i * 8, job.a7 + i * 0x5c, groups[i], &flags, 0,
                   item_scale, 0, job.instance_slot, 0, 0, 0, job.a11, 0, 0.0f);
    }
}

static void run_chunk(LONG chunk) {
    DWORD first = chunk * job.chunk_items;
    ChunkOutput *output = chunk ? &outputs[chunk] : NULL;
    if (output) output->list_count = 0;
    TlsSetValue(capture_slot, output);
    run_objects(first, min(first + job.chunk_items, job.count));
    TlsSetValue(capture_slot, NULL);
    InterlockedIncrement(&job.done_chunks);
}

static void copy_chunk(LONG chunk) {
    for (int i = 0; i < outputs[chunk].list_count; i++) {
        CapturedList *captured = &outputs[chunk].lists[i];
        ItemList *header = (ItemList *)(captured->list + 8);
        memcpy(header->items + (size_t)captured->offset * RENDER_ITEM_SIZE, captured->items, (size_t)captured->count * RENDER_ITEM_SIZE);
    }
    InterlockedIncrement(&job.done_chunks);
}

static void claim_chunks(LONG64 generation) {
    for (;;) {
        LONG64 claim = claim_word;
        if (claim >> 32 != generation || (LONG)(claim & 0xffffffff) >= job.chunk_count) return;
        if (InterlockedCompareExchange64(&claim_word, claim + 1, claim) != claim) continue;
        if (job.copying) copy_chunk((LONG)(claim & 0xffffffff));
        else run_chunk((LONG)(claim & 0xffffffff));
    }
}

// A helper can still hold a claim word it read before the last job ended. Moving to a new generation before the job is
// rewritten makes that stale compare-and-swap fail instead of claiming a chunk of the half-written job.
static void retire_generation(void) {
    InterlockedExchange64(&claim_word, ((claim_word >> 32) + 1) << 32 | INT32_MAX);
}

static LONG64 publish(LONG first_chunk) {
    LONG64 generation = (claim_word >> 32) + 1;
    InterlockedExchange64(&claim_word, generation << 32 | first_chunk);
    WakeByAddressAll((void *)&claim_word);
    return generation;
}

static DWORD WINAPI helper(LPVOID unused) {
    (void)unused;
    LONG64 seen = 0;
    for (;;) {
        LONG64 claim = claim_word;
        for (int round = 0; round < HELPER_SPIN_ROUNDS && claim >> 32 == seen; round++, claim = claim_word) _mm_pause();
        if (claim >> 32 == seen) {
            WaitOnAddress(&claim_word, &claim, sizeof claim, INFINITE);
            continue;
        }
        seen = claim >> 32;
        _mm_setcsr(job.csr);
        claim_chunks(seen);
    }
    return 0;
}

// Same growth and storage as the engine's own push: its allocator, its heap, and a {capacity, count, items} header at list + 8.
// Returns where the reserved items start; they are filled in by copy_chunk.
static uint32_t reserve_items(BYTE *list, uint32_t count) {
    ItemList *header = (ItemList *)(list + 8);
    if (header->count + count > header->capacity) {
        uint32_t capacity = max(header->capacity * 2, header->count + count);
        BYTE *grown = engine_alloc((size_t)capacity * RENDER_ITEM_SIZE, *(void **)(engine + ENGINE_HEAP_RVA));
        memcpy(grown, header->items, (size_t)header->count * RENDER_ITEM_SIZE);
        if (header->items) engine_free(header->items);
        header->items = grown;
        header->capacity = capacity;
    }
    uint32_t offset = header->count;
    header->count += count;
    return offset;
}

// The record step reads items' change flags only without instancing (sub_181b15590 at 0x181b15758) or with
// variant_rigid_sort off (0x181b159fe); otherwise it walks the items and derives its own.
static BOOL item_flags_unread(BYTE *scene) {
    return scene[0x2e0] && !scene[0x2e2] && *(DWORD *)(scene + 0x2d4) && engine[VARIANT_RIGID_SORT_RVA];
}

// The batch's instancing setup, done once per batch exactly as the batch function does before its loop.
static void prepare_batch(BYTE *scene, void *view) {
    scene[0x2e1] = 1;
    void *views[2] = {view, view};
    instancing_reserve(scene + 0x270, views);
    job.instance_slot = *(DWORD *)(scene + 0x274) - 1;
    job.initial_flags = 0x11e;
    InterlockedIncrement64(&split_stats.instancing_calls);
}

static void batch_hook(BYTE *scene, const DWORD *handles, DWORD count, void *view, BYTE *a5, BYTE *a6, BYTE *a7, BYTE *a8, BYTE *a9, void *a10,
                       uint64_t a11) {
    if (!*fix_enabled || !split_enabled || count < (DWORD)split_min_objects || !item_flags_unread(scene)) {
        InterlockedIncrement64(&split_stats.serial_calls);
        original_batch(scene, handles, count, view, a5, a6, a7, a8, a9, a10, a11);
        return;
    }
    if (InterlockedCompareExchange(&busy, 1, 0)) {
        InterlockedIncrement64(&split_stats.busy_fallbacks);
        original_batch(scene, handles, count, view, a5, a6, a7, a8, a9, a10, a11);
        return;
    }
    LARGE_INTEGER started, ended;
    QueryPerformanceCounter(&started);
    DWORD chunk_items = max((DWORD)chunk_min_objects, (count + MAX_CHUNKS - 1) / MAX_CHUNKS);
    retire_generation();
    job = (Job){scene, handles, view, a10, a5, a6, a7, a8, a9, a11, count, chunk_items, 0, 0, (LONG)((count + chunk_items - 1) / chunk_items), FALSE, 0,
                _mm_getcsr()};
    prepare_batch(scene, view);
    LONG64 generation = publish(1);
    run_chunk(0);
    claim_chunks(generation);
    while (job.done_chunks < job.chunk_count) _mm_pause();
    LONG64 captured_items = 0;
    for (LONG chunk = 1; chunk < job.chunk_count; chunk++)
        for (int i = 0; i < outputs[chunk].list_count; i++) {
            CapturedList *captured = &outputs[chunk].lists[i];
            captured->offset = reserve_items(captured->list, captured->count);
            captured_items += captured->count;
        }
    job.copying = TRUE;
    job.done_chunks = 1;
    generation = publish(1);
    claim_chunks(generation);
    while (job.done_chunks < job.chunk_count) _mm_pause();
    QueryPerformanceCounter(&ended);
    InterlockedIncrement64(&split_stats.split_calls);
    InterlockedAdd64(&split_stats.split_items, count);
    InterlockedAdd64(&split_stats.chunks, job.chunk_count);
    InterlockedAdd64(&split_stats.captured_items, captured_items);
    InterlockedAdd64(&split_stats.split_ticks, ended.QuadPart - started.QuadPart);
    busy = 0;
}

static const char *patch_slot(void **slot, void *replacement) {
    DWORD protection;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protection)) return "VirtualProtect on a vtable failed";
    InterlockedExchangePointer(slot, replacement);
    VirtualProtect(slot, sizeof *slot, protection, &protection);
    return NULL;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    engine = api->engine;
    fix_enabled = api->fix_enabled;
    void **batch_slot = (void **)(engine + SCENE_BATCH_SLOT_RVA), **append_slot = (void **)(engine + LIST_APPEND_SLOT_RVA);
    // Earlier modules' wrappers in these slots are replaced, not chained: chunks must reach the engine's own functions.
    if (*batch_slot != engine + BATCH_RVA) api->log_line("batchsplit: replacing another module's wrapper in the batch slot");
    if (*append_slot != engine + LIST_APPEND_RVA) api->log_line("batchsplit: replacing another module's wrapper in the list append slot");
    original_batch = (BatchFn)(engine + BATCH_RVA);
    original_append = (AppendFn)(engine + LIST_APPEND_RVA);
    per_object = (PerObjectFn)(engine + PER_OBJECT_RVA);
    instancing_reserve = (InstancingReserveFn)(engine + INSTANCING_RESERVE_RVA);
    item_scale = *(float *)(engine + ITEM_SCALE_RVA);
    engine_alloc = (EngineAllocFn)(engine + ENGINE_ALLOC_RVA);
    engine_free = (EngineFreeFn)(engine + ENGINE_FREE_RVA);
    capture_slot = TlsAlloc();
    if (capture_slot == TLS_OUT_OF_INDEXES) return "TlsAlloc failed";
    for (int i = 0; i < HELPERS; i++) {
        HANDLE thread = CreateThread(NULL, 0, helper, NULL, 0, NULL);
        if (!thread) return "CreateThread failed";
        CloseHandle(thread);
    }
    const char *error = patch_slot(append_slot, (void *)append_hook);
    return error ? error : patch_slot(batch_slot, (void *)batch_hook);
}
