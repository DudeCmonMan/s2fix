#include "../s2module.h"
#include "../pathsplit.h"
#include "../checks/pathsplit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The battle pathfinder's triangulation searches its triangles one by one, in up to three attempts on main. The
// searches run on helper threads, and attempts 2 and 3, predictable from the edges, start alongside attempt 1.
#define POINT_SEARCH_RVA 0x134e965
#define CROSSING_CALL_RVA 0x1369467
#define EDGE_STEP_RVA 0x1369544
#define EDGE_LOOP_RVA 0x1369460
#define EDGE_INSERT_RVA 0x1369370
#define EDGE_INSERT_SIZE 0x615
#define TRIANGULATE_RVA 0x1350ec0
#define TRIANGULATE_SIZE 0x3ff
#define POINT_IN_TRIANGLE_RVA 0x135ffc0
#define CROSSES_RVA 0x135f2a0
#define FREE_RVA 0x13564a0
#define FAILED_FLAG_RVA 0x1bd913c
#define FAILED_EDGE_RVA 0x1c99590
#define FILE_BASE 0x10000000u
#define MIN_SPLIT 1024
// The engine's retries leave the first four edges (the map's border) in place and use this generator for attempt 3.
#define FIRST_REORDERED 4
#define SHUFFLE_SEED 0x61266u
#define COPY_BYTES 0x1000

typedef int *(*TriangulateFn)(int index_count, const int *edges, const float *points, int *out);
typedef DWORD (*EdgeInsertFn)(BYTE *triangles, const float *a, const float *b);
typedef void (*FreeFn)(void *block);

typedef struct {
    DWORD offset, target_rva;
} CallSite;

typedef struct {
    DWORD offset, file_value;
} AbsoluteSite;

// From the engine's relocation table and its disassembly (the rel32 calls leaving each function); checked at install.
static const CallSite edge_insert_calls[] = {{0xf7, 0x135f2a0},  {0x12e, 0x8c2b0},   {0x158, 0x8c2b0},   {0x197, 0xf04d0},   {0x1b5, 0x1352790},
                                             {0x21f, 0x1352790}, {0x2c2, 0x1351360}, {0x2d7, 0x1370520}, {0x2eb, 0x135f2a0}, {0x2fd, 0x1352790},
                                             {0x313, 0x1352790}, {0x325, 0x135f2a0}, {0x3d4, 0x1351360}, {0x3eb, 0x1370520}, {0x3fb, 0x135f2a0},
                                             {0x40d, 0x1352790}, {0x423, 0x1352790}, {0x435, 0x135f2a0}, {0x4fa, 0x1352790}, {0x542, 0x1360f10},
                                             {0x584, 0x1351360}, {0x592, 0x1370520}, {0x59b, 0x1360f10}, {0x5e4, 0x14fc0d2}, {0x5fe, 0x14fbf8a},
                                             {0x608, 0x14fc088}};
static const AbsoluteSite edge_insert_addresses[] = {
    {0xb, 0x11cb06d0},   {0x14, 0x11c99668},  {0x3d, 0x11c99660},  {0x65, 0x11c99664},  {0x108, 0x11c9965c}, {0x111, 0x11c99664}, {0x126, 0x11c9965c},
    {0x135, 0x11c99660}, {0x13d, 0x11c99664}, {0x148, 0x11c99664}, {0x14f, 0x11c9965c}, {0x15f, 0x11c99660}, {0x170, 0x11c99664}, {0x181, 0x11c99660},
    {0x187, 0x11c99664}, {0x1a5, 0x11c99660}, {0x1b1, 0x11c99660}, {0x1f3, 0x11c99664}, {0x237, 0x11c99660}, {0x23d, 0x11c99664}, {0x244, 0x11c99660},
    {0x333, 0x11c99660}, {0x338, 0x11c99664}, {0x34e, 0x11c99660}, {0x353, 0x11c99664}, {0x35f, 0x11c99660}, {0x36a, 0x11c99664}, {0x373, 0x11c99660},
    {0x382, 0x11c99664}, {0x388, 0x11c99660}, {0x443, 0x11c99660}, {0x448, 0x11c99664}, {0x455, 0x11c99660}, {0x45d, 0x11c99664}, {0x464, 0x11c99660},
    {0x475, 0x11c99660}, {0x47a, 0x11c99664}, {0x488, 0x11c99664}, {0x48e, 0x11c99660}, {0x512, 0x11c99660}, {0x518, 0x11c99664}, {0x51f, 0x11c99660},
    {0x5a9, 0x11bd913c}, {0x5b1, 0x11c99590}, {0x5b9, 0x11c99594}, {0x5c1, 0x11c99598}, {0x5c9, 0x11c9959c}, {0x5d2, 0x11c99660}, {0x5e0, 0x11c99668},
    {0x5ee, 0x11c99668}, {0x5fa, 0x1159e150}, {0x604, 0x11c99668}};
static const CallSite triangulate_calls[] = {{0xd7, 0x102700},   {0x10a, 0x39fb70},  {0x123, 0x134e8c0}, {0x13e, 0x134e8c0},
                                             {0x16f, 0x134e930}, {0x180, 0x134e930}, {0x18c, 0x1369370}, {0x203, 0x136ecd0},
                                             {0x211, 0x1370520}, {0x256, 0x136ecd0}, {0x264, 0x1370520}, {0x2a9, 0x136ecd0},
                                             {0x2b7, 0x1370520}, {0x2e1, 0xf0510},   {0x3e0, 0xf04d0},   {0x3ed, 0xf04d0}};
static const AbsoluteSite triangulate_addresses[] = {{0x69, 0x115b2454}, {0x2d9, 0x117952bc}};

typedef struct {
    LONG64 point_searches, point_triangles, edge_inserts, crossing_passes;
    LONGLONG point_ticks, edge_ticks, crossing_ticks, total_ticks;
} Stats;

typedef struct {
    int *indices, index_count;
    BYTE failed;
    DWORD failed_edge[4];
    Stats stats;
} Result;

// One attempt's search state.
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

// A copy's own globals; the guard is preset so the one-time set-up never runs.
typedef struct {
    DWORD capacity, size;
    void *data;
    LONG guard;
    DWORD failed_edge[4];
    BYTE ok;
} CopyState;

typedef struct {
    BYTE *triangulate;
    CopyState state;
    int *edges, index_count, out[16], *result;
    const float *points;
    unsigned int csr;
    unsigned short x87;
    HANDLE thread;
    BOOL served;
} Ahead;

// 0: the searches on main only and every attempt in turn, the engine's way.
__declspec(dllexport) volatile LONG split_enabled = 1;

static const BYTE point_search_original[32] = {0x3b, 0xf7, 0x74, 0x1c, 0x8b, 0x5c, 0x24, 0x1c, 0x0f, 0x1f, 0x00, 0x53, 0x56, 0xe8, 0x49, 0x16,
                                               0x01, 0x00, 0x83, 0xc4, 0x08, 0x84, 0xc0, 0x75, 0x0f, 0x83, 0xc6, 0x1c, 0x3b, 0xf7, 0x75, 0xeb};
static const BYTE crossing_call_original[5] = {0xe8, 0x34, 0x5e, 0xff, 0xff};
static const BYTE edge_step_original[29] = {0x8b, 0x45, 0x04, 0x83, 0xc7, 0x1c, 0x8d, 0x0c, 0xc5, 0x00, 0x00, 0x00, 0x00, 0x2b, 0xc8,
                                            0x8b, 0x45, 0x08, 0x8d, 0x04, 0x88, 0x3b, 0xf8, 0x0f, 0x85, 0xff, 0xfe, 0xff, 0xff};
static const BYTE edge_insert_prologue[] = {0x64, 0xa1, 0x2c, 0x00, 0x00, 0x00};
static const BYTE triangulate_prologue[] = {0x83, 0xec, 0x5c, 0x33, 0xc9};
static BYTE point_search_replacement[32], crossing_call_replacement[5], edge_step_replacement[29];

static const S2Api *api;
static TriangulateFn original_triangulate;
static PointTest point_in_triangle;
static CrossingTest crosses;
static FreeFn engine_free;
static BOOL verify;
static BYTE *copies;
static Lane lanes[SPLIT_LANES];
static Ahead ahead[SPLIT_LANES - 1];
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

// Returns the triangle the engine's loop would stop at; the loop is shared, so the lane is found by thread.
static BYTE *point_search(BYTE *begin, BYTE *end, const float *point) {
    LONG count = (LONG)((end - begin) / SPLIT_TRIANGLE_BYTES), found = count;
    DWORD thread = GetCurrentThreadId();
    int k = 0;
    while (k < SPLIT_LANES && lanes[k].thread != thread) k++;
    LONGLONG started = now();
    if (k < SPLIT_LANES && lanes[k].splitting && count >= MIN_SPLIT) {
        found = split_point_search(k, point_in_triangle, begin, count, point);
    } else {
        for (LONG i = 0; i < count; i++)
            if (point_in_triangle(begin + (size_t)i * SPLIT_TRIANGLE_BYTES, point)) {
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

// A pass's answers are worked out for the whole array at its first triangle; the search doesn't change the array.
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

// With a pass's answers, skips to the next crossing triangle: the engine's loop does nothing for the others.
static BYTE *next_triangle(Lane *lane, BYTE *current) {
    if (lane->pass_count && current >= lane->pass_data) {
        LONG index = (LONG)((current - lane->pass_data) / SPLIT_TRIANGLE_BYTES) + 1;
        const BYTE *hit = index < lane->pass_count ? memchr(lane->answers + index, 1, (size_t)(lane->pass_count - index)) : NULL;
        return hit ? (BYTE *)lane->pass_data + (size_t)(hit - lane->answers) * SPLIT_TRIANGLE_BYTES : NULL;
    }
    BYTE *data = *(BYTE **)(lane->edge_triangles + 8);
    current += SPLIT_TRIANGLE_BYTES;
    return current != data + (size_t)*(LONG *)(lane->edge_triangles + 4) * SPLIT_TRIANGLE_BYTES ? current : NULL;
}

static DWORD edge_insert(Lane *lane, BYTE *triangles, const float *a, const float *b) {
    LONGLONG started = now();
    lane->edge_triangles = triangles, lane->pass_count = 0;
    DWORD result = lane->insert(triangles, a, b);
    lane->edge_triangles = NULL, lane->pass_count = 0;
    lane->stats.edge_inserts++, lane->stats.edge_ticks += now() - started;
    return result;
}

// Lane 0's entries are called from the engine's edge insert, the others' from their copies.
#define LANE_ENTRIES(k)                                                                                                                              \
    static unsigned char crossing_test_##k(const BYTE *triangle, const float *a, const float *b) { return crossing_test(&lanes[k], triangle, a, b); } \
    static BYTE *next_triangle_##k(BYTE *current) { return next_triangle(&lanes[k], current); }                                                     \
    static DWORD edge_insert_##k(BYTE *triangles, const float *a, const float *b) { return edge_insert(&lanes[k], triangles, a, b); }
LANE_ENTRIES(0)
LANE_ENTRIES(1)
LANE_ENTRIES(2)
static const void *const lane_crossing_tests[SPLIT_LANES] = {(const void *)crossing_test_0, (const void *)crossing_test_1, (const void *)crossing_test_2};
static const void *const lane_next_triangles[SPLIT_LANES] = {(const void *)next_triangle_0, (const void *)next_triangle_1, (const void *)next_triangle_2};
static const void *const lane_edge_inserts[SPLIT_LANES] = {(const void *)edge_insert_0, (const void *)edge_insert_1, (const void *)edge_insert_2};

static const char *patch_searches(BOOL ours) {
    static const struct {
        DWORD rva;
        const BYTE *original, *replacement;
        size_t size;
    } patches[] = {{POINT_SEARCH_RVA, point_search_original, point_search_replacement, sizeof point_search_original},
                   {CROSSING_CALL_RVA, crossing_call_original, crossing_call_replacement, sizeof crossing_call_original},
                   {EDGE_STEP_RVA, edge_step_original, edge_step_replacement, sizeof edge_step_original}};
    for (size_t i = 0; i < sizeof patches / sizeof *patches; i++) {
        const char *error = api->patch(api->engine + patches[i].rva, ours ? patches[i].original : patches[i].replacement,
                                       ours ? patches[i].replacement : patches[i].original, patches[i].size);
        if (error) return error;
    }
    return NULL;
}

static Result run(int index_count, const int *edges, const float *points, int *out, BOOL split) {
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
    unsigned short x87 = attempt->x87;
    _mm_setcsr(attempt->csr);
    __asm__ volatile("fldcw %0" : : "m"(x87));
    memset(&lane->stats, 0, sizeof lane->stats);
    lane->splitting = TRUE;
    lane->thread = GetCurrentThreadId();
    attempt->state.ok = 1;
    memset(attempt->out, 0, sizeof attempt->out);
    split_begin();
    LONGLONG started = now();
    attempt->result = ((TriangulateFn)(void *)attempt->triangulate)(attempt->index_count, attempt->edges, attempt->points, attempt->out);
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

static void drop_ahead(void) {
    wait_ahead();
    for (int k = 0; k < SPLIT_LANES - 1; k++) {
        Ahead *attempt = &ahead[k];
        if (!attempt->thread) continue;
        CloseHandle(attempt->thread);
        if (!attempt->served) engine_free(attempt->result);
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

// Mirrors the engine's reorderings before attempts 2 and 3 (0x11349be7 to 0x11349cc9) exactly.
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
    unsigned short x87;
    __asm__ volatile("fnstcw %0" : "=m"(x87));
    for (int k = 0; k < SPLIT_LANES - 1; k++) {
        Ahead *attempt = &ahead[k];
        attempt->edges = k ? third : second, attempt->index_count = index_count, attempt->points = points;
        attempt->csr = _mm_getcsr(), attempt->x87 = x87, attempt->served = FALSE, attempt->result = NULL;
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

// As if it had run here: the engine owns the index list, and the failure globals change only if the attempt failed.
static int *serve_ahead(Ahead *attempt, int *out, LONGLONG *waited) {
    LONGLONG started = now();
    WaitForSingleObject(attempt->thread, INFINITE);
    *waited = now() - started;
    attempt->served = TRUE;
    out[0] = attempt->out[0];
    if (!attempt->state.ok) {
        *(api->engine + FAILED_FLAG_RVA) = 0;
        memcpy(api->engine + FAILED_EDGE_RVA, attempt->state.failed_edge, sizeof attempt->state.failed_edge);
    }
    return attempt->result;
}

static int *verify_attempt_one(int number, int index_count, const int *edges, const float *points, int *out) {
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
    int single_out[16] = {0}, split_out[16] = {0};
    // The copies run the shared point insert, so they must finish before its search is switched back to the engine's.
    Result split = run(index_count, edges_copy, points_copy, split_out, TRUE);
    LONGLONG waited = wait_ahead(), all_done = now() - triangulation_started;
    Result single = run(index_count, edges_copy, points_copy, single_out, FALSE);
    const char *error = patch_searches(FALSE);
    Result engine = run(index_count, edges, points, out, FALSE);
    if (!error) error = patch_searches(TRUE);
    describe(single_text, sizeof single_text, "ours on main", &single.stats);
    describe(split_text, sizeof split_text, "ours split", &split.stats);
    api->log_line("pathsplit: verify, triangulation %d, attempt 1: %d edges, %d triangles%s; the engine's loops %.0f ms; %s; %s; on main %s, split %s; "
                  "attempts 2 and 3 alongside done %.0f ms after attempt 1 began (main waited %.0f ms for them after its split run)%s%s",
                  number, index_count / 2, engine.index_count / 3, engine.failed ? " (failed, the engine retries)" : "", ms(engine.stats.total_ticks),
                  single_text, split_text, same(&single, &engine) ? "identical" : "DIFFERENT", same(&split, &engine) ? "identical" : "DIFFERENT",
                  ms(all_done), ms(waited), error ? "; the searches could not be switched back: " : "", error ? error : "");
    engine_free(single.indices);
    engine_free(split.indices);
    free(edges_copy), free(points_copy);
    return engine.indices;
}

static int *triangulate_hook(int index_count, const int *edges, const float *points, int *out) {
    if (!*api->fix_enabled) return original_triangulate(index_count, edges, points, out);
    Ahead *attempt = split_enabled ? predicted(index_count, edges, points) : NULL;
    if (attempt) {
        int number = (int)(attempt - ahead) + 2;
        char text[320];
        LONGLONG waited;
        int *indices = serve_ahead(attempt, out, &waited);
        BOOL failed = !attempt->state.ok;
        describe(text, sizeof text, "split on its thread", &lanes[number - 1].stats);
        if (!verify) {
            api->log_line("pathsplit: triangulation %d, attempt %d: run alongside (input as predicted), %d triangles%s, %s; main waited %.0f ms, "
                          "%.0f ms since attempt 1 began",
                          triangulations, number, out[0] / 3, failed ? " (failed)" : "", text, ms(waited), ms(now() - triangulation_started));
            return indices;
        }
        Result served = {indices, out[0], failed, {0}, {0}};
        memcpy(served.failed_edge, attempt->state.failed_edge, sizeof served.failed_edge);
        const char *error = patch_searches(FALSE);
        Result engine = run(index_count, edges, points, out, FALSE);
        if (!error) error = patch_searches(TRUE);
        api->log_line("pathsplit: verify, triangulation %d, attempt %d: the engine's loops %.0f ms; run alongside (input as predicted), %s; %s%s%s",
                      triangulations, number, ms(engine.stats.total_ticks), text, same(&served, &engine) ? "identical" : "DIFFERENT",
                      error ? "; the searches could not be switched back: " : "", error ? error : "");
        engine_free(indices);
        return engine.indices;
    }
    // A retry the prediction didn't match runs here, in turn.
    if (split_enabled && ahead[0].thread && !(ahead[0].served && ahead[1].served) && ahead[0].index_count == index_count && ahead[0].points == points) {
        Result result = run(index_count, edges, points, out, TRUE);
        api->log_line("pathsplit: triangulation %d: a retry's input differs from the prediction; it ran here, %.0f ms", triangulations,
                      ms(result.stats.total_ticks));
        return result.indices;
    }
    int number = ++triangulations;
    drop_ahead();
    triangulation_started = now();
    if (split_enabled) start_ahead(index_count, edges, points);
    if (verify) return verify_attempt_one(number, index_count, edges, points, out);
    char text[320];
    Result result = run(index_count, edges, points, out, split_enabled);
    describe(text, sizeof text, split_enabled ? "split" : "on main", &result.stats);
    api->log_line("pathsplit: triangulation %d: %d edges, %d triangles%s; %s", number, index_count / 2, result.index_count / 3,
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

static void call_at(BYTE *code, size_t at, BYTE *site, const void *target) {
    code[at] = 0xe8;
    *(INT32 *)(code + at + 1) = (INT32)((const BYTE *)target - (site + at + 5));
}

// The edge search's step for `site`: edi = next(edi), keeping edx (the engine's hit count); loops while not null.
static void make_step(BYTE *out, BYTE *site, BYTE *loop, const void *next) {
    static const BYTE body[] = {0x52, 0x57, 0, 0, 0, 0, 0, 0x83, 0xc4, 0x04, 0x5a, 0x8b, 0xf8, 0x85, 0xff, 0x0f, 0x85, 0, 0, 0, 0};
    memset(out, 0x90, sizeof edge_step_original);
    memcpy(out, body, sizeof body);
    call_at(out, 2, site, next);
    *(INT32 *)(out + 17) = (INT32)(loop - (site + 21));
}

static void *copy_global(CopyState *state, DWORD file_value) {
    switch (file_value) {
    case 0x11c9965c: return &state->capacity;
    case 0x11c99660: return &state->size;
    case 0x11c99664: return &state->data;
    case 0x11c99668: return &state->guard;
    case 0x11bd913c: return &state->ok;
    case 0x11c99590:
    case 0x11c99594:
    case 0x11c99598:
    case 0x11c9959c: return &state->failed_edge[(file_value - 0x11c99590) / 4];
    }
    return NULL;
}

// Calls re-aimed from the copy (the edge insert's to `edge_insert`); the globals in `state` point at the copy's own.
static const char *copy_function(BYTE *copy, DWORD rva, DWORD size, const CallSite *calls, size_t call_count, const AbsoluteSite *addresses,
                                 size_t address_count, CopyState *state, const void *edge_insert) {
    BYTE *original = api->engine + rva;
    memcpy(copy, original, size);
    for (size_t i = 0; i < call_count; i++) {
        BYTE *site = original + calls[i].offset;
        if (*site != 0xe8 || site + 5 + *(INT32 *)(site + 1) != api->engine + calls[i].target_rva) return "a call in the triangulation differs from the analysed build";
        call_at(copy, calls[i].offset, copy, edge_insert && calls[i].target_rva == EDGE_INSERT_RVA ? edge_insert : api->engine + calls[i].target_rva);
    }
    for (size_t i = 0; i < address_count; i++) {
        DWORD *field = (DWORD *)(copy + addresses[i].offset);
        if (*field != (DWORD)(uintptr_t)(api->engine + (addresses[i].file_value - FILE_BASE))) return "an address in the triangulation differs from the analysed build";
        void *own = state ? copy_global(state, addresses[i].file_value) : NULL;
        if (own) *field = (DWORD)(uintptr_t)own;
    }
    return NULL;
}

// Attempts 2 and 3 (lanes 1 and 2), made before our hooks and patches so they copy the engine's own bytes.
static const char *make_copies(void) {
    copies = VirtualAlloc(NULL, (SPLIT_LANES - 1) * COPY_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!copies) return "VirtualAlloc failed";
    for (int k = 0; k < SPLIT_LANES - 1; k++) {
        BYTE *triangulate = copies + k * COPY_BYTES, *edge_insert = triangulate + 0x400, *step = edge_insert + (EDGE_STEP_RVA - EDGE_INSERT_RVA);
        ahead[k].triangulate = triangulate;
        ahead[k].state.guard = INT32_MIN;
        const char *error = copy_function(edge_insert, EDGE_INSERT_RVA, EDGE_INSERT_SIZE, edge_insert_calls, sizeof edge_insert_calls / sizeof *edge_insert_calls,
                                          edge_insert_addresses, sizeof edge_insert_addresses / sizeof *edge_insert_addresses, &ahead[k].state, NULL);
        if (!error)
            error = copy_function(triangulate, TRIANGULATE_RVA, TRIANGULATE_SIZE, triangulate_calls, sizeof triangulate_calls / sizeof *triangulate_calls,
                                  triangulate_addresses, sizeof triangulate_addresses / sizeof *triangulate_addresses, NULL, lane_edge_inserts[k + 1]);
        if (error) return error;
        call_at(edge_insert, CROSSING_CALL_RVA - EDGE_INSERT_RVA, edge_insert, lane_crossing_tests[k + 1]);
        make_step(step, step, edge_insert + (EDGE_LOOP_RVA - EDGE_INSERT_RVA), lane_next_triangles[k + 1]);
        lanes[k + 1].insert = (EdgeInsertFn)(void *)edge_insert;
    }
    DWORD old;
    if (!VirtualProtect(copies, (SPLIT_LANES - 1) * COPY_BYTES, PAGE_EXECUTE_READ, &old)) return "VirtualProtect failed";
    FlushInstructionCache(GetCurrentProcess(), copies, (SPLIT_LANES - 1) * COPY_BYTES);
    return NULL;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    const char *error = S2_CHECK_ENGINE(api, pathsplit_checks);
    if (error) return error;
    if (memcmp(api->engine + POINT_SEARCH_RVA, point_search_original, sizeof point_search_original) ||
        memcmp(api->engine + CROSSING_CALL_RVA, crossing_call_original, sizeof crossing_call_original) ||
        memcmp(api->engine + EDGE_STEP_RVA, edge_step_original, sizeof edge_step_original) ||
        !s2_prologue_ready(api->engine + EDGE_INSERT_RVA, edge_insert_prologue, sizeof edge_insert_prologue) ||
        !s2_prologue_ready(api->engine + TRIANGULATE_RVA, triangulate_prologue, sizeof triangulate_prologue))
        return "engine code differs from the analysed build";
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    qpc_per_ms = frequency.QuadPart / 1000.0;
    point_in_triangle = (PointTest)(void *)(api->engine + POINT_IN_TRIANGLE_RVA);
    crosses = (CrossingTest)(void *)(api->engine + CROSSES_RVA);
    engine_free = (FreeFn)(void *)(api->engine + FREE_RVA);
    verify = verify_in_ini();
    error = make_copies();
    if (error) return error;
    // ebx = the point; point_search(start, end, point), then the engine's found (0x1134e98d) or not found (0x1134e985).
    static const BYTE search_body[] = {0x8b, 0x5c, 0x24, 0x1c, 0x53, 0x57, 0x56, 0, 0, 0, 0, 0, 0x83, 0xc4, 0x0c, 0x8b, 0xf0, 0x3b, 0xf7, 0x74, 0x0b, 0xeb, 0x11};
    memset(point_search_replacement, 0x90, sizeof point_search_replacement);
    memcpy(point_search_replacement, search_body, sizeof search_body);
    call_at(point_search_replacement, 7, api->engine + POINT_SEARCH_RVA, (const void *)point_search);
    call_at(crossing_call_replacement, 0, api->engine + CROSSING_CALL_RVA, lane_crossing_tests[0]);
    make_step(edge_step_replacement, api->engine + EDGE_STEP_RVA, api->engine + EDGE_LOOP_RVA, lane_next_triangles[0]);
    if (!split_start_helpers()) return "CreateThread failed";
    error = api->hook(api->engine + EDGE_INSERT_RVA, edge_insert_prologue, sizeof edge_insert_prologue, lane_edge_inserts[0], (void **)&lanes[0].insert);
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
