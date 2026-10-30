#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include "../checks/walksplit.h"
#include "../spinwait.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include <xmmintrin.h>

// Splits the serial draw-time batching walk: helpers filter items and pick LODs for ordered chunks; the caller
// records each chunk in order with the same flags, re-flagging a chunk's first record against the previous one's last.
#define WALK_RVA 0x1b15d00
#define LOD_LOOKUP_RVA 0x1b27b30
#define MERGE_RVA 0x1b4d9d0
#define ALTERNATE_RECORD_RVA 0x1baa2f0
#define ALTERNATE_MODE_RVA 0x345fe93
#define RECORD_CALL_RVA 0x1b15edf
#define RENDER_ITEM_SIZE 0xb0
#define HELPERS 4
#define CHUNKS_PER_WORKER 12
#define MAX_CHUNKS ((HELPERS + 1) * CHUNKS_PER_WORKER)
#define HELPER_SPIN_ROUNDS 20000
// Set by the contention fix: spin briefly between a frame's jobs, then sleep instead of spinning into the draw phase.
__declspec(dllexport) volatile LONG short_spin_enabled;
__declspec(dllexport) volatile LONG short_spin_us = 50;

typedef void (*WalkFn)(BYTE *queue, DWORD pass_bit, float scale, BYTE alternate_list);
typedef BYTE *(*LodLookupFn)(BYTE *table, uint64_t mesh, float distance, uint64_t alternate_list);
typedef void (*MergeFn)(BYTE *list);
typedef void (*RecordFn)(BYTE *lod, DWORD index, DWORD flags);
typedef void (*AlternateRecordFn)(BYTE *recorder, BYTE *lod, BYTE *item, DWORD flags, BYTE one);

typedef struct {
    uint64_t mesh;
    DWORD material, nibble, high_bit;
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
    DWORD count, chunk_items, pass_bit;
    LONG chunk_count;
    float scale;
    BYTE alternate_list, two_kind_pass, second_kind;
    unsigned int csr;
} Job;

typedef struct {
    LONG64 split_calls, serial_calls, items, records, chunks;
    LONG64 compared_calls[2], compared_items[2], compared_cycles[2];
    LONG64 verified_calls, verified_records, mismatched_records, mismatched_counts;
} WalkStats;

__declspec(dllexport) volatile WalkStats walk_stats;
__declspec(dllexport) volatile LONG split_enabled = 1;
// 1 runs the engine's or the split walk at random per call, timing both.
__declspec(dllexport) volatile LONG alternate_for_comparison = 0;
__declspec(dllexport) volatile LONG verify = 0;
__declspec(dllexport) volatile LONG split_min_items = 1024, chunk_min_items = 256;

static const ItemState no_item = {UINT64_MAX, UINT32_MAX, 0xfffffffe, UINT32_MAX};
static const BYTE walk_prologue[] = {0x44, 0x88, 0x4c, 0x24, 0x20, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x57, 0x41, 0x54};
static const BYTE record_call[] = {0x49, 0x8b, 0x02, 0x8b, 0xd5, 0x49, 0x8b, 0xca, 0xff, 0x90, 0x58, 0x01, 0x00, 0x00};

static const S2Api *api;
static BYTE *engine;
static volatile LONG *fix_enabled;
// The split has one job and one helper set, so only main splits; the overhaul's parallel records walk on other threads.
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
static volatile LONG64 chunk_done[MAX_CHUNKS];
static Job job;
// High 32 bits: job generation; low 32 bits: next chunk to claim. Swapped as one, so a stale claim fails.
static volatile LONG64 claim_word;
static Record *logged;
static DWORD logged_count, logged_capacity;
static BOOL record_call_patched, verifying;

static DWORD changed_flags(const ItemState *before, const ItemState *now) {
    DWORD flags = now->material != before->material ? 2 : 0;
    if (now->mesh != before->mesh) flags |= 4;
    if (now->nibble != before->nibble) flags |= 0x20;
    if (now->high_bit != before->high_bit) flags |= 0x100;
    return flags;
}

static void run_chunk(LONG chunk, LONG64 generation) {
    ChunkOutput *output = &outputs[chunk];
    ItemState previous = no_item;
    DWORD first = chunk * job.chunk_items, end = min(first + job.chunk_items, job.count);
    output->count = 0;
    for (DWORD i = first; i < end; i++) {
        BYTE *item = job.items + (size_t)i * RENDER_ITEM_SIZE;
        DWORD packed = *(DWORD *)(item + 0xa0);
        if (job.two_kind_pass && (((packed >> 4) & 7) != 3) != job.second_kind) continue;
        if (!(*(DWORD *)(item + 0x74) >> (job.pass_bit & 31) & 1) || *(DWORD *)(item + 0x70) == 1) continue;
        ItemState state = {*(uint64_t *)(item + 0x78), *(DWORD *)(item + 0x80), packed & 0xf, packed >> 7 & 1};
        if (!output->count) output->first = state;
        if (output->count == output->capacity) {
            output->capacity = output->capacity ? output->capacity * 2 : 1024;
            output->records = realloc(output->records, (size_t)output->capacity * sizeof *output->records);
        }
        output->records[output->count++] =
            (Record){lod_lookup(job.lod_table, state.mesh, job.scale * *(float *)(item + 0x88), job.alternate_list), i, changed_flags(&previous, &state)};
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
        if (short_spin_enabled && *fix_enabled) {
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
        _mm_setcsr(job.csr);
        claim_chunks(seen);
    }
    return 0;
}

static LONG64 start_job(BYTE *queue, BYTE *list, DWORD pass_bit, float scale, BYTE alternate_list) {
    DWORD count = *(DWORD *)(list + 4), pass = *(DWORD *)(queue + 0x26c), chunk_items = max((DWORD)chunk_min_items, (count + MAX_CHUNKS - 1) / MAX_CHUNKS);
    LONG64 generation = (claim_word >> 32) + 1;
    // A helper preempted inside claim_chunks must see a new generation before job changes, or it could claim from it.
    InterlockedExchange64(&claim_word, generation << 32 | 0x7fffffff);
    job = (Job){*(BYTE **)(list + 8), *(BYTE **)(queue + 0xe0), count, chunk_items, pass_bit, (LONG)((count + chunk_items - 1) / chunk_items), scale,
                alternate_list, pass - 0x16 <= 1, pass == 0x17, _mm_getcsr()};
    generation++;
    InterlockedExchange64(&claim_word, generation << 32 | 1);
    WakeByAddressAll((void *)&claim_word);
    // The caller takes the first chunk: it is needed first, and the merge just left its items in the caller's cache.
    if (job.chunk_count) run_chunk(0, generation);
    return generation;
}

// Runs a chunk here if no helper has claimed it yet, then re-flags its first record against the previous chunk's last.
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

static void split_walk(BYTE *queue, BYTE *list, DWORD pass_bit, float scale, BYTE alternate_list) {
    LONG64 generation = start_job(queue, list, pass_bit, scale, alternate_list), records = 0;
    ItemState previous = no_item;
    for (LONG chunk = 0; chunk < job.chunk_count; chunk++) {
        ChunkOutput *output = finish_chunk(chunk, generation, &previous);
        for (DWORD i = 0; i < output->count; i++) {
            Record *record = &output->records[i];
            if (engine[ALTERNATE_MODE_RVA])
                alternate_record(queue + 0x2e8, record->lod, job.items + (size_t)record->index * RENDER_ITEM_SIZE, record->flags, 1);
            else
                (*(RecordFn **)record->lod)[0x158 / 8](record->lod, record->index, record->flags);
        }
        records += output->count;
    }
    InterlockedIncrement64(&walk_stats.split_calls);
    InterlockedAdd64(&walk_stats.items, job.count);
    InterlockedAdd64(&walk_stats.records, records);
    InterlockedAdd64(&walk_stats.chunks, job.chunk_count);
}

void log_record(BYTE *lod, DWORD index, DWORD flags) {
    if (!verifying) return;
    if (logged_count == logged_capacity) {
        logged_capacity = logged_capacity ? logged_capacity * 2 : 4096;
        logged = realloc(logged, (size_t)logged_capacity * sizeof *logged);
    }
    logged[logged_count++] = (Record){lod, index, flags};
}

// Replaces the walk's record call (call [rax+0x158]; lod r10, index ebp, flags r8d); stays in place after verifying.
void record_spy(void);
__asm__(".text\n"
        ".intel_syntax noprefix\n"
        "record_spy:\n"
        "  push r8\n"
        "  push r10\n"
        "  sub rsp, 0x28\n"
        "  mov rcx, r10\n"
        "  mov edx, ebp\n"
        "  call log_record\n"
        "  add rsp, 0x28\n"
        "  pop rcx\n"
        "  pop r8\n"
        "  mov edx, ebp\n"
        "  mov rax, [rcx]\n"
        "  jmp [rax+0x158]\n"
        ".att_syntax\n");

// Patched from the walk's only caller thread, between walks, with the others suspended.
static BOOL patch_record_call(void) {
    BYTE *site = engine + RECORD_CALL_RVA, patch[sizeof record_call] = {0x49, 0xbb};
    void *spy = (void *)record_spy;
    memcpy(patch + 2, &spy, sizeof spy);
    memcpy(patch + 10, (BYTE[]){0x41, 0xff, 0xd3, 0x90}, 4);
    const char *error = api->patch(site, record_call, patch, sizeof patch);
    if (error) api->log_line("walksplit: verify unavailable: %s", error);
    return record_call_patched = !error;
}

// The engine's walk makes the records; the split's are only predicted and compared.
static void verify_walk(BYTE *queue, BYTE *list, DWORD pass_bit, float scale, BYTE alternate_list) {
    if (!record_call_patched && !patch_record_call()) {
        verify = 0;
        return;
    }
    LONG64 generation = start_job(queue, list, pass_bit, scale, alternate_list);
    ItemState previous = no_item;
    for (LONG chunk = 0; chunk < job.chunk_count; chunk++) finish_chunk(chunk, generation, &previous);
    logged_count = 0;
    verifying = TRUE;
    original_walk(queue, pass_bit, scale, alternate_list);
    verifying = FALSE;
    DWORD position = 0, mismatched = 0;
    for (LONG chunk = 0; chunk < job.chunk_count; chunk++)
        for (DWORD i = 0; i < outputs[chunk].count; i++, position++) {
            Record *predicted = &outputs[chunk].records[i];
            if (position >= logged_count || memcmp(predicted, &logged[position], sizeof *predicted)) mismatched++;
        }
    InterlockedIncrement64(&walk_stats.verified_calls);
    InterlockedAdd64(&walk_stats.verified_records, logged_count);
    InterlockedAdd64(&walk_stats.mismatched_records, mismatched);
    if (position != logged_count) InterlockedIncrement64(&walk_stats.mismatched_counts);
    logged_count = 0;
}

static void walk_hook(BYTE *queue, DWORD pass_bit, float scale, BYTE alternate_list) {
    if (!on_main_thread()) {
        original_walk(queue, pass_bit, scale, alternate_list);
        return;
    }
    BYTE *list = queue + (alternate_list ? 0x1d0 : 0x100);
    if (list[0x30]) merge(list);
    if (verify) {
        verify_walk(queue, list, pass_bit, scale, alternate_list);
        return;
    }
    DWORD count = *(DWORD *)(list + 4);
    if (!*fix_enabled || !split_enabled || count < (DWORD)split_min_items) {
        InterlockedIncrement64(&walk_stats.serial_calls);
        original_walk(queue, pass_bit, scale, alternate_list);
        return;
    }
    if (!alternate_for_comparison) {
        split_walk(queue, list, pass_bit, scale, alternate_list);
        return;
    }
    int use_engine = __rdtsc() >> 4 & 1;
    uint64_t started = __rdtsc();
    if (use_engine) original_walk(queue, pass_bit, scale, alternate_list);
    else split_walk(queue, list, pass_bit, scale, alternate_list);
    InterlockedAdd64(&walk_stats.compared_cycles[use_engine], __rdtsc() - started);
    InterlockedIncrement64(&walk_stats.compared_calls[use_engine]);
    InterlockedAdd64(&walk_stats.compared_items[use_engine], count);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    const char *mismatch = S2_CHECK_ENGINE(api, walksplit_checks);
    if (mismatch) return mismatch;
    engine = api->engine;
    fix_enabled = api->fix_enabled;
    lod_lookup = (LodLookupFn)(engine + LOD_LOOKUP_RVA);
    merge = (MergeFn)(engine + MERGE_RVA);
    alternate_record = (AlternateRecordFn)(engine + ALTERNATE_RECORD_RVA);
    for (int i = 0; i < HELPERS; i++) {
        HANDLE thread = CreateThread(NULL, 0, helper, NULL, 0, NULL);
        if (!thread) return "CreateThread failed";
        CloseHandle(thread);
    }
    return api->hook(engine + WALK_RVA, walk_prologue, sizeof walk_prologue, (const void *)walk_hook, (void **)&original_walk);
}
