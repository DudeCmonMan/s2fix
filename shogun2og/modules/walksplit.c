#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include "../spinwait.h"
#include "../checks/walksplit.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

// Splits the Variant node's draw-time walk (0x114bc310, thiscall) over a queue's sorted render items. Per item the walk
// filters by pass, picks the LOD (0x114ca3f0: reads only shared tables, tweakers and the camera) and records it through
// the LOD's vtable slot +0xa8, passing "state changed since the last recorded item" flags that decide instancing. Helpers
// filter and pick LODs for ordered chunks; the caller records each chunk, in order, as soon as it is ready, with the same
// flags, so recording and batching are unchanged. Each chunk starts its flags from scratch, so its first record is
// re-flagged against the previous chunk's last recorded item. The lookup has x87 code, so helpers take the caller's x87
// control word as well as its MXCSR.
#define WALK_RVA 0x14bc310
#define LOD_LOOKUP_RVA 0x14ca3f0
#define MERGE_RVA 0x14e74d0
#define ALTERNATE_RECORD_RVA 0x14ef000
#define ALTERNATE_MODE_RVA 0x1ca871b
#define RECORD_CALL_RVA 0x14bc497
#define RENDER_ITEM_SIZE 0x138
#define HELPERS 4
#define CHUNKS_PER_WORKER 12
#define MAX_CHUNKS ((HELPERS + 1) * CHUNKS_PER_WORKER)
#define HELPER_SPIN_ROUNDS 20000
// Set by the contention fix: a short spin covers the gaps between one frame's jobs, then the helpers sleep instead
// of spinning into the draw phase. The budget can be patched live.
__declspec(dllexport) volatile LONG short_spin_enabled;
__declspec(dllexport) volatile LONG short_spin_us = 50;

typedef void(__attribute__((thiscall)) *WalkFn)(BYTE *queue, DWORD pass_bit, float scale, DWORD alternate_list);
typedef BYTE *(__attribute__((thiscall)) *LodLookupFn)(BYTE *table, uint64_t mesh, float distance, DWORD alternate_list);
typedef void(__attribute__((thiscall)) *MergeFn)(BYTE *list);
typedef void(__attribute__((thiscall)) *RecordFn)(BYTE *lod, DWORD index, DWORD flags);
typedef void(__attribute__((thiscall)) *AlternateRecordFn)(BYTE *recorder, BYTE *lod, BYTE *item, DWORD flags, DWORD one);

typedef struct {
    uint64_t mesh;
    DWORD group, texture, flag_byte;
} ItemState;

typedef struct {
    BYTE *lod;
    DWORD index, flags;
} Record;

typedef struct {
    Record *records;
    DWORD count, capacity;
    ItemState first, last;
} ChunkOutput;

typedef struct {
    BYTE *items, *lod_table;
    DWORD count, chunk_items, pass_bit, alternate_list;
    LONG chunk_count;
    float scale;
    unsigned int csr;
    unsigned short x87;
} Job;

typedef struct {
    LONG64 split_calls, serial_calls, items, records, chunks;
    LONG64 verified_calls, verified_records, mismatched_records, mismatched_counts;
} WalkStats;

__declspec(dllexport) volatile WalkStats walk_stats;
// 0 runs the engine's walk; exported, and a switch, so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG split_enabled = 1;
// 1 runs the engine's walk with its record calls logged and compares them with what the split would record; a switch.
__declspec(dllexport) volatile LONG verify = 0;
__declspec(dllexport) volatile LONG split_min_items = 1024, chunk_min_items = 256;

static const ItemState no_item = {UINT64_MAX, UINT32_MAX, 0xfffffffe, UINT32_MAX};
static const BYTE walk_prologue[] = {0x83, 0xec, 0x20, 0x53, 0x55, 0x56};
static const BYTE record_call[] = {0xff, 0x90, 0xa8, 0x00, 0x00, 0x00};

static const S2Api *api;
// The split has one job and one helper set, so only main splits.
static volatile LONG main_thread;

static BOOL on_main_thread(void) {
    LONG self = (LONG)GetCurrentThreadId();
    InterlockedCompareExchange(&main_thread, self, 0);
    return main_thread == self;
}
static WalkFn original_walk;
static LodLookupFn lod_lookup;
static MergeFn merge;
static AlternateRecordFn alternate_record;
static ChunkOutput outputs[MAX_CHUNKS];
// The generation a chunk was last finished in.
static volatile LONG64 chunk_done[MAX_CHUNKS];
static Job job;
// High 32 bits: job generation; low 32 bits: next chunk to claim. Claims compare-and-swap the whole word, so a helper
// still finishing an old job can never take a chunk index from the next one.
static volatile LONG64 claim_word;
static Record *logged;
static DWORD logged_count, logged_capacity;
static BOOL record_call_patched;
BOOL walksplit_verifying;

static DWORD changed_flags(const ItemState *before, const ItemState *now) {
    DWORD flags = now->group != before->group ? 2 : 0;
    if (now->mesh != before->mesh) flags |= 4;
    if (now->texture != before->texture) flags |= 0x20;
    if (now->flag_byte != before->flag_byte) flags |= 0x100;
    return flags;
}

static void run_chunk(LONG chunk, LONG64 generation) {
    ChunkOutput *output = &outputs[chunk];
    ItemState previous = no_item;
    DWORD first = chunk * job.chunk_items, end = min(first + job.chunk_items, job.count);
    output->count = 0;
    for (DWORD i = first; i < end; i++) {
        BYTE *item = job.items + (size_t)i * RENDER_ITEM_SIZE;
        if (!(*(DWORD *)(item + 0xe4) >> (job.pass_bit & 31) & 1) || *(DWORD *)(item + 0xe0) == 1) continue;
        ItemState state = {*(uint64_t *)(item + 0xe8), *(DWORD *)(item + 0x118), *(DWORD *)(item + 0x124), item[0x134] != 0};
        if (!output->count) output->first = state;
        if (output->count == output->capacity) {
            output->capacity = output->capacity ? output->capacity * 2 : 1024;
            output->records = realloc(output->records, (size_t)output->capacity * sizeof *output->records);
        }
        output->records[output->count++] =
            (Record){lod_lookup(job.lod_table, state.mesh, *(float *)(item + 0xf0) * job.scale, job.alternate_list), i, changed_flags(&previous, &state)};
        previous = state;
    }
    output->last = previous;
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

static LONG64 start_job(BYTE *queue, BYTE *list, DWORD pass_bit, float scale, DWORD alternate_list) {
    DWORD count = *(DWORD *)(list + 4), chunk_items = max((DWORD)chunk_min_items, (count + MAX_CHUNKS - 1) / MAX_CHUNKS);
    LONG64 generation = (claim_word >> 32) + 1;
    // A helper preempted inside claim_chunks must see a new generation before job changes, or it could claim from it.
    InterlockedExchange64(&claim_word, generation << 32 | 0x7fffffff);
    unsigned short x87;
    __asm__ volatile("fnstcw %0" : "=m"(x87));
    job = (Job){*(BYTE **)(list + 8),   *(BYTE **)(queue + 0x78), count, chunk_items, pass_bit, alternate_list & 0xff, (LONG)((count + chunk_items - 1) / chunk_items),
                scale, _mm_getcsr(), x87};
    generation++;
    InterlockedExchange64(&claim_word, generation << 32 | 1);
    WakeByAddressAll((void *)&claim_word);
    // The caller takes the first chunk: it is needed first, and the caller just merged the items, so they are in its cache.
    if (job.chunk_count) run_chunk(0, generation);
    return generation;
}

// Waits for a chunk, running it here if no helper has claimed it yet, then re-flags its first record against the previous
// chunk's last.
static ChunkOutput *finish_chunk(LONG chunk, LONG64 generation, ItemState *previous) {
    LONG64 unclaimed = generation << 32 | chunk;
    while (chunk_done[chunk] != generation)
        if (claim_word == unclaimed && InterlockedCompareExchange64(&claim_word, unclaimed + 1, unclaimed) == unclaimed) run_chunk(chunk, generation);
        else _mm_pause();
    ChunkOutput *output = &outputs[chunk];
    if (output->count) {
        output->records[0].flags = changed_flags(previous, &output->first);
        *previous = output->last;
    }
    return output;
}

static void split_walk(BYTE *queue, BYTE *list, DWORD pass_bit, float scale, DWORD alternate_list) {
    LONG64 generation = start_job(queue, list, pass_bit, scale, alternate_list), records = 0;
    ItemState previous = no_item;
    for (LONG chunk = 0; chunk < job.chunk_count; chunk++) {
        ChunkOutput *output = finish_chunk(chunk, generation, &previous);
        for (DWORD i = 0; i < output->count; i++) {
            Record *record = &output->records[i];
            if (api->engine[ALTERNATE_MODE_RVA])
                alternate_record(queue + 0x1cc, record->lod, job.items + (size_t)record->index * RENDER_ITEM_SIZE, record->flags, 1);
            else
                ((RecordFn)(*(void ***)record->lod)[0xa8 / 4])(record->lod, record->index, record->flags);
        }
        records += output->count;
    }
    walk_stats.split_calls++;
    walk_stats.items += job.count;
    walk_stats.records += records;
    walk_stats.chunks += job.chunk_count;
}

void __attribute__((used)) walksplit_log_record(BYTE *lod, DWORD index, DWORD flags) {
    if (!walksplit_verifying) return;
    if (logged_count == logged_capacity) {
        logged_capacity = logged_capacity ? logged_capacity * 2 : 4096;
        logged = realloc(logged, (size_t)logged_capacity * sizeof *logged);
    }
    logged[logged_count++] = (Record){lod, index, flags};
}

// Replaces the walk's record call (call [eax+0xa8], with ecx the LOD and index, flags on the stack): logs them, then
// makes the same call, which returns past the nop the patch leaves. It stays in place after verifying.
void walksplit_record_spy(void);
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _walksplit_record_spy\n"
        "_walksplit_record_spy:\n"
        "  push eax\n"
        "  push ecx\n"
        "  push dword ptr [esp + 0x10]\n"
        "  push dword ptr [esp + 0x10]\n"
        "  push ecx\n"
        "  call _walksplit_log_record\n"
        "  add esp, 0xc\n"
        "  pop ecx\n"
        "  pop eax\n"
        "  jmp dword ptr [eax + 0xa8]\n"
        ".att_syntax\n");

// Patched from the walk's only caller thread, between walks, with the others suspended.
static BOOL patch_record_call(void) {
    BYTE *site = api->engine + RECORD_CALL_RVA, call[sizeof record_call];
    LONG offset = (LONG)((BYTE *)(void *)walksplit_record_spy - (site + 5));
    call[0] = 0xe8;
    memcpy(call + 1, &offset, sizeof offset);
    call[5] = 0x90;
    const char *error = api->patch(site, record_call, call, sizeof call);
    if (error) api->log_line("walksplit: verify unavailable: %s", error);
    return record_call_patched = !error;
}

// Predicts the walk's records without making them, runs the engine's walk with its record calls logged, and compares.
static void verify_walk(BYTE *queue, BYTE *list, DWORD pass_bit, float scale, DWORD alternate_list) {
    if (!record_call_patched && !patch_record_call()) {
        verify = 0;
        original_walk(queue, pass_bit, scale, alternate_list);
        return;
    }
    LONG64 generation = start_job(queue, list, pass_bit, scale, alternate_list);
    ItemState previous = no_item;
    for (LONG chunk = 0; chunk < job.chunk_count; chunk++) finish_chunk(chunk, generation, &previous);
    logged_count = 0;
    walksplit_verifying = !api->engine[ALTERNATE_MODE_RVA];
    original_walk(queue, pass_bit, scale, alternate_list);
    if (!walksplit_verifying) return;
    walksplit_verifying = FALSE;
    DWORD position = 0, mismatched = 0;
    for (LONG chunk = 0; chunk < job.chunk_count; chunk++)
        for (DWORD i = 0; i < outputs[chunk].count; i++, position++) {
            Record *predicted = &outputs[chunk].records[i];
            if (position >= logged_count || memcmp(predicted, &logged[position], sizeof *predicted)) mismatched++;
        }
    walk_stats.verified_calls++;
    walk_stats.verified_records += logged_count;
    walk_stats.mismatched_records += mismatched;
    if (position != logged_count) walk_stats.mismatched_counts++;
    logged_count = 0;
}

static void __attribute__((thiscall)) walk_hook(BYTE *queue, DWORD pass_bit, float scale, DWORD alternate_list) {
    if (!on_main_thread()) {
        original_walk(queue, pass_bit, scale, alternate_list);
        return;
    }
    BYTE *list = queue + ((BYTE)alternate_list ? 0x118 : 0x90);
    if (list[0x24]) merge(list);
    if (verify && *api->fix_enabled) {
        verify_walk(queue, list, pass_bit, scale, alternate_list);
        return;
    }
    if (!*api->fix_enabled || !split_enabled || *(DWORD *)(list + 4) < (DWORD)split_min_items) {
        walk_stats.serial_calls++;
        original_walk(queue, pass_bit, scale, alternate_list);
        return;
    }
    split_walk(queue, list, pass_bit, scale, alternate_list);
}

static void report(BOOL start) {
    if (start) {
        memset((void *)&walk_stats, 0, sizeof walk_stats);
        return;
    }
    volatile WalkStats *s = &walk_stats;
    api->log_line("  walksplit: %lld split walks (%.0f items, %.0f records, %.1f chunks each), %lld serial", s->split_calls,
                  s->split_calls ? (double)s->items / s->split_calls : 0.0, s->split_calls ? (double)s->records / s->split_calls : 0.0,
                  s->split_calls ? (double)s->chunks / s->split_calls : 0.0, s->serial_calls);
    if (s->verified_calls)
        api->log_line("  walksplit verify: %lld walks, %lld records, %lld mismatched records, %lld walks with a different record count", s->verified_calls,
                      s->verified_records, s->mismatched_records, s->mismatched_counts);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, walksplit_checks);
    if (mismatch) return mismatch;
    api = host;
    if (memcmp(api->engine + RECORD_CALL_RVA, record_call, sizeof record_call) || !s2_prologue_ready(api->engine + WALK_RVA, walk_prologue, sizeof walk_prologue))
        return "the walk differs from the analysed build";
    lod_lookup = (LodLookupFn)(api->engine + LOD_LOOKUP_RVA);
    merge = (MergeFn)(api->engine + MERGE_RVA);
    alternate_record = (AlternateRecordFn)(api->engine + ALTERNATE_RECORD_RVA);
    for (int i = 0; i < HELPERS; i++) {
        HANDLE thread = CreateThread(NULL, 0, helper, NULL, 0, NULL);
        if (!thread) return "CreateThread failed";
        CloseHandle(thread);
    }
    const char *error = api->hook(api->engine + WALK_RVA, walk_prologue, sizeof walk_prologue, (const void *)walk_hook, (void **)&original_walk);
    if (error) return error;
    api->register_switch("walksplit", &split_enabled, report);
    api->register_switch("walkverify", &verify, report);
    return NULL;
}
