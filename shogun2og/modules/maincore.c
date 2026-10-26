#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include <winternl.h>
#include <stdint.h>
#include <stdlib.h>

// Main's physical core to main alone. The engine pins main to one CPU and a pool thread to every other logical CPU, so on
// a CPU with SMT one pool thread sits on main's twin (cpuplace keeps that layout inside its CCD), and unpinned threads may
// run there too; whenever they work, main shares its core. Every thread pinned only inside main's core moves onto the
// other CPUs that share main's L3, and the process's default CPU sets (cpuplace's, or all CPUs) leave main's core out.
// In a CPU-bound battle (2026-10-25): -0.66 and -0.67 ms a frame, 164 -> 184 fps, 5 of 5 pairs each. Off, or with Apply
// Fixes off, both go back. Main's CPU is followed every POLL_MS; every RESCAN_MS, threads the engine pins later are
// moved and cpuplace's CPU sets are checked again.
WINBASEAPI BOOL WINAPI GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
WINBASEAPI BOOL WINAPI SetProcessDefaultCpuSets(HANDLE, const ULONG *, ULONG);
WINBASEAPI BOOL WINAPI GetProcessDefaultCpuSets(HANDLE, PULONG, ULONG, PULONG);
NTSTATUS NTAPI NtGetNextThread(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags, PHANDLE next);
#define MAX_CPUS 32
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

// 0 leaves main's core shared, as the engine has it; exported, and a switch.
__declspec(dllexport) volatile LONG main_core_enabled = 1;

static const S2Api *api;
static ULONG set_ids[MAX_CPUS], cache_of[MAX_CPUS], core_of[MAX_CPUS];
static int cpu_count;
static DWORD_PTR core_mask;
static int rest[MAX_CPUS], rest_size, next_rest;
// The default CPU sets before this module's (count 0: none, all CPUs), and this module's.
static ULONG previous_ids[MAX_CPUS], previous_count, own_ids[MAX_CPUS], own_count;
static MovedThread moved[MAX_MOVED];
static int moved_count;

static const char *read_cpus(void) {
    ULONG size;
    GetSystemCpuSetInformation(NULL, 0, &size, GetCurrentProcess(), 0);
    BYTE *sets = malloc(size);
    if (!sets || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)sets, size, &size, GetCurrentProcess(), 0)) return "GetSystemCpuSetInformation failed";
    for (BYTE *at = sets; at < sets + size; at += ((PSYSTEM_CPU_SET_INFORMATION)at)->Size) {
        PSYSTEM_CPU_SET_INFORMATION set = (PSYSTEM_CPU_SET_INFORMATION)at;
        int cpu = set->CpuSet.LogicalProcessorIndex;
        if (set->CpuSet.Group || cpu >= MAX_CPUS) return "more than 32 CPUs or one processor group is not supported";
        set_ids[cpu] = set->CpuSet.Id;
        cache_of[cpu] = set->CpuSet.LastLevelCacheIndex;
        core_of[cpu] = set->CpuSet.CoreIndex;
        if (cpu >= cpu_count) cpu_count = cpu + 1;
    }
    free(sets);
    return NULL;
}

// Main's CPU from its affinity: the engine and cpuplace both give it exactly one.
static int main_cpu(void) {
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, *api->main_thread);
    if (!thread) return -1;
    ThreadBasicInfo basic;
    LONG status = NtQueryInformationThread(thread, (THREADINFOCLASS)THREAD_BASIC_INFORMATION_CLASS, &basic, sizeof basic, NULL);
    CloseHandle(thread);
    if (status < 0 || !basic.affinity || basic.affinity & (basic.affinity - 1)) return -1;
    return __builtin_ctz((unsigned)basic.affinity);
}

static void find_core(int cpu) {
    core_mask = 0;
    rest_size = 0;
    for (int c = 0; c < cpu_count; c++) {
        if (core_of[c] == core_of[cpu] && cache_of[c] == cache_of[cpu]) core_mask |= (DWORD_PTR)1 << c;
        else if (cache_of[c] == cache_of[cpu]) rest[rest_size++] = c;
    }
}

static BOOL in_core(ULONG id) {
    for (int c = 0; c < cpu_count; c++)
        if (set_ids[c] == id) return (core_mask >> c & 1) != 0;
    return FALSE;
}

// Takes the current default CPU sets as the ones to restore unless they are this module's, and sets them minus main's core.
static void exclude_core_from_defaults(void) {
    ULONG ids[MAX_CPUS], count = 0;
    if (!GetProcessDefaultCpuSets(GetCurrentProcess(), ids, MAX_CPUS, &count)) count = 0;
    BOOL own = count == own_count && own_count;
    for (ULONG i = 0; own && i < count; i++) own = ids[i] == own_ids[i];
    if (own) return;
    memcpy(previous_ids, ids, count * sizeof *ids);
    previous_count = count;
    own_count = 0;
    if (count) {
        for (ULONG i = 0; i < count; i++)
            if (!in_core(ids[i])) own_ids[own_count++] = ids[i];
    } else {
        for (int c = 0; c < cpu_count; c++)
            if (!(core_mask >> c & 1)) own_ids[own_count++] = set_ids[c];
    }
    if (!SetProcessDefaultCpuSets(GetCurrentProcess(), own_ids, own_count)) api->log_line("maincore: setting CPU sets failed: error %lu", GetLastError());
}

static void restore_defaults(void) {
    ULONG ids[MAX_CPUS], count = 0;
    if (!GetProcessDefaultCpuSets(GetCurrentProcess(), ids, MAX_CPUS, &count)) count = 0;
    BOOL own = count == own_count;
    for (ULONG i = 0; own && i < count; i++) own = ids[i] == own_ids[i];
    // Someone else's since: theirs stays.
    if (own && !SetProcessDefaultCpuSets(GetCurrentProcess(), previous_count ? previous_ids : NULL, previous_count))
        api->log_line("maincore: restoring CPU sets failed: error %lu", GetLastError());
    own_count = 0;
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
        if ((known && basic.affinity == known->placed) || !basic.affinity || (basic.affinity & ~core_mask) || !rest_size) continue;
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

static DWORD WINAPI follow_switch(LPVOID unused) {
    (void)unused;
    BOOL applied = FALSE;
    int core_cpu = -1;
    for (DWORD since_scan = 0;; since_scan += POLL_MS, Sleep(POLL_MS)) {
        BOOL wanted = main_core_enabled && *api->fix_enabled && *api->main_thread;
        // Main's CPU is checked every poll, so a placement change (cpuplace's switch) is followed at once.
        int cpu = wanted ? main_cpu() : core_cpu;
        if (wanted == applied && (!applied || (since_scan < RESCAN_MS && cpu == core_cpu))) continue;
        since_scan = 0;
        if (wanted && cpu < 0) continue;
        // Main moved (cpuplace switched): everything goes back first, then is placed around its new core.
        if (applied && wanted && cpu != core_cpu) {
            move_threads(FALSE);
            restore_defaults();
            applied = FALSE;
        }
        if (wanted && !applied) find_core(core_cpu = cpu);
        int changed = move_threads(wanted);
        if (wanted) exclude_core_from_defaults();
        else restore_defaults();
        if (wanted != applied) {
            applied = wanted;
            api->log_line("maincore: %s (%d threads moved; main's core 0x%lx, %d other CPUs share its L3)", wanted ? "main's core to main alone" : "main's core shared again",
                          changed, (unsigned long)core_mask, rest_size);
        }
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
