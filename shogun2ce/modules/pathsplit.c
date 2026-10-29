#include "../s2module.h"
#include "../pathsplit.h"
#include "../checks/pathsplit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// The original game's pathsplit on the Complete Edition. The battle pathfinder triangulates the map in one call
// (0x18198a2d0) and, when an edge cannot be inserted, reorders the edges and runs it again, up to three times
// (0x181980a80). Two changes, each leaving the triangulation the same:
// - The searches. Inserting a point scans the 56-byte triangles from the first for the one holding it, with the test
//   inline (0x181987480); inserting an edge calls the crossing test (0x18199b320) for every triangle (0x1819a6330). The
//   tests read the triangles and the points and write nothing, so the searches run on helper threads (pathsplit.h):
//   the point loop's entry jumps to ours, which finds the triangle with an exact copy of the inline test and re-enters
//   the engine's loop body there, so the engine's own test passes on it and every register is as the engine leaves it;
//   the crossing call calls ours, which at the first triangle of a pass works out every triangle's answer, and the
//   loop's step (0x1819a6469) asks ours for the next crossing triangle instead of walking every one.
// - The retries. A retry's input is the previous attempt's edges reordered in place by the engine from the edges
//   alone, so attempts 2 and 3 start alongside attempt 1 on two threads, each running its own copy of the triangulator
//   and the edge insert, whose globals (the hit list, its one-time set-up guard, the failure flag, the failing edge)
//   point at the copy's own. When the engine asks for attempt 2 or 3, its input is compared with the prediction byte for
//   byte: if equal, the copy's result is handed back, else the attempt runs here.
// x64 code reaches globals and calls by 32-bit offsets, so the thunks and the copies sit within 2 GB above the engine;
// each copied offset is re-aimed after checking it against the engine (the tables come from the disassembly), and the
// copies' unwind entries are registered with the engine's unwind data, so exceptions and stack walks pass through them.
// The three attempts search in lanes: lane 0 is the engine's code on main, lanes 1 and 2 the copies, whose test calls
// and steps go to their lane's entries; the point loop is shared engine code, so ours finds the lane by thread.
// [pathsplit] verify=1 also runs each attempt the engine's own way, whose output the game then uses, and compares.
#define TRIANGULATE_RVA 0x198a2d0
#define TRIANGULATE_SIZE 0x634
#define EDGE_INSERT_RVA 0x19a62a0
#define EDGE_INSERT_SIZE 0x92b
#define POINT_ENTRY_RVA 0x1987480
#define POINT_BODY_RVA 0x19874a0
#define POINT_NONE_RVA 0x198754c
#define CROSSING_CALL_OFFSET 0x99
#define EDGE_STEP_OFFSET 0x1c9
#define EDGE_LOOP_OFFSET 0x90
#define CROSSES_RVA 0x199b320
#define FREE_RVA 0x19fe10
#define FAILED_FLAG_RVA 0x338fda5
#define FAILED_EDGE_RVA 0x338fda8
#define MIN_SPLIT 1024
// The engine's retries leave the first four edges (the map's border) in place and use this generator for attempt 3.
#define FIRST_REORDERED 4
#define SHUFFLE_SEED 0x61266u
#define NEAR_BYTES 0x10000
#define CODE_BYTES 0x5000
#define DATA_OFFSET 0x8000
#define MAX_UNWIND_ENTRIES 64

typedef int *(*TriangulateFn)(int index_count, const int *edges, const float *points, uint32_t *out);
typedef uint64_t (*EdgeInsertFn)(BYTE *triangles, const float *a, const float *b);
typedef void (*FreeFn)(void *block);

// A 32-bit offset in a copied function: where it is, where its instruction ends (what it is relative to), its target.
typedef struct {
    uint16_t field, end;
    uint32_t target_rva;
} CopySite;

// Generated from the disassembly (every rel32 branch leaving the function and every RIP-relative operand); checked
// against the engine at install.
static const CopySite edge_insert_sites[] = {
    {0x21, 0x25, 0x3471bd0},  {0x39, 0x3d, 0x344ff00},  {0x5d, 0x61, 0x344fef4},  {0x9a, 0x9e, 0x199b320},  {0xa8, 0xac, 0x344fef0},
    {0xb3, 0xb7, 0x344fef8},  {0xc9, 0xcd, 0x2e3fd98},  {0xd2, 0xda, 0x344fef0},  {0xdb, 0xdf, 0x19fe50},   {0xe1, 0xe5, 0x344fef4},
    {0xe8, 0xec, 0x344fef8},  {0xf5, 0xf9, 0x344fef8},  {0xfd, 0x101, 0x344fef0}, {0x106, 0x10a, 0x2e3fd98}, {0x111, 0x115, 0x19fe50},
    {0x117, 0x11b, 0x344fef4}, {0x128, 0x12c, 0x344fef8}, {0x144, 0x148, 0x344fef4}, {0x14b, 0x14f, 0x344fef8}, {0x162, 0x166, 0x19fe10},
    {0x168, 0x16c, 0x344fef4}, {0x170, 0x174, 0x344fef4}, {0x1e5, 0x1e9, 0x344fef8}, {0x268, 0x26c, 0x344fef4}, {0x26f, 0x273, 0x344fef8},
    {0x277, 0x27b, 0x344fef4}, {0x315, 0x319, 0x344fef8}, {0x388, 0x38c, 0x198a910}, {0x39b, 0x39f, 0x19ad3c0}, {0x3a9, 0x3ad, 0x199b320},
    {0x3ba, 0x3be, 0x199b320}, {0x4b3, 0x4b7, 0x344fef4}, {0x4ba, 0x4be, 0x344fef8}, {0x4d1, 0x4d5, 0x344fef4}, {0x4d8, 0x4dc, 0x344fef8},
    {0x4e7, 0x4eb, 0x344fef4}, {0x4f4, 0x4f8, 0x344fef8}, {0x504, 0x508, 0x344fef4}, {0x56d, 0x571, 0x198a910}, {0x581, 0x585, 0x19ad3c0},
    {0x58f, 0x593, 0x199b320}, {0x5a5, 0x5a9, 0x199b320}, {0x69c, 0x6a0, 0x344fef4}, {0x6a3, 0x6a7, 0x344fef8}, {0x6b3, 0x6b7, 0x344fef4},
    {0x6bc, 0x6c0, 0x344fef8}, {0x6c4, 0x6c8, 0x344fef4}, {0x6d4, 0x6d8, 0x344fef4}, {0x6db, 0x6df, 0x344fef8}, {0x6f2, 0x6f6, 0x344fef8},
    {0x6f8, 0x6fc, 0x344fef4}, {0x7c8, 0x7cc, 0x344fef4}, {0x7cf, 0x7d3, 0x344fef8}, {0x7d7, 0x7db, 0x344fef4}, {0x816, 0x81a, 0x199d0b0},
    {0x86d, 0x871, 0x198a910}, {0x87c, 0x880, 0x19ad3c0}, {0x88d, 0x891, 0x199d0b0}, {0x89d, 0x8a1, 0x338fda8}, {0x8aa, 0x8ae, 0x338fdb0},
    {0x8b0, 0x8b5, 0x338fda5}, {0x8c7, 0x8cf, 0x344fef4}, {0x8df, 0x8e7, 0x344fef4}, {0x8f8, 0x8fc, 0x344ff00}, {0x8fd, 0x901, 0x1d7b958},
    {0x903, 0x908, 0x344ff00}, {0x911, 0x915, 0x21474e0}, {0x916, 0x91a, 0x1d7b898}, {0x91d, 0x921, 0x344ff00}, {0x922, 0x926, 0x1d7b8ec}};
static const CopySite triangulate_sites[] = {
    {0x19f, 0x1a3, 0x218039c}, {0x22b, 0x22f, 0x2e3fd98}, {0x238, 0x23c, 0x19fe50},  {0x260, 0x264, 0x4ddd20},  {0x2a6, 0x2aa, 0x19a4800},
    {0x2fb, 0x2ff, 0x19a4800}, {0x350, 0x354, 0x1987430}, {0x365, 0x369, 0x1987430}, {0x375, 0x379, 0x19a62a0}, {0x3fd, 0x401, 0x19aba60},
    {0x40d, 0x411, 0x19ad3c0}, {0x46a, 0x46e, 0x19aba60}, {0x47a, 0x47e, 0x19ad3c0}, {0x4d7, 0x4db, 0x19aba60}, {0x4e7, 0x4eb, 0x19ad3c0},
    {0x507, 0x50b, 0x2e3fd98}, {0x513, 0x517, 0x19fe50},  {0x614, 0x618, 0x19fe10},  {0x621, 0x625, 0x19fe10}};

typedef struct {
    LONG64 point_searches, point_triangles, edge_inserts, crossing_passes;
    LONGLONG point_ticks, edge_ticks, crossing_ticks, total_ticks;
} Stats;

typedef struct {
    int *indices;
    uint32_t index_count;
    BYTE failed;
    BYTE failed_edge[16];
    Stats stats;
} Result;

// An attempt's searches: its edge insert, the thread running it, and the triangle array and answers of its pass.
typedef struct {
    EdgeInsertFn insert;
    DWORD thread;
    BOOL splitting;
    BYTE *edge_triangles, *answers;
    const BYTE *pass_data;
    const float *pass_a, *pass_b;
    LONG pass_count, answers_capacity;
    Stats stats;
} Lane;

// A copy's globals: the hit list (capacity, size, data, and its one-time set-up guard, set so the set-up never runs),
// the "no edge failed" flag and the failing edge.
typedef struct {
    DWORD capacity, size;
    void *data;
    LONG guard;
    BYTE ok;
    uint64_t failed_edge[2];
} CopyState;

typedef struct {
    TriangulateFn triangulate;
    CopyState *state;
    int *edges, index_count, *result;
    uint32_t out[16];
    const float *points;
    unsigned int csr;
    HANDLE thread;
    BOOL served;
} Ahead;

// 0 runs the searches on main only and every attempt here, the engine's way; exported, and a switch.
__declspec(dllexport) volatile LONG split_enabled = 1;

static const BYTE point_entry_original[32] = {0x48, 0x3b, 0xda, 0x0f, 0x84, 0xc3, 0x00, 0x00, 0x00, 0xf3, 0x45, 0x0f, 0x10, 0x4d, 0x04, 0xf3,
                                              0x45, 0x0f, 0x10, 0x55, 0x00, 0x66, 0x66, 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
static const BYTE crossing_call_original[5] = {0xe8, 0xe2, 0x4f, 0xff, 0xff};
static const BYTE edge_step_original[25] = {0x41, 0x8b, 0x45, 0x04, 0x48, 0x83, 0xc6, 0x38, 0x48, 0x6b, 0xc8, 0x38, 0x49,
                                            0x03, 0x4d, 0x08, 0x48, 0x3b, 0xf1, 0x0f, 0x85, 0xae, 0xfe, 0xff, 0xff};
static const BYTE edge_insert_prologue[] = {0x40, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24, 0xc1, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00};
static const BYTE triangulate_prologue[] = {0x48, 0x8b, 0xc4, 0x4c, 0x89, 0x48, 0x20, 0x55, 0x56, 0x41, 0x54, 0x48, 0x8d, 0x68, 0x88};
static BYTE point_entry_replacement[32], crossing_call_replacement[5], edge_step_replacement[25];

static const S2Api *api;
static TriangulateFn original_triangulate;
static CrossingTest crosses;
static FreeFn engine_free;
static BOOL verify;
static BYTE *near_memory;
static Lane lanes[SPLIT_LANES];
static Ahead ahead[SPLIT_LANES - 1];
static RUNTIME_FUNCTION unwind_entries[MAX_UNWIND_ENTRIES];
static LONG triangulations;
static LONGLONG triangulation_started;
static double qpc_per_ms;

static LONGLONG now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double ms(LONGLONG ticks) {
    return ticks / qpc_per_ms;
}

// The engine's inline point test (0x1819874a0 to 0x18198753d), operation for operation: the triangle's three corners
// are pointers to (x, y); each side's test is a compare of two products, failing when less or unordered.
static unsigned char point_test(const void *triangle, const float *point) {
    const float *a = ((const float *const *)triangle)[0], *b = ((const float *const *)triangle)[1], *c = ((const float *const *)triangle)[2];
    float px = point[0], py = point[1];
    if (!((b[1] - a[1]) * (a[0] - px) >= (b[0] - a[0]) * (a[1] - py))) return 0;
    if (!((c[1] - b[1]) * (b[0] - px) >= (c[0] - b[0]) * (b[1] - py))) return 0;
    return (a[1] - c[1]) * (c[0] - px) >= (a[0] - c[0]) * (c[1] - py);
}

// Called from the point loop's entry through our stub; returns the first triangle whose test passes (end if none).
static BYTE *point_search(BYTE *begin, BYTE *end, const float *point) {
    LONG count = (LONG)((end - begin) / SPLIT_TRIANGLE_BYTES), found = count;
    DWORD thread = GetCurrentThreadId();
    int k = 0;
    while (k < SPLIT_LANES && lanes[k].thread != thread) k++;
    LONGLONG started = now();
    if (k < SPLIT_LANES && lanes[k].splitting && count >= MIN_SPLIT) {
        found = split_point_search(k, point_test, begin, count, point);
    } else {
        for (LONG i = 0; i < count; i++)
            if (point_test(begin + (size_t)i * SPLIT_TRIANGLE_BYTES, point)) {
                found = i;
                break;
            }
    }
    if (k < SPLIT_LANES) {
        Stats *stats = &lanes[k].stats;
        stats->point_searches++, stats->point_triangles += count, stats->point_ticks += now() - started;
    }
    return begin + (size_t)found * SPLIT_TRIANGLE_BYTES;
}

static BOOL answers_fit(Lane *lane, LONG count) {
    if (count <= lane->answers_capacity) return TRUE;
    BYTE *grown = realloc(lane->answers, (size_t)count * 2);
    if (!grown) return FALSE;
    lane->answers = grown, lane->answers_capacity = count * 2;
    return TRUE;
}

// Called from an edge search for the triangles it visits. A pass starts at the first triangle; its answers are worked
// out then for the whole array, which the search does not change.
static unsigned char crossing_test(Lane *lane, const BYTE *triangle, const float *a, const float *b) {
    if (lane->splitting && lane->edge_triangles) {
        const BYTE *data = *(BYTE **)(lane->edge_triangles + 8);
        LONG count = *(LONG *)(lane->edge_triangles + 4);
        if (triangle == data) {
            lane->pass_count = 0;
            if (count >= MIN_SPLIT && answers_fit(lane, count)) {
                LONGLONG started = now();
                split_crossing_tests((int)(lane - lanes), crosses, data, count, a, b, lane->answers);
                lane->pass_data = data, lane->pass_count = count, lane->pass_a = a, lane->pass_b = b;
                lane->stats.crossing_passes++, lane->stats.crossing_ticks += now() - started;
            }
        }
        LONG index = (LONG)((triangle - lane->pass_data) / SPLIT_TRIANGLE_BYTES);
        if (lane->pass_count && triangle >= lane->pass_data && index < lane->pass_count && a == lane->pass_a && b == lane->pass_b)
            return lane->answers[index];
    }
    return crosses(triangle, a, b);
}

// Called from an edge search's step: with a pass's answers, the next crossing triangle (the engine's loop does nothing
// for the others); otherwise the next triangle, as the engine steps; NULL at the end.
static BYTE *next_triangle(Lane *lane, BYTE *current) {
    if (lane->pass_count && current >= lane->pass_data) {
        LONG index = (LONG)((current - lane->pass_data) / SPLIT_TRIANGLE_BYTES) + 1;
        const BYTE *hit = index < lane->pass_count ? memchr(lane->answers + index, 1, (size_t)(lane->pass_count - index)) : NULL;
        return hit ? (BYTE *)lane->pass_data + (size_t)(hit - lane->answers) * SPLIT_TRIANGLE_BYTES : NULL;
    }
    BYTE *data = *(BYTE **)(lane->edge_triangles + 8);
    current += SPLIT_TRIANGLE_BYTES;
    return current != data + (size_t)*(DWORD *)(lane->edge_triangles + 4) * SPLIT_TRIANGLE_BYTES ? current : NULL;
}

static uint64_t edge_insert(Lane *lane, BYTE *triangles, const float *a, const float *b) {
    LONGLONG started = now();
    lane->edge_triangles = triangles, lane->pass_count = 0;
    uint64_t result = lane->insert(triangles, a, b);
    lane->edge_triangles = NULL, lane->pass_count = 0;
    lane->stats.edge_inserts++, lane->stats.edge_ticks += now() - started;
    return result;
}

// Each lane's entries: lane 0's from the engine's edge insert (hooked and patched), the others' from their copies.
#define LANE_ENTRIES(k)                                                                                                                              \
    static unsigned char crossing_test_##k(const BYTE *triangle, const float *a, const float *b) { return crossing_test(&lanes[k], triangle, a, b); } \
    static BYTE *next_triangle_##k(BYTE *current) { return next_triangle(&lanes[k], current); }                                                     \
    static uint64_t edge_insert_##k(BYTE *triangles, const float *a, const float *b) { return edge_insert(&lanes[k], triangles, a, b); }
LANE_ENTRIES(0)
LANE_ENTRIES(1)
LANE_ENTRIES(2)
static const void *const lane_crossing_tests[SPLIT_LANES] = {(const void *)crossing_test_0, (const void *)crossing_test_1, (const void *)crossing_test_2};
static const void *const lane_next_triangles[SPLIT_LANES] = {(const void *)next_triangle_0, (const void *)next_triangle_1, (const void *)next_triangle_2};
static const void *const lane_edge_inserts[SPLIT_LANES] = {(const void *)edge_insert_0, (const void *)edge_insert_1, (const void *)edge_insert_2};
// The lanes' thunks in the memory near the engine (rel32 calls reach them).
static BYTE *crossing_thunks[SPLIT_LANES], *step_thunks[SPLIT_LANES], *edge_entry_thunks[SPLIT_LANES], *point_stub;

static const char *patch_searches(BOOL ours) {
    static const struct {
        DWORD rva;
        const BYTE *original, *replacement;
        size_t size;
    } patches[] = {{POINT_ENTRY_RVA, point_entry_original, point_entry_replacement, sizeof point_entry_original},
                   {EDGE_INSERT_RVA + CROSSING_CALL_OFFSET, crossing_call_original, crossing_call_replacement, sizeof crossing_call_original},
                   {EDGE_INSERT_RVA + EDGE_STEP_OFFSET, edge_step_original, edge_step_replacement, sizeof edge_step_original}};
    for (size_t i = 0; i < sizeof patches / sizeof *patches; i++) {
        const char *error = api->patch(api->engine + patches[i].rva, ours ? patches[i].original : patches[i].replacement,
                                       ours ? patches[i].replacement : patches[i].original, patches[i].size);
        if (error) return error;
    }
    return NULL;
}

static Result run(int index_count, const int *edges, const float *points, uint32_t *out, BOOL split) {
    BYTE *failed_flag = api->engine + FAILED_FLAG_RVA;
    Lane *lane = &lanes[0];
    Result result = {0};
    memset(&lane->stats, 0, sizeof lane->stats);
    lane->splitting = split;
    lane->thread = GetCurrentThreadId();
    *failed_flag = 1;
    if (split) split_begin();
    LONGLONG started = now();
    result.indices = original_triangulate(index_count, edges, points, out);
    lane->stats.total_ticks = now() - started;
    if (split) split_end();
    lane->splitting = FALSE;
    lane->thread = 0;
    result.index_count = out[0];
    result.failed = !*failed_flag;
    memcpy(result.failed_edge, api->engine + FAILED_EDGE_RVA, sizeof result.failed_edge);
    result.stats = lane->stats;
    return result;
}

static void describe(char *text, size_t size, const char *label, const Stats *s) {
    double total = ms(s->total_ticks), searches = ms(s->point_ticks), inserts = ms(s->edge_ticks);
    int used = snprintf(text, size, "%s %.0f ms (point searches %lld over %.0f triangles each, %.0f ms; edge inserts %lld, %.0f ms", label, total,
                        s->point_searches, s->point_searches ? (double)s->point_triangles / s->point_searches : 0.0, searches, s->edge_inserts, inserts);
    if (s->crossing_passes && used > 0 && (size_t)used < size)
        used += snprintf(text + used, size - used, ", of which crossing tests split in %lld passes %.0f ms", s->crossing_passes, ms(s->crossing_ticks));
    if (used > 0 && (size_t)used < size) snprintf(text + used, size - used, "; the rest %.0f ms)", total - searches - inserts);
}

static BOOL same(const Result *a, const Result *b) {
    return a->index_count == b->index_count && a->failed == b->failed && (!a->failed || !memcmp(a->failed_edge, b->failed_edge, sizeof a->failed_edge)) &&
           (!a->index_count || (a->indices && b->indices && !memcmp(a->indices, b->indices, (size_t)a->index_count * sizeof *a->indices)));
}

static DWORD WINAPI run_ahead(LPVOID context) {
    Ahead *attempt = context;
    Lane *lane = &lanes[attempt - ahead + 1];
    _mm_setcsr(attempt->csr);
    memset(&lane->stats, 0, sizeof lane->stats);
    lane->splitting = TRUE;
    lane->thread = GetCurrentThreadId();
    attempt->state->ok = 1;
    memset(attempt->out, 0, sizeof attempt->out);
    split_begin();
    LONGLONG started = now();
    attempt->result = attempt->triangulate(attempt->index_count, attempt->edges, attempt->points, attempt->out);
    lane->stats.total_ticks = now() - started;
    split_end();
    lane->splitting = FALSE;
    lane->thread = 0;
    return 0;
}

static LONGLONG wait_ahead(void) {
    LONGLONG started = now();
    for (int k = 0; k < SPLIT_LANES - 1; k++)
        if (ahead[k].thread) WaitForSingleObject(ahead[k].thread, INFINITE);
    return now() - started;
}

// Waits for attempts started for an earlier triangulation and frees what the engine did not ask for.
static void drop_ahead(void) {
    wait_ahead();
    for (int k = 0; k < SPLIT_LANES - 1; k++) {
        Ahead *attempt = &ahead[k];
        if (!attempt->thread) continue;
        CloseHandle(attempt->thread);
        if (!attempt->served && attempt->result) engine_free(attempt->result);
        attempt->thread = NULL, attempt->result = NULL;
    }
    free(ahead[0].edges);
    ahead[0].edges = ahead[1].edges = NULL;
}

static void swap_edges(int *edges, DWORD i, DWORD j) {
    int first = edges[2 * i], second = edges[2 * i + 1];
    edges[2 * i] = edges[2 * j], edges[2 * i + 1] = edges[2 * j + 1];
    edges[2 * j] = first, edges[2 * j + 1] = second;
}

// The engine's reorderings before attempts 2 and 3 (in 0x181980a80), in place, edge by edge.
static void start_ahead(int index_count, const int *edges, const float *points) {
    DWORD count = (DWORD)index_count / 2;
    int *second = malloc((size_t)index_count * 2 * sizeof *second);
    if (!second) return;
    int *third = second + index_count;
    memcpy(second, edges, (size_t)index_count * sizeof *second);
    for (DWORD i = FIRST_REORDERED; i < count; i++) swap_edges(second, i, count - 1 - i);
    memcpy(third, second, (size_t)index_count * sizeof *third);
    DWORD x = SHUFFLE_SEED;
    for (DWORD i = FIRST_REORDERED; i < count; i++) {
        x = x * 0x343fd + 0x269ec3;
        DWORD j = (x >> 16) % count;
        swap_edges(third, i, j < FIRST_REORDERED ? FIRST_REORDERED : j);
    }
    for (int k = 0; k < SPLIT_LANES - 1; k++) {
        Ahead *attempt = &ahead[k];
        attempt->edges = k ? third : second, attempt->index_count = index_count, attempt->points = points;
        attempt->csr = _mm_getcsr(), attempt->served = FALSE, attempt->result = NULL;
        attempt->thread = CreateThread(NULL, 0, run_ahead, attempt, 0, NULL);
    }
}

static Ahead *predicted(int index_count, const int *edges, const float *points) {
    for (int k = 0; k < SPLIT_LANES - 1; k++) {
        Ahead *attempt = &ahead[k];
        if (attempt->thread && !attempt->served && attempt->index_count == index_count && attempt->points == points &&
            !memcmp(attempt->edges, edges, (size_t)index_count * sizeof *edges))
            return attempt;
    }
    return NULL;
}

// Hands the engine an attempt run alongside, as if it had run here: the index count, the index list (the engine now
// owns it), and the failure flag and failing edge in the engine's globals (left as they were when the attempt worked).
static int *serve_ahead(Ahead *attempt, uint32_t *out, LONGLONG *waited) {
    LONGLONG started = now();
    WaitForSingleObject(attempt->thread, INFINITE);
    *waited = now() - started;
    attempt->served = TRUE;
    out[0] = attempt->out[0];
    if (!attempt->state->ok) {
        *(api->engine + FAILED_FLAG_RVA) = 0;
        memcpy(api->engine + FAILED_EDGE_RVA, attempt->state->failed_edge, sizeof attempt->state->failed_edge);
    }
    return attempt->result;
}

static int *verify_attempt_one(int number, int index_count, const int *edges, const float *points, uint32_t *out) {
    char split_text[320], single_text[320];
    // Our runs read copies, so whatever they did could not reach the engine's own run, which the game then uses.
    int highest = 0;
    for (int i = 0; i < index_count; i++)
        if (edges[i] > highest) highest = edges[i];
    int *edges_copy = malloc((size_t)index_count * sizeof *edges_copy);
    float *points_copy = malloc((size_t)(highest + 1) * 2 * sizeof *points_copy);
    if (!edges_copy || !points_copy) {
        free(edges_copy), free(points_copy);
        wait_ahead();
        return original_triangulate(index_count, edges, points, out);
    }
    memcpy(edges_copy, edges, (size_t)index_count * sizeof *edges_copy);
    memcpy(points_copy, points, (size_t)(highest + 1) * 2 * sizeof *points_copy);
    uint32_t single_out[16] = {0}, split_out[16] = {0};
    // Split while the copies run, as without verify; the copies run the shared point loop, so they must be done before
    // it is switched back to the engine's.
    Result split = run(index_count, edges_copy, points_copy, split_out, TRUE);
    LONGLONG waited = wait_ahead(), all_done = now() - triangulation_started;
    Result single = run(index_count, edges_copy, points_copy, single_out, FALSE);
    const char *error = patch_searches(FALSE);
    Result engine = run(index_count, edges, points, out, FALSE);
    if (!error) error = patch_searches(TRUE);
    describe(single_text, sizeof single_text, "ours on main", &single.stats);
    describe(split_text, sizeof split_text, "ours split", &split.stats);
    api->log_line("pathsplit: verify, triangulation %d, attempt 1: %d edges, %u triangles%s; the engine's loops %.0f ms; %s; %s; on main %s, split %s; "
                  "attempts 2 and 3 alongside done %.0f ms after attempt 1 began (main waited %.0f ms for them after its split run)%s%s",
                  number, index_count / 2, engine.index_count / 3, engine.failed ? " (failed, the engine retries)" : "", ms(engine.stats.total_ticks),
                  single_text, split_text, same(&single, &engine) ? "identical" : "DIFFERENT", same(&split, &engine) ? "identical" : "DIFFERENT",
                  ms(all_done), ms(waited), error ? "; the searches could not be switched back: " : "", error ? error : "");
    if (single.indices) engine_free(single.indices);
    if (split.indices) engine_free(split.indices);
    free(edges_copy), free(points_copy);
    return engine.indices;
}

static int *triangulate_hook(int index_count, const int *edges, const float *points, uint32_t *out) {
    if (!*api->fix_enabled) return original_triangulate(index_count, edges, points, out);
    Ahead *attempt = split_enabled ? predicted(index_count, edges, points) : NULL;
    if (attempt) {
        int number = (int)(attempt - ahead) + 2;
        char text[320];
        LONGLONG waited;
        int *indices = serve_ahead(attempt, out, &waited);
        BOOL failed = !attempt->state->ok;
        describe(text, sizeof text, "split on its thread", &lanes[number - 1].stats);
        if (!verify) {
            api->log_line("pathsplit: triangulation %ld, attempt %d: run alongside (input as predicted), %u triangles%s, %s; main waited %.0f ms, "
                          "%.0f ms since attempt 1 began",
                          triangulations, number, out[0] / 3, failed ? " (failed)" : "", text, ms(waited), ms(now() - triangulation_started));
            return indices;
        }
        Result served = {indices, out[0], (BYTE)failed, {0}, {0}};
        memcpy(served.failed_edge, attempt->state->failed_edge, sizeof served.failed_edge);
        const char *error = patch_searches(FALSE);
        Result engine = run(index_count, edges, points, out, FALSE);
        if (!error) error = patch_searches(TRUE);
        api->log_line("pathsplit: verify, triangulation %ld, attempt %d: the engine's loops %.0f ms; run alongside (input as predicted), %s; %s%s%s",
                      triangulations, number, ms(engine.stats.total_ticks), text, same(&served, &engine) ? "identical" : "DIFFERENT",
                      error ? "; the searches could not be switched back: " : "", error ? error : "");
        if (indices) engine_free(indices);
        return engine.indices;
    }
    // A retry of the same triangulation the prediction did not match (never seen): it runs here, the engine's way.
    if (split_enabled && ahead[0].thread && !(ahead[0].served && ahead[1].served) && ahead[0].index_count == index_count && ahead[0].points == points) {
        Result result = run(index_count, edges, points, out, TRUE);
        api->log_line("pathsplit: triangulation %ld: a retry's input differs from the prediction; it ran here, %.0f ms", triangulations,
                      ms(result.stats.total_ticks));
        return result.indices;
    }
    int number = (int)++triangulations;
    drop_ahead();
    triangulation_started = now();
    if (split_enabled) start_ahead(index_count, edges, points);
    if (verify) return verify_attempt_one(number, index_count, edges, points, out);
    char text[320];
    Result result = run(index_count, edges, points, out, split_enabled);
    describe(text, sizeof text, split_enabled ? "split" : "on main", &result.stats);
    api->log_line("pathsplit: triangulation %d: %d edges, %u triangles%s; %s", number, index_count / 2, result.index_count / 3,
                  result.failed ? " (failed, the engine retries)" : "", text);
    return result.indices;
}

// s2prof.ini sits next to the game, two folders up from fixes\pathsplit.dll.
static BOOL verify_in_ini(void) {
    wchar_t path[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)verify_in_ini, &self);
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return FALSE;
    for (int up = 0; up < 2; up++) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) *slash = 0;
    }
    wcscat(path, L"\\s2prof.ini");
    return GetPrivateProfileIntW(L"pathsplit", L"verify", 0, path) != 0;
}

static BYTE *put(BYTE *at, const void *bytes, size_t size) {
    memcpy(at, bytes, size);
    return at + size;
}

static BYTE *put_jump(BYTE *at, const void *target) {
    static const BYTE jump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    at = put(at, jump, sizeof jump);
    return put(at, &target, sizeof target);
}

// mov rax, target; call rax
static BYTE *put_call(BYTE *at, const void *target) {
    static const BYTE mov_rax[] = {0x48, 0xb8};
    static const BYTE call_rax[] = {0xff, 0xd0};
    at = put(at, mov_rax, sizeof mov_rax);
    at = put(at, &target, sizeof target);
    return put(at, call_rax, sizeof call_rax);
}

// A 5- or 6-byte rel32 branch at `at` in `code`, whose bytes will run at `site`.
static BOOL put_rel32(BYTE *code, size_t at, size_t length, BYTE *site, const void *target) {
    int64_t rel = (const BYTE *)target - (site + at + length);
    if (rel != (int32_t)rel) return FALSE;
    int32_t rel32 = (int32_t)rel;
    memcpy(code + at + length - 4, &rel32, 4);
    return TRUE;
}

// The point loop's stub, jumped to from its entry with rbx the first triangle, rdx the end and r13 the point: rbx =
// point_search(rbx, rdx, r13); none found continues at the engine's "none" code, else the engine's loop body runs its
// own test on that triangle, after the two loads the entry made (xmm9, xmm10). r8-r11 are live in the engine's code.
static BYTE *make_point_stub(BYTE *at) {
    static const BYTE save[] = {0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0xd9, 0x4d, 0x89, 0xe8};
    static const BYTE restore[] = {0x48, 0x83, 0xc4, 0x28, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x48, 0x39, 0xd0};
    static const BYTE take_found[] = {0x48, 0x89, 0xc3};
    at = put(at, save, sizeof save);
    at = put_call(at, (const void *)point_search);
    at = put(at, restore, sizeof restore);
    BYTE *skip = at;
    at += 2;
    at = put(at, take_found, sizeof take_found);
    at = put(at, point_entry_original + 9, 12);
    at = put_jump(at, api->engine + POINT_BODY_RVA);
    skip[0] = 0x74, skip[1] = (BYTE)(at - (skip + 2));
    return put_jump(at, api->engine + POINT_NONE_RVA);
}

// A lane's step, called from the edge search's step with rsi the triangle just visited: rsi = next or, at the end, the
// engine's own exit state (eax the count, rcx and rsi the end) with ZF set; r8 holds the engine's crossing count.
static BYTE *make_step_thunk(BYTE *at, const void *next) {
    static const BYTE save[] = {0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0xf1};
    static const BYTE restore[] = {0x48, 0x83, 0xc4, 0x28, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x48, 0x85, 0xc0, 0x74, 0x04, 0x48, 0x89, 0xc6, 0xc3};
    static const BYTE end_state[] = {0x48, 0x89, 0xce, 0x48, 0x39, 0xce, 0xc3};
    at = put(at, save, sizeof save);
    at = put_call(at, next);
    at = put(at, restore, sizeof restore);
    at = put(at, edge_step_original, 4);
    at = put(at, edge_step_original + 8, 8);
    return put(at, end_state, sizeof end_state);
}

// The 25 bytes of an edge search's step at `site`: call the lane's step thunk, loop while ZF is clear.
static BOOL make_step(BYTE *out, BYTE *site, BYTE *loop, const BYTE *thunk) {
    memset(out, 0x90, sizeof edge_step_original);
    out[0] = 0xe8;
    out[5] = 0x0f, out[6] = 0x85;
    return put_rel32(out, 0, 5, site, thunk) && put_rel32(out, 5, 6, site, loop);
}

static void *copy_global(CopyState *state, uint32_t rva) {
    switch (rva) {
    case 0x344fef0: return &state->capacity;
    case 0x344fef4: return &state->size;
    case 0x344fef8: return &state->data;
    case 0x344ff00: return &state->guard;
    case 0x338fda5: return &state->ok;
    case 0x338fda8: return &state->failed_edge[0];
    case 0x338fdb0: return &state->failed_edge[1];
    }
    return NULL;
}

// A copy of an engine function at `copy`: each 32-bit offset re-aimed at its target from the copy, or for the globals in
// `state` at the copy's own, and a call to the edge insert at `edge_entry` when given. Every site is checked first.
static const char *copy_function(BYTE *copy, DWORD rva, DWORD size, const CopySite *sites, size_t count, CopyState *state, const void *edge_entry) {
    BYTE *original = api->engine + rva;
    memcpy(copy, original, size);
    for (size_t i = 0; i < count; i++) {
        int32_t rel;
        memcpy(&rel, original + sites[i].field, 4);
        BYTE *target = api->engine + sites[i].target_rva;
        if (original + sites[i].end + rel != target) return "an offset in the triangulation differs from the analysed build";
        void *own = state ? copy_global(state, sites[i].target_rva) : NULL;
        const BYTE *aimed = own ? own : edge_entry && sites[i].target_rva == EDGE_INSERT_RVA ? edge_entry : target;
        int64_t moved = aimed - (copy + sites[i].end);
        if (moved != (int32_t)moved) return "the copies are out of reach of the engine";
        rel = (int32_t)moved;
        memcpy(copy + sites[i].field, &rel, 4);
    }
    return NULL;
}

// The copies' unwind entries: the engine's, moved to the copy, with the engine's unwind data (the code is the same).
static int add_unwind_entries(int used, DWORD rva, DWORD size, BYTE *copy) {
    IMAGE_NT_HEADERS64 *headers = (IMAGE_NT_HEADERS64 *)(api->engine + ((IMAGE_DOS_HEADER *)api->engine)->e_lfanew);
    IMAGE_DATA_DIRECTORY directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const RUNTIME_FUNCTION *entries = (const RUNTIME_FUNCTION *)(api->engine + directory.VirtualAddress);
    DWORD copy_rva = (DWORD)(copy - api->engine);
    for (DWORD i = 0; i < directory.Size / sizeof *entries && used < MAX_UNWIND_ENTRIES; i++)
        if (entries[i].BeginAddress >= rva && entries[i].BeginAddress < rva + size)
            unwind_entries[used++] = (RUNTIME_FUNCTION){copy_rva + entries[i].BeginAddress - rva, copy_rva + entries[i].EndAddress - rva, entries[i].UnwindData};
    return used;
}

// 64 KB in the first free spot above the engine's image, within reach of its rel32 offsets.
static BYTE *allocate_near(void) {
    IMAGE_NT_HEADERS64 *headers = (IMAGE_NT_HEADERS64 *)(api->engine + ((IMAGE_DOS_HEADER *)api->engine)->e_lfanew);
    uintptr_t at = ((uintptr_t)api->engine + headers->OptionalHeader.SizeOfImage + 0xffff) & ~(uintptr_t)0xffff;
    for (; at < (uintptr_t)api->engine + 0x7f000000; at += 0x10000) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)at, &info, sizeof info)) break;
        if (info.State != MEM_FREE || info.RegionSize < NEAR_BYTES) continue;
        BYTE *memory = VirtualAlloc((void *)at, NEAR_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory) return memory;
    }
    return NULL;
}

// The thunks, the stub and the two copies (attempts 2 and 3, lanes 1 and 2), each a triangulator calling its lane's entry
// for its own edge insert, whose test call and step go to its lane's. Made before any of our patches.
static const char *make_near_code(void) {
    near_memory = allocate_near();
    if (!near_memory) return "no memory free within reach of the engine";
    BYTE *at = near_memory;
    point_stub = at;
    at = make_point_stub(at);
    for (int k = 0; k < SPLIT_LANES; k++) {
        at = (BYTE *)(((uintptr_t)at + 15) & ~(uintptr_t)15);
        crossing_thunks[k] = at, at = put_jump(at, lane_crossing_tests[k]);
        edge_entry_thunks[k] = at, at = put_jump(at, lane_edge_inserts[k]);
        at = (BYTE *)(((uintptr_t)at + 15) & ~(uintptr_t)15);
        step_thunks[k] = at, at = make_step_thunk(at, lane_next_triangles[k]);
    }
    int unwind_count = 0;
    for (int k = 0; k < SPLIT_LANES - 1; k++) {
        BYTE *triangulate = near_memory + 0x1000 + k * 0x2000, *edge = triangulate + 0x1000, *step = edge + EDGE_STEP_OFFSET;
        CopyState *state = (CopyState *)(near_memory + DATA_OFFSET) + k;
        state->guard = INT32_MIN;
        ahead[k].triangulate = (TriangulateFn)(void *)triangulate;
        ahead[k].state = state;
        lanes[k + 1].insert = (EdgeInsertFn)(void *)edge;
        const char *error = copy_function(edge, EDGE_INSERT_RVA, EDGE_INSERT_SIZE, edge_insert_sites, sizeof edge_insert_sites / sizeof *edge_insert_sites, state, NULL);
        if (!error)
            error = copy_function(triangulate, TRIANGULATE_RVA, TRIANGULATE_SIZE, triangulate_sites, sizeof triangulate_sites / sizeof *triangulate_sites, NULL,
                                  edge_entry_thunks[k + 1]);
        if (error) return error;
        if (!put_rel32(edge, CROSSING_CALL_OFFSET, 5, edge, crossing_thunks[k + 1]) || !make_step(step, step, edge + EDGE_LOOP_OFFSET, step_thunks[k + 1]))
            return "the copies are out of reach of their thunks";
        unwind_count = add_unwind_entries(unwind_count, TRIANGULATE_RVA, TRIANGULATE_SIZE, triangulate);
        unwind_count = add_unwind_entries(unwind_count, EDGE_INSERT_RVA, EDGE_INSERT_SIZE, edge);
    }
    DWORD old;
    if (!VirtualProtect(near_memory, CODE_BYTES, PAGE_EXECUTE_READ, &old)) return "VirtualProtect failed";
    FlushInstructionCache(GetCurrentProcess(), near_memory, CODE_BYTES);
    if (!unwind_count || unwind_count == MAX_UNWIND_ENTRIES || !RtlAddFunctionTable(unwind_entries, (DWORD)unwind_count, (DWORD64)api->engine))
        return "the copies' unwind entries could not be registered";
    return NULL;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    const char *error = S2_CHECK_ENGINE(api, pathsplit_checks);
    if (error) return error;
    if (memcmp(api->engine + POINT_ENTRY_RVA, point_entry_original, sizeof point_entry_original) ||
        memcmp(api->engine + EDGE_INSERT_RVA + CROSSING_CALL_OFFSET, crossing_call_original, sizeof crossing_call_original) ||
        memcmp(api->engine + EDGE_INSERT_RVA + EDGE_STEP_OFFSET, edge_step_original, sizeof edge_step_original))
        return "engine code differs from the analysed build";
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    qpc_per_ms = frequency.QuadPart / 1000.0;
    crosses = (CrossingTest)(void *)(api->engine + CROSSES_RVA);
    engine_free = (FreeFn)(void *)(api->engine + FREE_RVA);
    verify = verify_in_ini();
    error = make_near_code();
    if (error) return error;
    put_jump(point_entry_replacement, point_stub);
    memset(point_entry_replacement + 14, 0x90, sizeof point_entry_replacement - 14);
    crossing_call_replacement[0] = 0xe8;
    BYTE *edge = api->engine + EDGE_INSERT_RVA;
    if (!put_rel32(crossing_call_replacement, 0, 5, edge + CROSSING_CALL_OFFSET, crossing_thunks[0]) ||
        !make_step(edge_step_replacement, edge + EDGE_STEP_OFFSET, edge + EDGE_LOOP_OFFSET, step_thunks[0]))
        return "the engine is out of reach of the thunks";
    if (!split_start_helpers()) return "CreateThread failed";
    error = api->hook(edge, edge_insert_prologue, sizeof edge_insert_prologue, lane_edge_inserts[0], (void **)&lanes[0].insert);
    if (!error)
        error = api->hook(api->engine + TRIANGULATE_RVA, triangulate_prologue, sizeof triangulate_prologue, (const void *)triangulate_hook,
                          (void **)&original_triangulate);
    if (!error) error = patch_searches(TRUE);
    if (error) return error;
    api->register_switch("pathsplit", &split_enabled, NULL);
    api->log_line("pathsplit: the battle triangulation's attempts run side by side on 3 threads, their searches over %d helpers%s", SPLIT_HELPERS,
                  verify ? "; verify: each attempt also runs the engine's own way and is compared" : "");
    return NULL;
}
