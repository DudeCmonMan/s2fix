#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include "../spinwait.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

// Splits the Variant node's soldier batch (0x114bab30, thiscall, from the entity display at 0x102b4b5d), which runs
// serially on main, into ordered chunks run on helper threads. The batch's instancing setup runs once, as the batch does
// it, then each chunk runs the batch's per-object loop over its own range, calling the engine's per-object function
// (0x114b9180) exactly as the loop does. Its only shared writes are the caller's flags word, which each chunk keeps for
// itself, and the list appends (0x114da700, slot +8 of the lists' vtable): chunk 0 appends straight into the node's lists,
// later chunks' appends are captured and replayed in chunk order through the engine's own serial append, so every list
// ends up with the same items in the same order. The one difference: each chunk starts the flags word afresh, and an item
// stores that word at +0xf4 until its chunk's first instanced emit. That field is read only without instancing
// (0x114bbf80) or with variant_rigid_sort off (0x114bbcf0), so the batch splits only with both on.
#define BATCH_RVA 0x14bab30
#define PER_OBJECT_RVA 0x14b9180
#define APPEND_RVA 0x14da700
#define PUSH_BACK_RVA 0x98b40
#define INITIAL_VALUE_RVA 0x15c2778
#define RIGID_SORT_RVA (0x1ca9588 + 0x5c)
#define RENDER_ITEM_SIZE 0x138
#define FLAGS_OFFSET 0xf4
#define HELPERS 6
#define CHUNKS_PER_WORKER 4
#define MAX_CHUNKS ((HELPERS + 1) * CHUNKS_PER_WORKER)
#define MAX_LISTS 4
#define HELPER_SPIN_ROUNDS 20000
// Set by the contention fix: a short spin covers the gaps between one frame's jobs, then the helpers sleep instead
// of spinning into the draw phase. The budget can be patched live.
__declspec(dllexport) volatile LONG short_spin_enabled;
__declspec(dllexport) volatile LONG short_spin_us = 50;

typedef void(__attribute__((thiscall)) *BatchFn)(BYTE *node, const DWORD *handles, DWORD count, void *view, BYTE *transforms, BYTE *extras,
                                                 const DWORD *groups, BYTE *a7, const float *values);
typedef void(__attribute__((thiscall)) *PerObjectFn)(BYTE *object, DWORD part, void *view, BYTE *transform, BYTE *a7, BYTE *extra, DWORD group,
                                                     DWORD *flags, DWORD zero8, float value, DWORD zero10, DWORD instance_slot, DWORD zero12,
                                                     DWORD zero13, DWORD zero14, DWORD zero15, DWORD zero16);
typedef DWORD(__attribute__((thiscall)) *AppendFn)(BYTE *list, const BYTE *item, DWORD parallel);
typedef void(__attribute__((thiscall)) *PushBackFn)(BYTE *vector, const DWORD *value);

typedef struct {
    BYTE *list;
    DWORD count, capacity;
    BYTE *items;
} CapturedList;

typedef struct {
    CapturedList lists[MAX_LISTS];
    int list_count;
    BOOL overflowed;
} Capture;

typedef struct {
    BYTE *node;
    const DWORD *handles, *groups;
    void *view;
    BYTE *transforms, *extras, *a7;
    const float *values;
    DWORD count, chunk_objects, instance_slot;
    LONG chunk_count;
    BOOL capture_all;
    unsigned int csr;
    unsigned short x87;
} Job;

typedef struct {
    LONG64 split_calls, split_objects, chunks, serial_calls, captured_items, ticks, replay_ticks, bulk_lists, item_lists;
    LONG64 verified_calls, verified_items, mismatched_items, mismatched_counts;
} SplitStats;

__declspec(dllexport) volatile SplitStats split_stats;
// 0 runs the engine's batch; exported, and a switch, so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG split_enabled = 1;
// 1 runs the engine's batch with its appends captured and compares them with what the split would append; a switch.
__declspec(dllexport) volatile LONG verify = 0;
__declspec(dllexport) volatile LONG split_min_objects = 16, chunk_min_objects = 4;
// 1 replays a captured list with one copy when its vector has room; a switch.
__declspec(dllexport) volatile LONG bulk_replay = 1;

static const BYTE batch_prologue[] = {0x83, 0xec, 0x1c, 0xf3, 0x0f, 0x10, 0x0d, 0x78, 0x27, 0x5c, 0x11};
static const BYTE append_prologue[] = {0x83, 0xec, 0x1c, 0x80, 0x7c, 0x24, 0x24, 0x00};
static const S2Api *api;
static BatchFn original_batch;
static AppendFn original_append;
static PerObjectFn per_object;
static DWORD capture_slot;
static Capture captures[MAX_CHUNKS], engine_capture;
static volatile LONG64 chunk_done[MAX_CHUNKS];
static Job job;
// High 32 bits: job generation; low 32 bits: next chunk to claim (see walksplit).
static volatile LONG64 claim_word;
static volatile LONG main_thread;
static BOOL busy;

static BOOL on_main_thread(void) {
    LONG self = (LONG)GetCurrentThreadId();
    InterlockedCompareExchange(&main_thread, self, 0);
    return main_thread == self;
}

static BOOL instancing_active(const BYTE *node) {
    return node[0x1c8] && !node[0x1ca] && *(const DWORD *)(node + 0x1c0);
}

static void capture_item(Capture *capture, BYTE *list, const BYTE *item) {
    CapturedList *captured = NULL;
    for (int i = 0; i < capture->list_count && !captured; i++)
        if (capture->lists[i].list == list) captured = &capture->lists[i];
    if (!captured) {
        if (capture->list_count == MAX_LISTS) {
            capture->overflowed = TRUE;
            return;
        }
        captured = &capture->lists[capture->list_count++];
        captured->list = list;
        captured->count = 0;
    }
    if (captured->count == captured->capacity) {
        captured->capacity = captured->capacity ? captured->capacity * 2 : 256;
        captured->items = realloc(captured->items, (size_t)captured->capacity * RENDER_ITEM_SIZE);
    }
    memcpy(captured->items + (size_t)captured->count++ * RENDER_ITEM_SIZE, item, RENDER_ITEM_SIZE);
}

// The callers ignore the returned index.
static DWORD __attribute__((thiscall)) append_hook(BYTE *list, const BYTE *item, DWORD parallel) {
    Capture *capture = TlsGetValue(capture_slot);
    if (!capture) return original_append(list, item, parallel);
    capture_item(capture, list, item);
    return 0;
}

// The serial append is a push_back (0x114dc2b0) onto the vector at the list's start, {capacity, count, data}, with an item
// copy equal to a memcpy (see itemcopy). Only a vector that would grow goes through the engine.
static void replay(Capture *capture) {
    for (int i = 0; i < capture->list_count; i++) {
        CapturedList *captured = &capture->lists[i];
        DWORD *vector = (DWORD *)captured->list;
        if (bulk_replay && *api->fix_enabled && vector[2] && vector[1] + captured->count <= vector[0]) {
            memcpy((BYTE *)(uintptr_t)vector[2] + (size_t)vector[1] * RENDER_ITEM_SIZE, captured->items, (size_t)captured->count * RENDER_ITEM_SIZE);
            vector[1] += captured->count;
            split_stats.bulk_lists++;
        } else {
            for (DWORD k = 0; k < captured->count; k++) original_append(captured->list, captured->items + (size_t)k * RENDER_ITEM_SIZE, 0);
            split_stats.item_lists++;
        }
        split_stats.captured_items += captured->count;
    }
}

// The batch's per-object loop (0x114baba6..0x114bacc1) over objects [first, end), with its change flags.
static void run_objects(DWORD first, DWORD end) {
    BYTE *node = job.node;
    DWORD flags = 0x11e, last_group = 0xffffffff, last_effect = 0xffffffff;
    float last_value = *(const float *)(api->engine + INITIAL_VALUE_RVA);
    if (first) {
        DWORD previous = job.handles[first - 1] & 0xffff;
        last_group = job.groups[first - 1];
        if (previous < *(DWORD *)(node + 0x1c)) last_effect = *(DWORD *)((*(BYTE ***)(node + 0x20))[previous] + 0x54);
        if (job.values) last_value = job.values[first - 1];
    }
    for (DWORD i = first; i < end; i++) {
        DWORD index = job.handles[i] & 0xffff, part = job.handles[i] >> 16;
        BYTE *object = index < *(DWORD *)(node + 0x1c) ? (*(BYTE ***)(node + 0x20))[index] : NULL;
        if (job.groups[i] != last_group) {
            flags |= 2;
            last_group = job.groups[i];
        }
        DWORD effect = *(DWORD *)(object + 0x54);
        if (effect != last_effect || effect == 0xffffffff) {
            flags |= 4;
            last_effect = effect;
        }
        float value = 0.0f;
        if (job.values) {
            value = job.values[i];
            if (!(value == last_value)) {
                flags |= 8;
                last_value = value;
            }
        }
        per_object(object, part, job.view, job.transforms + (size_t)i * 0x30, job.a7 + (size_t)i * 8, job.extras + (size_t)i * 0x5c, job.groups[i],
                   &flags, 0, value, 0, job.instance_slot, 0, 0, 0, 0, 0);
    }
}

static void run_chunk(LONG chunk, LONG64 generation) {
    DWORD first = chunk * job.chunk_objects, end = min(first + job.chunk_objects, job.count);
    Capture *capture = &captures[chunk];
    capture->list_count = 0;
    capture->overflowed = FALSE;
    BOOL captured = chunk || job.capture_all;
    if (captured) TlsSetValue(capture_slot, capture);
    run_objects(first, end);
    if (captured) TlsSetValue(capture_slot, NULL);
    InterlockedExchange64(&chunk_done[chunk], generation);
}

static void claim_chunks(LONG64 generation) {
    for (;;) {
        LONG64 claim = claim_word;
        if (claim >> 32 != generation || (LONG)(claim & 0xffffffff) >= job.chunk_count) return;
        if (InterlockedCompareExchange64(&claim_word, claim + 1, claim) != claim) continue;
        run_chunk((LONG)(claim & 0xffffffff), generation);
    }
}

static DWORD WINAPI helper(LPVOID unused) {
    (void)unused;
    LONG64 seen = 0;
    for (;;) {
        LONG64 claim = claim_word;
        if (short_spin_enabled && *api->fix_enabled) {
            LONGLONG deadline = spin_deadline(short_spin_us);
            for (int round = 0; claim >> 32 == seen && !spin_expired(round, deadline); round++, claim = claim_word) _mm_pause();
        } else {
            for (int round = 0; round < HELPER_SPIN_ROUNDS && claim >> 32 == seen; round++, claim = claim_word) _mm_pause();
        }
        if (claim >> 32 == seen) {
            WaitOnAddress(&claim_word, &claim, sizeof claim, INFINITE);
            continue;
        }
        seen = claim >> 32;
        unsigned short x87 = job.x87;
        _mm_setcsr(job.csr);
        __asm__ volatile("fldcw %0" : : "m"(x87));
        claim_chunks(seen);
    }
    return 0;
}

// Runs every chunk, helpers and caller together. The caller takes chunk 0, whose appends go straight into the lists, then
// whatever is still unclaimed.
static void run_job(BYTE *node, const DWORD *handles, DWORD count, void *view, BYTE *transforms, BYTE *extras, const DWORD *groups, BYTE *a7,
                    const float *values, DWORD instance_slot, BOOL capture_all) {
    DWORD chunk_objects = max((DWORD)chunk_min_objects, (count + MAX_CHUNKS - 1) / MAX_CHUNKS);
    LONG64 generation = (claim_word >> 32) + 1;
    // A helper preempted inside claim_chunks must see a new generation before job changes, or it could claim from it.
    InterlockedExchange64(&claim_word, generation << 32 | 0x7fffffff);
    unsigned short x87;
    __asm__ volatile("fnstcw %0" : "=m"(x87));
    job = (Job){node,   handles, groups, view, transforms, extras, a7, values, count, chunk_objects, instance_slot, (LONG)((count + chunk_objects - 1) / chunk_objects),
                capture_all, _mm_getcsr(), x87};
    generation++;
    InterlockedExchange64(&claim_word, generation << 32 | 1);
    WakeByAddressAll((void *)&claim_word);
    run_chunk(0, generation);
    claim_chunks(generation);
    for (LONG chunk = 0; chunk < job.chunk_count; chunk++)
        while (chunk_done[chunk] != generation) _mm_pause();
}

// The batch's instancing setup (0x114bab5b..0x114bab8a), with instancing known to be active.
static DWORD prepare_instancing(BYTE *node, void *view) {
    DWORD value = (DWORD)(uintptr_t)view;
    node[0x1c9] = 1;
    ((PushBackFn)(api->engine + PUSH_BACK_RVA))(node + 0x17c, &value);
    return *(DWORD *)(node + 0x180) - 1;
}

static BOOL same_item(const BYTE *a, const BYTE *b) {
    return !memcmp(a, b, FLAGS_OFFSET) && !memcmp(a + FLAGS_OFFSET + 4, b + FLAGS_OFFSET + 4, 0x121 - FLAGS_OFFSET - 4) &&
           !memcmp(a + 0x124, b + 0x124, 0x135 - 0x124);
}

// Runs the engine's batch with its appends captured, predicts the split's appends, compares, then appends the engine's.
static void verify_batch(BYTE *node, const DWORD *handles, DWORD count, void *view, BYTE *transforms, BYTE *extras, const DWORD *groups, BYTE *a7,
                         const float *values) {
    engine_capture.list_count = 0;
    engine_capture.overflowed = FALSE;
    TlsSetValue(capture_slot, &engine_capture);
    original_batch(node, handles, count, view, transforms, extras, groups, a7, values);
    TlsSetValue(capture_slot, NULL);
    run_job(node, handles, count, view, transforms, extras, groups, a7, values, *(DWORD *)(node + 0x180) - 1, TRUE);
    LONG64 items = 0, mismatched = 0;
    BOOL counts_differ = engine_capture.overflowed;
    for (int i = 0; i < engine_capture.list_count; i++) {
        CapturedList *engine_list = &engine_capture.lists[i];
        DWORD position = 0;
        for (LONG chunk = 0; chunk < job.chunk_count; chunk++) {
            Capture *capture = &captures[chunk];
            counts_differ |= capture->overflowed;
            for (int l = 0; l < capture->list_count; l++) {
                if (capture->lists[l].list != engine_list->list) continue;
                for (DWORD k = 0; k < capture->lists[l].count; k++, position++)
                    if (position >= engine_list->count || !same_item(capture->lists[l].items + (size_t)k * RENDER_ITEM_SIZE,
                                                                     engine_list->items + (size_t)position * RENDER_ITEM_SIZE))
                        mismatched++;
            }
        }
        counts_differ |= position != engine_list->count;
        items += engine_list->count;
    }
    for (LONG chunk = 0; chunk < job.chunk_count; chunk++)
        for (int l = 0; l < captures[chunk].list_count; l++) {
            BOOL known = FALSE;
            for (int i = 0; i < engine_capture.list_count; i++) known |= engine_capture.lists[i].list == captures[chunk].lists[l].list;
            counts_differ |= !known && captures[chunk].lists[l].count;
        }
    replay(&engine_capture);
    split_stats.verified_calls++;
    split_stats.verified_items += items;
    split_stats.mismatched_items += mismatched;
    if (counts_differ) split_stats.mismatched_counts++;
}

static void __attribute__((thiscall)) batch_hook(BYTE *node, const DWORD *handles, DWORD count, void *view, BYTE *transforms, BYTE *extras,
                                                 const DWORD *groups, BYTE *a7, const float *values) {
    if (!*api->fix_enabled || (!split_enabled && !verify) || count < (DWORD)split_min_objects || busy || !on_main_thread() || !instancing_active(node) ||
        !api->engine[RIGID_SORT_RVA]) {
        split_stats.serial_calls++;
        original_batch(node, handles, count, view, transforms, extras, groups, a7, values);
        return;
    }
    busy = TRUE;
    if (verify) {
        verify_batch(node, handles, count, view, transforms, extras, groups, a7, values);
        busy = FALSE;
        return;
    }
    LARGE_INTEGER started, replaying, ended;
    QueryPerformanceCounter(&started);
    run_job(node, handles, count, view, transforms, extras, groups, a7, values, prepare_instancing(node, view), FALSE);
    QueryPerformanceCounter(&replaying);
    for (LONG chunk = 1; chunk < job.chunk_count; chunk++) {
        // Lists beyond MAX_LISTS were not captured; nothing the batch appends to comes close.
        replay(&captures[chunk]);
    }
    QueryPerformanceCounter(&ended);
    split_stats.replay_ticks += ended.QuadPart - replaying.QuadPart;
    split_stats.split_calls++;
    split_stats.split_objects += count;
    split_stats.chunks += job.chunk_count;
    split_stats.ticks += ended.QuadPart - started.QuadPart;
    busy = FALSE;
}

static void report(BOOL start) {
    if (start) {
        memset((void *)&split_stats, 0, sizeof split_stats);
        return;
    }
    volatile SplitStats *s = &split_stats;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    api->log_line("  batchsplit: %lld split batches (%.0f objects, %.1f chunks, %.1f us each, %.1f of it replaying), %lld items replayed (%lld lists in "
                  "one copy, %lld item by item), %lld serial",
                  s->split_calls, s->split_calls ? (double)s->split_objects / s->split_calls : 0.0, s->split_calls ? (double)s->chunks / s->split_calls : 0.0,
                  s->split_calls ? 1e6 * s->ticks / frequency.QuadPart / s->split_calls : 0.0,
                  s->split_calls ? 1e6 * s->replay_ticks / frequency.QuadPart / s->split_calls : 0.0, s->captured_items, s->bulk_lists, s->item_lists,
                  s->serial_calls);
    if (s->verified_calls)
        api->log_line("  batchsplit verify: %lld batches, %lld items, %lld differ from the engine's, %lld batches with different lists or counts",
                      s->verified_calls, s->verified_items, s->mismatched_items, s->mismatched_counts);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    if (!s2_prologue_ready(api->engine + BATCH_RVA, batch_prologue, 3) || !s2_prologue_ready(api->engine + APPEND_RVA, append_prologue, sizeof append_prologue))
        return "the batch or the list append differs from the analysed build";
    capture_slot = TlsAlloc();
    if (capture_slot == TLS_OUT_OF_INDEXES) return "TlsAlloc failed";
    per_object = (PerObjectFn)(api->engine + PER_OBJECT_RVA);
    for (int i = 0; i < HELPERS; i++) {
        HANDLE thread = CreateThread(NULL, 0, helper, NULL, 0, NULL);
        if (!thread) return "CreateThread failed";
        CloseHandle(thread);
    }
    const char *error = api->hook(api->engine + APPEND_RVA, append_prologue, sizeof append_prologue, (const void *)append_hook, (void **)&original_append);
    if (!error)
        error = api->hook(api->engine + BATCH_RVA, batch_prologue, sizeof batch_prologue, (const void *)batch_hook, (void **)&original_batch);
    if (error) return error;
    api->register_switch("batchsplit", &split_enabled, report);
    api->register_switch("batchverify", &verify, report);
    api->register_switch("bulkreplay", &bulk_replay, report);
    return NULL;
}
