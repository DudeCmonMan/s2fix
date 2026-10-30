#pragma once
#include <windows.h>
#include <immintrin.h>

// The battle triangulation's two brute-force triangle searches, split across helper threads with the answers the
// engine's sequential loops give: the lowest passing triangle, or one answer per triangle in the engine's order.
#define SPLIT_HELPERS 7
#define SPLIT_LANES 3
#define SPLIT_MAX_CHUNKS 64
#define SPLIT_MIN_CHUNK 128
#define SPLIT_TRIANGLE_BYTES 56

typedef unsigned char (*PointTest)(const void *triangle, const float *point);
typedef unsigned char (*CrossingTest)(const void *triangle, const float *a, const float *b);

enum { SPLIT_POINT_SEARCH, SPLIT_CROSSING_TESTS };

typedef struct {
    int kind;
    const BYTE *triangles;
    LONG count, chunk_size, chunk_count;
    PointTest point_test;
    CrossingTest crossing_test;
    const float *point, *a, *b;
    BYTE *answers;
    unsigned int csr;
    unsigned short x87;
} SplitJob;

typedef struct {
    SplitJob job;
    volatile LONG64 claim, done[SPLIT_MAX_CHUNKS];
    volatile LONG best;
} SplitLane;

static SplitLane split_lanes[SPLIT_LANES];
// Helpers spin while a triangulation runs in any lane, its searches coming back to back; else they sleep.
static volatile LONG split_active, split_posted;

static void split_lower_best(SplitLane *lane, LONG index) {
    for (LONG best = lane->best; index < best; best = lane->best)
        if (InterlockedCompareExchange(&lane->best, index, best) == best) return;
}

static void split_run_chunk(SplitLane *lane, LONG chunk, LONG64 generation) {
    const SplitJob *job = &lane->job;
    LONG first = chunk * job->chunk_size, end = first + job->chunk_size < job->count ? first + job->chunk_size : job->count;
    if (job->kind == SPLIT_POINT_SEARCH) {
        // Only the lowest hit counts: a chunk stops once a lower triangle has passed.
        for (LONG i = first; i < end && i < lane->best; i++)
            if (job->point_test(job->triangles + (size_t)i * SPLIT_TRIANGLE_BYTES, job->point)) {
                split_lower_best(lane, i);
                break;
            }
    } else {
        for (LONG i = first; i < end; i++) job->answers[i] = job->crossing_test(job->triangles + (size_t)i * SPLIT_TRIANGLE_BYTES, job->a, job->b) != 0;
    }
    InterlockedExchange64(&lane->done[chunk], generation);
}

// Runs chunks of the lane's current job until none is left unclaimed; a helper takes on the job's control words first.
static BOOL split_claim_chunks(SplitLane *lane, BOOL helper) {
    BOOL ran = FALSE;
    for (;;) {
        LONG64 claim = lane->claim;
        LONG chunk = (LONG)(claim & 0xffffffff);
        if (chunk >= lane->job.chunk_count) return ran;
        if (InterlockedCompareExchange64(&lane->claim, claim + 1, claim) != claim) continue;
        if (helper) {
            unsigned short x87 = lane->job.x87;
            _mm_setcsr(lane->job.csr);
            __asm__ volatile("fldcw %0" : : "m"(x87));
        }
        split_run_chunk(lane, chunk, claim >> 32);
        ran = TRUE;
    }
}

static DWORD WINAPI split_helper(LPVOID first_lane) {
    for (;;) {
        LONG posted = split_posted;
        BOOL ran = FALSE;
        for (int k = 0; k < SPLIT_LANES; k++) ran |= split_claim_chunks(&split_lanes[((int)(INT_PTR)first_lane + k) % SPLIT_LANES], TRUE);
        if (ran) continue;
        if (split_active)
            _mm_pause();
        else
            WaitOnAddress(&split_posted, &posted, sizeof posted, INFINITE);
    }
    return 0;
}

static void split_run(int lane_index, SplitJob job) {
    SplitLane *lane = &split_lanes[lane_index];
    job.chunk_size = (job.count + SPLIT_MAX_CHUNKS - 1) / SPLIT_MAX_CHUNKS;
    if (job.chunk_size < SPLIT_MIN_CHUNK) job.chunk_size = SPLIT_MIN_CHUNK;
    job.chunk_count = (job.count + job.chunk_size - 1) / job.chunk_size;
    __asm__ volatile("fnstcw %0" : "=m"(job.x87));
    job.csr = _mm_getcsr();
    LONG64 generation = (lane->claim >> 32) + 1;
    // A helper that read the claim before this must fail to take it once the job changes.
    InterlockedExchange64(&lane->claim, generation << 32 | 0x7fffffff);
    lane->job = job;
    lane->best = job.count;
    generation++;
    InterlockedExchange64(&lane->claim, generation << 32);
    InterlockedIncrement(&split_posted);
    WakeByAddressAll((void *)&split_posted);
    split_claim_chunks(lane, FALSE);
    for (LONG chunk = 0; chunk < job.chunk_count; chunk++)
        while (lane->done[chunk] != generation) _mm_pause();
}

// The index of the first triangle (in array order) the test passes, or count when none does.
static LONG split_point_search(int lane, PointTest test, const BYTE *triangles, LONG count, const float *point) {
    split_run(lane, (SplitJob){.kind = SPLIT_POINT_SEARCH, .triangles = triangles, .count = count, .point_test = test, .point = point});
    return split_lanes[lane].best;
}

static void split_crossing_tests(int lane, CrossingTest test, const BYTE *triangles, LONG count, const float *a, const float *b, BYTE *answers) {
    split_run(lane, (SplitJob){.kind = SPLIT_CROSSING_TESTS, .triangles = triangles, .count = count, .crossing_test = test, .a = a, .b = b, .answers = answers});
}

// Around a triangulation, from each lane: the helpers spin for the next search instead of going back to sleep.
static void split_begin(void) {
    InterlockedIncrement(&split_active);
    WakeByAddressAll((void *)&split_posted);
}

static void split_end(void) {
    InterlockedDecrement(&split_active);
}

static BOOL split_start_helpers(void) {
    for (int i = 0; i < SPLIT_HELPERS; i++) {
        HANDLE thread = CreateThread(NULL, 0, split_helper, (LPVOID)(INT_PTR)(i % SPLIT_LANES), 0, NULL);
        if (!thread) return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}
