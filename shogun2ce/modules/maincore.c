#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include <winternl.h>
#include <stdint.h>
#include <stdlib.h>

// A physical core for main alone. The engine leaves main unpinned: it moves over every CPU the process may use (the
// cpuwhere probe), and pool and TBB threads run on the twin of whichever core it is on, so main shares its core whenever
// they work. Main gets the CPU set of one core, the last of the fastest efficiency class in the process's default CPU
// sets (cpuplace's CCD, or all CPUs), and the defaults leave that core out; threads the engine pins inside it move onto
// the other CPUs that share its L3. In the original game, freeing main's core gave -0.66 ms a frame in a CPU-bound
// battle. Off, or with Apply Fixes off, all of it goes back. The defaults are checked every POLL_MS, so a change of
// cpuplace's is followed at once; every RESCAN_MS, threads the engine pins later are moved.
WINBASEAPI BOOL WINAPI GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
WINBASEAPI BOOL WINAPI SetProcessDefaultCpuSets(HANDLE, const ULONG *, ULONG);
WINBASEAPI BOOL WINAPI GetProcessDefaultCpuSets(HANDLE, PULONG, ULONG, PULONG);
NTSTATUS NTAPI NtGetNextThread(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags, PHANDLE next);
#define MAX_CPUS 64
#define MAX_MOVED 64
#define POLL_MS 20
#define RESCAN_MS 2000
#define THREAD_BASIC_INFORMATION_CLASS 0

typedef struct {
    LONG exit_status;
    PVOID teb;
    HANDLE process_id, thread_id;
    ULONG_PTR affinity;
    LONG priority, base_priority;
} ThreadBasicInfo;

typedef struct {
    DWORD thread;
    DWORD_PTR original, placed;
} MovedThread;

typedef struct {
    ULONG id, cache, core;
    BYTE efficiency;
} Cpu;

// 0 leaves main unpinned and its core shared, as the engine has it; exported, and a switch.
__declspec(dllexport) volatile LONG main_core_enabled = 1;

static const S2Api *api;
static Cpu cpus[MAX_CPUS];
static int cpu_count;
static uint64_t core_mask;
static int rest[MAX_CPUS], rest_size, next_rest;
// The default CPU sets this module found (count 0: none, all CPUs), and the ones it set.
static ULONG previous_ids[MAX_CPUS], previous_count, own_ids[MAX_CPUS], own_count;
static MovedThread moved[MAX_MOVED];
static int moved_count;
static HANDLE main_handle;

static const char *read_cpus(void) {
    ULONG size;
    GetSystemCpuSetInformation(NULL, 0, &size, GetCurrentProcess(), 0);
    BYTE *sets = malloc(size);
    if (!sets || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)sets, size, &size, GetCurrentProcess(), 0)) return "GetSystemCpuSetInformation failed";
    for (BYTE *at = sets; at < sets + size; at += ((PSYSTEM_CPU_SET_INFORMATION)at)->Size) {
        PSYSTEM_CPU_SET_INFORMATION set = (PSYSTEM_CPU_SET_INFORMATION)at;
        int cpu = set->CpuSet.LogicalProcessorIndex;
        if (set->CpuSet.Group || cpu >= MAX_CPUS) return "more than one processor group is not supported";
        cpus[cpu] = (Cpu){set->CpuSet.Id, set->CpuSet.LastLevelCacheIndex, set->CpuSet.CoreIndex, set->CpuSet.EfficiencyClass};
        if (cpu >= cpu_count) cpu_count = cpu + 1;
    }
    free(sets);
    return NULL;
}

static int cpu_of(ULONG id) {
    for (int c = 0; c < cpu_count; c++)
        if (cpus[c].id == id) return c;
    return -1;
}

static BOOL same_ids(const ULONG *a, ULONG a_count, const ULONG *b, ULONG b_count) {
    return a_count == b_count && !memcmp(a, b, a_count * sizeof *a);
}

// The CPUs the defaults allow, as a mask (none set: all CPUs).
static uint64_t allowed_mask(const ULONG *ids, ULONG count) {
    uint64_t mask = 0;
    for (ULONG i = 0; i < count; i++) {
        int cpu = cpu_of(ids[i]);
        if (cpu >= 0) mask |= 1ull << cpu;
    }
    return count ? mask : cpu_count == 64 ? ~0ull : (1ull << cpu_count) - 1;
}

// The last core of the fastest efficiency class among the allowed CPUs; its CPUs and the other CPUs sharing its L3.
static BOOL choose_core(uint64_t allowed) {
    int chosen = -1;
    for (int c = 0; c < cpu_count; c++)
        if (allowed >> c & 1 && (chosen < 0 || cpus[c].efficiency >= cpus[chosen].efficiency)) chosen = c;
    if (chosen < 0) return FALSE;
    core_mask = 0;
    rest_size = 0;
    for (int c = 0; c < cpu_count; c++) {
        if (cpus[c].core == cpus[chosen].core) core_mask |= 1ull << c;
        else if (cpus[c].cache == cpus[chosen].cache && allowed >> c & 1) rest[rest_size++] = c;
    }
    return rest_size > 0;
}

static BOOL pin_main(BOOL pin) {
    ULONG ids[MAX_CPUS] = {0}, count = 0;
    for (int c = 0; pin && c < cpu_count; c++)
        if (core_mask >> c & 1) ids[count++] = cpus[c].id;
    if (SetThreadSelectedCpuSets(main_handle, count ? ids : NULL, count)) return TRUE;
    api->log_line("maincore: %s main failed: error %lu", pin ? "pinning" : "unpinning", GetLastError());
    return FALSE;
}

static void set_defaults(const ULONG *ids, ULONG count) {
    if (!SetProcessDefaultCpuSets(GetCurrentProcess(), count ? ids : NULL, count)) api->log_line("maincore: setting CPU sets failed: error %lu", GetLastError());
}

static MovedThread *find_moved(DWORD thread) {
    for (int i = 0; i < moved_count; i++)
        if (moved[i].thread == thread) return &moved[i];
    return NULL;
}

static int move_threads(BOOL away) {
    int changed = 0;
    HANDLE handle = NULL, next;
    while (NtGetNextThread(GetCurrentProcess(), handle, THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION, 0, 0, &next) >= 0) {
        if (handle) CloseHandle(handle);
        handle = next;
        DWORD id = GetThreadId(handle);
        if (id == *api->main_thread) continue;
        ThreadBasicInfo basic;
        if (NtQueryInformationThread(handle, (THREADINFOCLASS)THREAD_BASIC_INFORMATION_CLASS, &basic, sizeof basic, NULL) < 0) continue;
        MovedThread *known = find_moved(id);
        if (!away) {
            if (known && basic.affinity == known->placed && SetThreadAffinityMask(handle, known->original)) changed++;
            continue;
        }
        if ((known && basic.affinity == known->placed) || !basic.affinity || (basic.affinity & ~core_mask)) continue;
        if (!known && moved_count < MAX_MOVED) known = &moved[moved_count++];
        if (!known) continue;
        DWORD_PTR placed = (DWORD_PTR)1 << rest[next_rest++ % rest_size];
        *known = (MovedThread){id, basic.affinity, placed};
        if (SetThreadAffinityMask(handle, placed)) changed++;
    }
    if (handle) CloseHandle(handle);
    if (!away) moved_count = 0;
    return changed;
}

static void release(void) {
    move_threads(FALSE);
    pin_main(FALSE);
    ULONG ids[MAX_CPUS], count = 0;
    // Someone else's defaults since: theirs stay.
    if (GetProcessDefaultCpuSets(GetCurrentProcess(), ids, MAX_CPUS, &count) && same_ids(ids, count, own_ids, own_count)) set_defaults(previous_ids, previous_count);
    own_count = 0;
}

// Main's core from the current defaults, which become the ones to restore, and the defaults without that core.
static BOOL claim(const ULONG *ids, ULONG count) {
    if (!choose_core(allowed_mask(ids, count))) return FALSE;
    memcpy(previous_ids, ids, count * sizeof *ids);
    previous_count = count;
    own_count = 0;
    uint64_t allowed = allowed_mask(ids, count) & ~core_mask;
    for (int c = 0; c < cpu_count; c++)
        if (allowed >> c & 1) own_ids[own_count++] = cpus[c].id;
    if (!pin_main(TRUE)) return FALSE;
    set_defaults(own_ids, own_count);
    return TRUE;
}

static DWORD WINAPI follow_switch(LPVOID unused) {
    (void)unused;
    BOOL applied = FALSE;
    for (DWORD since_scan = 0;; since_scan += POLL_MS, Sleep(POLL_MS)) {
        BOOL wanted = main_core_enabled && *api->fix_enabled && *api->main_thread;
        if (wanted && !main_handle) main_handle = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, *api->main_thread);
        if (wanted && !main_handle) continue;
        ULONG ids[MAX_CPUS], count = 0;
        if (!GetProcessDefaultCpuSets(GetCurrentProcess(), ids, MAX_CPUS, &count)) count = 0;
        BOOL defaults_ours = applied && same_ids(ids, count, own_ids, own_count);
        if (wanted == applied && (!applied || defaults_ours)) {
            if (applied && since_scan >= RESCAN_MS) {
                move_threads(TRUE);
                since_scan = 0;
            }
            continue;
        }
        since_scan = 0;
        // The defaults changed under us (cpuplace switched): everything goes back, then is placed in the new ones.
        if (applied) release();
        applied = FALSE;
        int changed = 0;
        if (wanted && GetProcessDefaultCpuSets(GetCurrentProcess(), ids, MAX_CPUS, &count) && claim(ids, count)) {
            applied = TRUE;
            changed = move_threads(TRUE);
        }
        api->log_line("maincore: %s (%d threads moved; main's core 0x%llx, %d other CPUs share its L3)",
                      applied ? "main's core to main alone" : wanted ? "no core could be claimed" : "main unpinned, its core shared again", changed,
                      (unsigned long long)core_mask, rest_size);
        if (wanted && !applied) main_core_enabled = 0;
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    const char *error = read_cpus();
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, follow_switch, NULL, 0, NULL);
    if (!thread) return "CreateThread failed";
    CloseHandle(thread);
    api->register_switch("maincore", &main_core_enabled, NULL);
    return NULL;
}
