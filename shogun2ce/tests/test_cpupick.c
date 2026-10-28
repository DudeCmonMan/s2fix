#include <stdio.h>
#include <string.h>
#include "cpupick.h"

// Checks the placement rules on CPU layouts by shape (what Windows reports, never a model), plus two real readouts.
static CpuInfo cpus[CPU_MAX];
static int count, failures, checks, twins = 1;

// Appends cores of `threads` logical CPUs each; Windows numbers a core and a cache by their first CPU.
static void add(int cores, int threads, int cache, int efficiency, uint32_t mb, const uint8_t *ranks) {
    for (int core = 0; core < cores; core++) {
        int first = count;
        for (int t = 0; t < threads; t++)
            cpus[count++] = (CpuInfo){(uint8_t)first, (uint8_t)cache, (uint8_t)efficiency, ranks ? ranks[core] : 0, mb << 20};
    }
}

static void reset(void) {
    count = 0;
    memset(cpus, 0, sizeof cpus);
}

static void expect(const char *shape, int number, int confine, int own_core, int group, int main_cpu, uint64_t main_cpus, uint64_t others,
                   const char *why) {
    const char *got_why = "";
    int got_group = cpu_pick_group(cpus, count, number, &got_why);
    CpuPlacement got = got_group < 0 ? (CpuPlacement){-1, 0, 0} : cpu_placement(cpus, count, got_group, confine, own_core, twins);
    checks++;
    if (got_group != group || got.main_cpu != main_cpu || got.main_cpus != main_cpus || got.others != others || (why && strcmp(why, got_why))) {
        failures++;
        printf("FAIL %s (group %d, confine %d, own core %d, twins %d): group %d main %d on %llx others %llx (%s); expected group %d main %d on %llx "
               "others %llx (%s)\n",
               shape, number, confine, own_core, twins, got_group, got.main_cpu, (unsigned long long)got.main_cpus, (unsigned long long)got.others,
               got_why, group, main_cpu, (unsigned long long)main_cpus, (unsigned long long)others, why ? why : "any");
    }
}

static void expect_list(uint64_t mask, const char *text) {
    char got[160];
    checks++;
    if (strcmp(cpu_list(mask, got, sizeof got), text)) {
        failures++;
        printf("FAIL cpu_list %llx: \"%s\", expected \"%s\"\n", (unsigned long long)mask, got, text);
    }
}

int main(void) {
    // Two cache groups of different sizes, SMT: this PC's 9950X3D, read on the desktop and with a game running (the
    // V-Cache driver then ranks the cache group higher, and two cores tie at the top).
    static const uint8_t desktop0[] = {5, 3, 6, 4, 7, 2, 0, 1}, desktop1[] = {10, 14, 13, 11, 12, 14, 8, 9};
    static const uint8_t game0[] = {13, 11, 14, 12, 14, 10, 8, 9}, game1[] = {2, 7, 5, 3, 4, 6, 0, 1};
    reset();
    add(8, 2, 0, 0, 96, desktop0);
    add(8, 2, 16, 0, 32, desktop1);
    expect("larger L3, desktop ranks", -1, 1, 1, 0, 8, 0x300, 0xfcff, "the largest L3");
    reset();
    add(8, 2, 0, 0, 96, game0);
    add(8, 2, 16, 0, 32, game1);
    expect("larger L3, game ranks (tie: not CPU 0's core, then the first)", -1, 1, 1, 0, 4, 0x30, 0xffcf, "the largest L3");
    expect("larger L3, main with them", -1, 1, 0, 0, -1, 0, 0xffff, NULL);
    expect("larger L3, main's core only (cpuplace off)", -1, 0, 1, 0, 4, 0x30, 0xffffffcfull, NULL);
    expect("larger L3, both off", -1, 0, 0, 0, -1, 0, 0, NULL);
    expect("group 1 from the ini", 1, 1, 1, 16, 18, 0xc0000, 0xfff30000, "set in s2prof.ini");
    expect("group 2 does not exist", 2, 1, 1, -1, -1, 0, 0, NULL);
    twins = 0;
    expect("larger L3, one CPU per core", -1, 1, 1, 0, 4, 0x30, 0x5545, NULL);
    twins = 1;

    // Two equal cache groups; the second holds the highest-ranked core.
    static const uint8_t low[] = {1, 2, 3, 4, 5, 6, 7, 8}, high[] = {9, 10, 11, 12, 13, 14, 2, 1};
    reset();
    add(8, 2, 0, 0, 32, low);
    add(8, 2, 16, 0, 32, high);
    expect("equal L3, higher rank", -1, 1, 1, 16, 26, 0xc000000, 0xf3ff0000, "the highest-ranked core");

    // Equal groups and no ranking: the first group, main off CPU 0's core.
    reset();
    add(8, 2, 0, 0, 32, NULL);
    add(8, 2, 16, 0, 32, NULL);
    expect("equal groups, no ranking", -1, 1, 1, 0, 2, 0xc, 0xfff3, "the first of equal groups");

    // Two groups of 6 SMT cores: 10 CPUs left for the others, still confined.
    reset();
    add(6, 2, 0, 0, 96, NULL);
    add(6, 2, 12, 0, 32, NULL);
    expect("6-core SMT groups", -1, 1, 1, 0, 2, 0xc, 0xff3, "the largest L3");

    // Four cache groups of four SMT cores, one ranked highest: 6 CPUs left in it, so the others go wide.
    static const uint8_t ccx[] = {1, 1, 1, 1}, ccx_best[] = {1, 3, 2, 1};
    reset();
    add(4, 2, 0, 0, 16, ccx);
    add(4, 2, 8, 0, 16, ccx);
    add(4, 2, 16, 0, 16, ccx_best);
    add(4, 2, 24, 0, 16, ccx);
    expect("four groups of 4 cores", -1, 1, 1, 16, 18, 0xc0000, 0xfff3ffff, "the highest-ranked core");

    // One group, all cores alike, SMT, no ranking.
    reset();
    add(8, 2, 0, 0, 32, NULL);
    expect("one group, no ranking", -1, 1, 1, 0, 2, 0xc, 0xfff3, "the only one");
    reset();
    add(4, 2, 0, 0, 16, NULL);
    expect("4 SMT cores", -1, 1, 1, 0, 2, 0xc, 0xf3, "the only one");

    // Fast cores with SMT beside slow cores without it, one L3 for both.
    static const uint8_t p_ranks[] = {4, 4, 6, 6, 4, 4, 5, 5};
    reset();
    add(8, 2, 0, 1, 36, p_ranks);
    add(16, 1, 0, 0, 36, NULL);
    expect("fast SMT cores and slow cores", -1, 1, 1, 0, 4, 0x30, 0xffcf, "the only one");
    expect("fast SMT cores and slow cores, main's core only", -1, 0, 1, 0, 4, 0x30, 0xffffffcfull, NULL);

    // Fast cores without SMT beside slow cores: too few for the others, so they go wide, and main, on a core of one CPU,
    // may use theirs too.
    static const uint8_t p6[] = {3, 3, 2, 5, 2, 3};
    reset();
    add(6, 1, 0, 1, 24, p6);
    add(8, 1, 0, 0, 24, NULL);
    expect("fast cores without SMT", -1, 1, 1, 0, 3, 0x3fff, 0x3ff7, "the only one");
    // The same numbered out of order (fast cores at 0-1 and 10-13), low-power cores on a cache of their own.
    static const uint8_t first2[] = {3, 3}, last4[] = {2, 5, 2, 3};
    reset();
    add(2, 1, 0, 1, 24, first2);
    add(8, 1, 0, 0, 24, NULL);
    add(4, 1, 0, 1, 24, last4);
    add(2, 1, 14, 0, 0, NULL);
    expect("out of order, low-power cores", -1, 1, 1, 0, 11, 0xffff, 0xf7ff, "the only one");
    for (int c = 2; c < 10; c++) cpus[c].efficiency_class = 1;
    for (int c = 0; c < 2; c++) cpus[c].efficiency_class = cpus[c + 10].efficiency_class = cpus[c + 12].efficiency_class = 2;
    expect("three classes", -1, 1, 1, 0, 11, 0xffff, 0xf7ff, "the only one");

    // Few cores.
    reset();
    add(2, 2, 0, 0, 8, NULL);
    expect("two cores, SMT", -1, 1, 1, 0, 2, 0xc, 0x3, NULL);
    reset();
    add(2, 1, 0, 0, 8, NULL);
    expect("two cores", -1, 1, 1, 0, 1, 0x3, 0x1, NULL);
    reset();
    add(3, 1, 0, 0, 8, NULL);
    expect("three cores", -1, 1, 1, 0, 1, 0x7, 0x5, NULL);
    reset();
    add(1, 2, 0, 0, 8, NULL);
    expect("one core, SMT", -1, 1, 1, 0, -1, 0, 0, NULL);
    expect("one core, SMT, main's core only", -1, 0, 1, 0, -1, 0, 0, NULL);

    // 64 CPUs, the mask's top bit.
    reset();
    add(32, 2, 0, 0, 128, NULL);
    expect("64 CPUs", -1, 1, 1, 0, 2, 0xc, 0xfffffffffffffff3ull, NULL);

    expect_list(0, "");
    expect_list(0xffff, "0-15");
    expect_list(0x5555, "0,2,4,6,8,10,12,14");
    expect_list(0x10f, "0-3,8");
    expect_list(0xc000000000000001ull, "0,62-63");

    printf("test_cpupick: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
