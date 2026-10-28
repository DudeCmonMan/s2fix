#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The CPU placement rules, apart from Windows so they can be checked on any layout. They read only what Windows reports
// for each logical CPU, never a CPU model. Main gets the best-ranked core of the fastest core type's largest cache group
// (a CCD, or the P-cores); the other threads stay in that group, SMT twins included, when it leaves them enough CPUs, and
// otherwise go wide. Measured in the original game (2026-10-27): one CCD of 8 SMT cores ran as well as every CPU, while
// 7 CPUs froze frames up to 100 ms when the camera moved, and main held to one CPU stalled 50-130 ms.
#define CPU_MAX 64
// A 6-core SMT group less main's core, expected to behave like the 8-core one measured.
#define CPU_MIN_OTHERS 10

typedef struct {
    uint8_t core_index, cache_index, efficiency_class, scheduling_class;
    uint32_t l3_bytes;
} CpuInfo;

typedef struct {
    int main_cpu;
    uint64_t main_cpus, others;
} CpuPlacement;

static inline uint64_t cpu_all(int count) {
    return count >= 64 ? ~0ull : (1ull << count) - 1;
}

static inline int cpu_same_core(const CpuInfo *cpus, int a, int b) {
    return cpus[a].core_index == cpus[b].core_index && cpus[a].cache_index == cpus[b].cache_index;
}

static inline uint64_t cpu_core_of(const CpuInfo *cpus, int count, int cpu) {
    uint64_t mask = 0;
    for (int c = 0; c < count; c++)
        if (cpu_same_core(cpus, c, cpu)) mask |= 1ull << c;
    return mask;
}

static inline uint64_t cpu_one_per_core(const CpuInfo *cpus, int count, uint64_t mask) {
    uint64_t picked = 0;
    for (int c = 0; c < count; c++)
        if (mask >> c & 1 && !(picked & cpu_core_of(cpus, count, c))) picked |= 1ull << c;
    return picked;
}

static inline int cpu_count_of(uint64_t mask) {
    return __builtin_popcountll(mask);
}

// The fastest-type CPUs sharing cpu's last-level cache. On a hybrid CPU the slower cores share the L3 too, or sit on a
// cache of their own (low-power cores), so the type is filtered first.
static inline uint64_t cpu_group(const CpuInfo *cpus, int count, int cpu) {
    uint8_t fastest = 0;
    uint64_t mask = 0;
    for (int c = 0; c < count; c++)
        if (cpus[c].efficiency_class > fastest) fastest = cpus[c].efficiency_class;
    for (int c = 0; c < count; c++)
        if (cpus[c].efficiency_class == fastest && cpus[c].cache_index == cpus[cpu].cache_index) mask |= 1ull << c;
    return mask;
}

// Groups are numbered by their first CPU, so group 0 holds CPU 0 unless CPU 0 is a slower type.
static inline int cpu_groups(const CpuInfo *cpus, int count, int *firsts) {
    int groups = 0;
    for (int c = 0; c < count; c++) {
        uint64_t group = cpu_group(cpus, count, c);
        if (group && __builtin_ctzll(group) == c) firsts[groups++] = c;
    }
    return groups;
}

static inline uint8_t cpu_best_rank(const CpuInfo *cpus, int count, uint64_t mask) {
    uint8_t best = 0;
    for (int c = 0; c < count; c++)
        if (mask >> c & 1 && cpus[c].scheduling_class > best) best = cpus[c].scheduling_class;
    return best;
}

// number < 0 applies the rules: the largest L3, then the highest-ranked core (SchedulingClass, undocumented, higher is
// preferred), then the first group. Returns the group's first CPU, or -1 when the numbered group does not exist.
static inline int cpu_pick_group(const CpuInfo *cpus, int count, int number, const char **why) {
    int firsts[CPU_MAX] = {0}, groups = cpu_groups(cpus, count, firsts);
    if (number >= 0) {
        *why = "set in s2prof.ini";
        return number < groups ? firsts[number] : -1;
    }
    int best = firsts[0], sizes_differ = 0, ranks_differ = 0;
    uint8_t best_rank = cpu_best_rank(cpus, count, cpu_group(cpus, count, best));
    for (int g = 1; g < groups; g++) {
        uint32_t bytes = cpus[firsts[g]].l3_bytes;
        uint8_t rank = cpu_best_rank(cpus, count, cpu_group(cpus, count, firsts[g]));
        sizes_differ |= bytes != cpus[firsts[0]].l3_bytes;
        ranks_differ |= rank != best_rank;
        if (bytes > cpus[best].l3_bytes || (bytes == cpus[best].l3_bytes && rank > best_rank)) {
            best = firsts[g];
            best_rank = rank;
        }
    }
    *why = groups == 1 ? "the only one" : sizes_differ ? "the largest L3" : ranks_differ ? "the highest-ranked core" : "the first of equal groups";
    return best;
}

// The group's highest-ranked core, as its first CPU. Between equal ones CPU 0's core comes last: Windows and drivers
// favour CPU 0 for system work.
static inline int cpu_main_cpu(const CpuInfo *cpus, int count, uint64_t group) {
    int best = -1;
    for (int c = 0; c < count; c++) {
        if (!(group >> c & 1)) continue;
        if (best < 0 || cpus[c].scheduling_class > cpus[best].scheduling_class ||
            (cpus[c].scheduling_class == cpus[best].scheduling_class && cpu_same_core(cpus, best, 0) && !cpu_same_core(cpus, c, 0)))
            best = c;
    }
    return best;
}

// confine keeps the others in the group; own_core keeps them off main's core; twins 0 thins a confined group to one CPU
// per core, for measuring. Main is held to its core when the core has two CPUs; on a core of one it may also use the
// others' CPUs (held to one CPU it stalled), with main_cpu as its preferred CPU. main_cpu -1: main runs with the others.
// others 0: no restriction.
static inline CpuPlacement cpu_placement(const CpuInfo *cpus, int count, int group_cpu, int confine, int own_core, int twins) {
    uint64_t all = cpu_all(count), group = cpu_group(cpus, count, group_cpu);
    int main_cpu = cpu_main_cpu(cpus, count, group);
    uint64_t main_core = own_core ? cpu_core_of(cpus, count, main_cpu) : 0, others = all;
    if (confine && cpu_count_of(group & ~main_core) >= CPU_MIN_OTHERS) others = twins ? group : cpu_one_per_core(cpus, count, group);
    others &= ~main_core;
    if (!own_core || !others) return (CpuPlacement){-1, 0, !confine || (others | main_core) == all ? 0 : others | main_core};
    return (CpuPlacement){main_cpu, cpu_count_of(main_core) > 1 ? main_core : main_core | others, others};
}

// "0-15", "0,2,4,6" or "0-3,8".
static inline const char *cpu_list(uint64_t mask, char *text, size_t size) {
    size_t used = 0;
    text[0] = 0;
    for (int c = 0; c < 64 && used < size; c++) {
        if (!(mask >> c & 1)) continue;
        int last = c;
        while (last < 63 && mask >> (last + 1) & 1) last++;
        int written = last > c ? snprintf(text + used, size - used, "%s%d-%d", used ? "," : "", c, last) : snprintf(text + used, size - used, "%s%d", used ? "," : "", c);
        used += written > 0 ? (size_t)written : 0;
        c = last;
    }
    return text;
}

// One line per cache and core type: its CPUs, cores, L3 and each core's rank, so a log shows what the rules read.
static inline void cpu_describe(const CpuInfo *cpus, int count, void (*log_line)(const char *format, ...)) {
    for (int c = 0; c < count; c++) {
        uint64_t set = 0;
        for (int o = 0; o < count; o++)
            if (cpus[o].cache_index == cpus[c].cache_index && cpus[o].efficiency_class == cpus[c].efficiency_class) set |= 1ull << o;
        if (__builtin_ctzll(set) != c) continue;
        uint64_t cores = cpu_one_per_core(cpus, count, set);
        char list[160], ranks[3 * CPU_MAX + 1] = "";
        for (int o = 0; o < count; o++)
            if (cores >> o & 1) snprintf(ranks + strlen(ranks), sizeof ranks - strlen(ranks), " %u", cpus[o].scheduling_class);
        log_line("cpuplace: cache %u, class %u: CPUs %s, %d cores, L3 %lu MB, ranks by core%s", cpus[c].cache_index, cpus[c].efficiency_class,
                 cpu_list(set, list, sizeof list), cpu_count_of(cores), (unsigned long)(cpus[c].l3_bytes >> 20), ranks);
    }
}
