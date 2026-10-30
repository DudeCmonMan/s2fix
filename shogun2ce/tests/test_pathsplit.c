#include "pathsplit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// pathsplit.h must answer as the engine's in-order loop does, with helpers on the asking lane's floating-point control
// word. A test's point carries its lane in [2], so the test knows which word to expect.
static int failures, checks;
static volatile LONG wrong_csr;
static unsigned int expected_csr[SPLIT_LANES];
static unsigned int odds;

static unsigned int mix(const BYTE *triangle, const float *a, const float *b) {
    unsigned int h = 2166136261u;
    for (int i = 0; i < SPLIT_TRIANGLE_BYTES; i++) h = (h ^ triangle[i]) * 16777619u;
    unsigned int bits;
    memcpy(&bits, a, sizeof bits), h = (h ^ bits) * 16777619u;
    if (b) memcpy(&bits, b, sizeof bits), h = (h ^ bits) * 16777619u;
    return h;
}

static unsigned char point_test(const void *triangle, const float *point) {
    if (_mm_getcsr() != expected_csr[(int)point[2]]) InterlockedIncrement(&wrong_csr);
    return odds && mix(triangle, point, NULL) % odds == 0;
}

static unsigned char crossing_test(const void *triangle, const float *a, const float *b) {
    if (_mm_getcsr() != expected_csr[(int)a[2]]) InterlockedIncrement(&wrong_csr);
    return odds && mix(triangle, a, b) % odds == 0;
}

// Answers that differ from the loop's.
static int compare(int lane, const BYTE *triangles, LONG count, BYTE *answers, int trials, unsigned int *seed) {
    int wrong = 0;
    for (int trial = 0; trial < trials; trial++) {
        *seed = *seed * 1103515245u + 12345u;
        float point[3] = {(float)(*seed >> 8), (float)trial, (float)lane}, other[3] = {(float)(*seed & 0xffff), (float)count, (float)lane};
        LONG first = count;
        for (LONG i = 0; i < count; i++)
            if (point_test(triangles + (size_t)i * SPLIT_TRIANGLE_BYTES, point)) {
                first = i;
                break;
            }
        wrong += split_point_search(lane, point_test, triangles, count, point) != first;
        split_crossing_tests(lane, crossing_test, triangles, count, point, other, answers);
        for (LONG i = 0; i < count; i++) wrong += answers[i] != crossing_test(triangles + (size_t)i * SPLIT_TRIANGLE_BYTES, point, other);
    }
    return wrong;
}

static BYTE *random_triangles(LONG count, unsigned int seed) {
    BYTE *triangles = malloc((size_t)count * SPLIT_TRIANGLE_BYTES + 1);
    for (size_t i = 0; i < (size_t)count * SPLIT_TRIANGLE_BYTES; i++) triangles[i] = (BYTE)((seed = seed * 1103515245u + 12345u) >> 16);
    return triangles;
}

typedef struct {
    int lane, wrong;
} Series;

static DWORD WINAPI run_series(LPVOID context) {
    Series *series = context;
    LONG count = 20000;
    unsigned int seed = 7u + (unsigned int)series->lane;
    BYTE *triangles = random_triangles(count, seed), *answers = malloc((size_t)count);
    _mm_setcsr(expected_csr[series->lane]);
    split_begin();
    series->wrong = compare(series->lane, triangles, count, answers, 200, &seed);
    split_end();
    free(triangles), free(answers);
    return 0;
}

int main(void) {
    if (!split_start_helpers()) return printf("CreateThread failed\n"), 1;
    unsigned int base = _mm_getcsr();
    // Flush-to-zero and denormals-are-zero; the default; round toward zero.
    expected_csr[0] = base | 0x8040, expected_csr[1] = base, expected_csr[2] = base | 0x6000;
    _mm_setcsr(expected_csr[0]);
    static const LONG sizes[] = {0, 1, 127, 128, 129, 1023, 5000, 20000, 100003};
    static const unsigned int all_odds[] = {0, 50000, 997, 13, 2};
    unsigned int seed = 1;
    split_begin();
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        LONG count = sizes[s];
        BYTE *triangles = random_triangles(count, (unsigned int)s), *answers = malloc((size_t)count + 1);
        for (size_t o = 0; o < sizeof all_odds / sizeof *all_odds; o++) {
            odds = all_odds[o];
            int wrong = compare(0, triangles, count, answers, 20, &seed);
            checks++;
            failures += wrong != 0;
            printf("%s %ld triangles, a test passing 1 in %u: %d of 20 point searches and crossing answers differ from the loop's\n", wrong ? "FAIL" : "ok",
                   count, odds, wrong);
        }
        free(triangles), free(answers);
    }
    split_end();
    checks++;
    failures += wrong_csr != 0;
    printf("%s the helpers ran with the asking thread's floating-point control word (%ld tests without it)\n", wrong_csr ? "FAIL" : "ok", wrong_csr);

    odds = 997;
    wrong_csr = 0;
    Series series[SPLIT_LANES];
    HANDLE threads[SPLIT_LANES];
    for (int k = 0; k < SPLIT_LANES; k++) {
        series[k] = (Series){k, 0};
        threads[k] = CreateThread(NULL, 0, run_series, &series[k], 0, NULL);
    }
    WaitForMultipleObjects(SPLIT_LANES, threads, TRUE, INFINITE);
    for (int k = 0; k < SPLIT_LANES; k++) {
        CloseHandle(threads[k]);
        checks++;
        failures += series[k].wrong != 0;
        printf("%s lane %d, searching while the other lanes search: %d of 200 point searches and crossing answers differ from the loop's\n",
               series[k].wrong ? "FAIL" : "ok", k, series[k].wrong);
    }
    checks++;
    failures += wrong_csr != 0;
    printf("%s with three lanes at once, every test ran with its own lane's control word (%ld tests without it)\n", wrong_csr ? "FAIL" : "ok", wrong_csr);
    printf("test_pathsplit: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
