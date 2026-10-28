#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include "../cpupick.h"
#include <winternl.h>
#include <stdlib.h>
#include <wchar.h>

// mingw-w64's headers lack these; its kernel32 import library has them.
WINBASEAPI BOOL WINAPI GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
WINBASEAPI BOOL WINAPI SetProcessDefaultCpuSets(HANDLE, const ULONG *, ULONG);
// Walks this process's threads only; a system-wide thread snapshot takes tens of milliseconds on a busy desktop.
NTSTATUS NTAPI NtGetNextThread(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags, PHANDLE next);

// Places the game's threads by cpupick.h's rules, from what Windows reports about this PC. Two fixes:
// - cpuplace keeps the other threads in main's cache group ([cpu] ccd in s2prof.ini: auto, off, or a group number) when
//   it leaves them enough CPUs. With the engine's own placement on a two-CCD CPU, frames stalled 40-60 ms about 2.5 times
//   a second; on one CCD, 68 frames over 15 ms fell to 2 and the frame time by 1.0-1.2 ms (army view, 2026-10-25). Its
//   twins switch (off: one logical CPU per core) is there to measure SMT sharing.
// - maincore gives main, the thread that presents frames, the group's highest-ranked core to itself and makes that CPU
//   its ideal processor. Main sharing its core cost 0.66 ms a frame in a CPU-bound battle (2026-10-25).
// CPU sets place every thread: the process's default for the others, main's own for main. The engine pins main to CPU 0
// and a pool thread to each other CPU with hard masks, which CPU sets can only narrow, so while placing a pin that leaves
// out a CPU its thread may use is widened to the process's mask (a pinned thread that wakes waits for its one CPU while
// others sit idle). Both follow Apply Fixes and their switches; off puts back the engine's masks and main's CPU set and
// ideal processor. A thread rechecks every POLL_MS, so it follows the switches within a bench block's skipped first
// 200 ms, and rescans every RESCAN_MS for threads the engine pins later.
#define CONFIG_FILE L"s2prof.ini"
#define MAX_CPUS 32
#define MAX_TRACKED 256
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

// What this module changed on a thread, to put back.
typedef struct {
    DWORD thread;
    ULONGLONG created;
    DWORD_PTR engine_mask, set_mask;
    DWORD ideal_before;
    BOOL selected, ideal_set, seen;
} TrackedThread;

// 0 leaves the threads where the engine and Windows put them; exported so each can be A/B tested live.
__declspec(dllexport) volatile LONG placement_enabled = 1;
__declspec(dllexport) volatile LONG main_core_enabled = 1;
__declspec(dllexport) volatile LONG twins_enabled = 1;

static const S2Api *api;
static CpuInfo cpus[MAX_CPUS];
static ULONG set_ids[MAX_CPUS];
static int cpu_count, group_cpu;
static BOOL confine, own_core;
static TrackedThread tracked[MAX_TRACKED];
static int tracked_count;

static const char *read_cpus(void) {
    ULONG size;
    const char *error = NULL;
    GetSystemCpuSetInformation(NULL, 0, &size, GetCurrentProcess(), 0);
    BYTE *sets = malloc(size);
    if (!sets || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)sets, size, &size, GetCurrentProcess(), 0)) error = "GetSystemCpuSetInformation failed";
    for (BYTE *at = sets; !error && at < sets + size; at += ((PSYSTEM_CPU_SET_INFORMATION)at)->Size) {
        PSYSTEM_CPU_SET_INFORMATION set = (PSYSTEM_CPU_SET_INFORMATION)at;
        int cpu = set->CpuSet.LogicalProcessorIndex;
        if (set->CpuSet.Group || cpu >= MAX_CPUS) {
            error = "more than 32 CPUs or one processor group is not supported";
            break;
        }
        set_ids[cpu] = set->CpuSet.Id;
        cpus[cpu] = (CpuInfo){set->CpuSet.CoreIndex, set->CpuSet.LastLevelCacheIndex, set->CpuSet.EfficiencyClass, set->CpuSet.SchedulingClass, 0};
        if (cpu >= cpu_count) cpu_count = cpu + 1;
    }
    free(sets);
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationCache, NULL, &length);
    BYTE *caches = malloc(length);
    if (!error && caches && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)caches, &length))
        for (BYTE *at = caches; at < caches + length; at += ((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)at)->Size) {
            CACHE_RELATIONSHIP *relation = &((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)at)->Cache;
            for (int cpu = 0; cpu < cpu_count; cpu++)
                if (relation->Level == 3 && !relation->GroupMask.Group && relation->GroupMask.Mask >> cpu & 1) cpus[cpu].l3_bytes = relation->CacheSize;
        }
    free(caches);
    return error;
}

static ULONG ids_of(uint64_t mask, ULONG *ids) {
    ULONG count = 0;
    for (int cpu = 0; cpu < cpu_count; cpu++)
        if (mask >> cpu & 1) ids[count++] = set_ids[cpu];
    return count;
}

static BOOL set_defaults(uint64_t others) {
    ULONG ids[MAX_CPUS], count = ids_of(others, ids);
    return SetProcessDefaultCpuSets(GetCurrentProcess(), count ? ids : NULL, count);
}

static TrackedThread *find_tracked(DWORD thread, ULONGLONG created) {
    for (int i = 0; i < tracked_count; i++) {
        if (tracked[i].thread != thread) continue;
        if (tracked[i].created == created) return &tracked[i];
        // A new thread with an exited one's id: what was changed went with the old one.
        tracked[i] = tracked[--tracked_count];
        return NULL;
    }
    return NULL;
}

// Gives main its CPU set and ideal processor and widens the engine's pins while placing, or puts all of it back; returns
// how many masks it changed.
static int place_threads(CpuPlacement placement, DWORD main_thread) {
    int changed = 0;
    DWORD_PTR process_mask, system_mask;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) return 0;
    BOOL placing = placement.main_cpu >= 0 || placement.others;
    DWORD_PTR others = placement.others ? (DWORD_PTR)placement.others : process_mask;
    ULONG main_ids[MAX_CPUS], main_count = ids_of(placement.main_cpus, main_ids);
    for (int i = 0; i < tracked_count; i++) tracked[i].seen = FALSE;
    HANDLE handle = NULL, next;
    while (NtGetNextThread(GetCurrentProcess(), handle, THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION, 0, 0,
                           &next) >= 0) {
        if (handle) CloseHandle(handle);
        handle = next;
        DWORD thread = GetThreadId(handle);
        ThreadBasicInfo basic;
        FILETIME created, exited, kernel, user;
        if (thread == GetCurrentThreadId() || NtQueryInformationThread(handle, (THREADINFOCLASS)THREAD_BASIC_INFORMATION_CLASS, &basic, sizeof basic, NULL) < 0 ||
            !GetThreadTimes(handle, &created, &exited, &kernel, &user))
            continue;
        ULONGLONG created_at = (ULONGLONG)created.dwHighDateTime << 32 | created.dwLowDateTime;
        TrackedThread *t = find_tracked(thread, created_at);
        // Before the first frame main is known only as the thread the engine pins to CPU 0.
        BOOL main = main_thread ? thread == main_thread : (t ? t->engine_mask : basic.affinity) == 1;
        BOOL main_placed = main && placement.main_cpu >= 0;
        if (!t && ((basic.affinity & process_mask) != process_mask || main_placed) && tracked_count < MAX_TRACKED) {
            t = &tracked[tracked_count++];
            *t = (TrackedThread){.thread = thread, .created = created_at};
        }
        if (!t) continue;
        t->seen = TRUE;
        // A mask other than the one set here is the engine's, first seen or changed since.
        if (basic.affinity != t->set_mask) t->engine_mask = basic.affinity, t->set_mask = 0;
        DWORD_PTR allowed = main_placed ? (DWORD_PTR)placement.main_cpus : others;
        DWORD_PTR wanted = placing && (allowed & ~t->engine_mask) ? process_mask : t->engine_mask;
        if (basic.affinity != wanted && SetThreadAffinityMask(handle, wanted)) {
            changed++;
            t->set_mask = wanted == t->engine_mask ? 0 : wanted;
        }
        if (main_placed ? SetThreadSelectedCpuSets(handle, main_ids, main_count) : t->selected && SetThreadSelectedCpuSets(handle, NULL, 0))
            t->selected = main_placed;
        if (main_placed) {
            DWORD before = SetThreadIdealProcessor(handle, (DWORD)placement.main_cpu);
            if (!t->ideal_set && before != (DWORD)-1) t->ideal_before = before, t->ideal_set = TRUE;
        } else if (t->ideal_set && SetThreadIdealProcessor(handle, t->ideal_before) != (DWORD)-1) {
            t->ideal_set = FALSE;
        }
    }
    if (handle) CloseHandle(handle);
    // Exited threads, and threads with nothing to put back, are dropped.
    int kept = 0;
    for (int i = 0; i < tracked_count; i++) {
        TrackedThread *t = &tracked[i];
        if (t->seen && (t->engine_mask != process_mask || t->set_mask || t->selected || t->ideal_set)) tracked[kept++] = *t;
    }
    tracked_count = kept;
    return changed;
}

static void log_placement(CpuPlacement placement, BOOL confined, int moved) {
    char main_text[96] = "with them", list[160];
    if (placement.main_cpu < 0 && !placement.others) {
        api->log_line("cpuplace: placement back to the engine and Windows (%d thread masks restored)", moved);
        return;
    }
    uint64_t main_core = placement.main_cpu >= 0 ? cpu_core_of(cpus, cpu_count, placement.main_cpu) : 0;
    if (placement.main_cpu >= 0 && placement.main_cpus == main_core)
        snprintf(main_text, sizeof main_text, "alone on CPUs %s", cpu_list(main_core, list, sizeof list));
    else if (placement.main_cpu >= 0)
        snprintf(main_text, sizeof main_text, "on CPU %d first, which the others keep off", placement.main_cpu);
    char wide_text[64] = "";
    if (confined && (placement.others & ~cpu_group(cpus, cpu_count, group_cpu)))
        snprintf(wide_text, sizeof wide_text, " (every CPU: the cache group leaves them fewer than %d)", CPU_MIN_OTHERS);
    api->log_line("cpuplace: main %s; the other threads on CPUs %s%s (%d thread masks widened)", main_text,
                  placement.others ? cpu_list(placement.others, list, sizeof list) : "all", wide_text, moved);
}

static DWORD WINAPI follow_switches(LPVOID unused) {
    (void)unused;
    CpuPlacement applied = {-1, 0, 0};
    DWORD placed_main = 0;
    BOOL defaults_set = TRUE;
    for (DWORD since_scan = RESCAN_MS;; since_scan += POLL_MS, Sleep(POLL_MS)) {
        BOOL fixes = *api->fix_enabled, confined = confine && placement_enabled && fixes;
        CpuPlacement wanted = cpu_placement(cpus, cpu_count, group_cpu, confined, own_core && main_core_enabled && fixes, twins_enabled);
        BOOL changed = wanted.main_cpu != applied.main_cpu || wanted.main_cpus != applied.main_cpus || wanted.others != applied.others;
        // Main is known from the first frame and gets its CPU set at once.
        DWORD main_thread = *api->main_thread;
        if (!changed && main_thread == placed_main && since_scan < RESCAN_MS) continue;
        since_scan = 0;
        if (changed || !defaults_set) {
            defaults_set = set_defaults(wanted.others);
            if (!defaults_set && changed) api->log_line("cpuplace: setting CPU sets failed: error %lu; retried every %d s", GetLastError(), RESCAN_MS / 1000);
        }
        int moved = place_threads(wanted, main_thread);
        placed_main = main_thread;
        if (changed) {
            log_placement(wanted, confined, moved);
            applied = wanted;
        }
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    HMODULE self;
    wchar_t path[MAX_PATH], setting[16];
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)s2_module_main, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    // The module sits in fixes\ next to s2prof.ini.
    *wcsrchr(path, L'\\') = 0;
    *wcsrchr(path, L'\\') = 0;
    wcscat(path, L"\\" CONFIG_FILE);
    GetPrivateProfileStringW(L"cpu", L"ccd", L"auto", setting, 16, path);
    confine = wcscmp(setting, L"off") != 0;
    // cache is the setting's name before the rules (the largest L3), which they still apply first.
    long number = -1;
    const char *error = read_cpus(), *why = NULL;
    if (!error && confine && wcscmp(setting, L"auto") && wcscmp(setting, L"cache")) {
        wchar_t *end;
        number = wcstol(setting, &end, 10);
        if (*end || end == setting || number < 0 || number >= CPU_MAX) error = "ccd must be auto, off or a cache group number (s2prof.ini)";
    }
    if (!error && (group_cpu = cpu_pick_group(cpus, cpu_count, (int)number, &why)) < 0) error = "ccd names a cache group this CPU does not have (s2prof.ini)";
    if (!error) {
        char list[160];
        cpu_describe(cpus, cpu_count, api->log_line);
        api->log_line("cpuplace: the cache group at CPU %d (%s): CPUs %s", group_cpu, why, cpu_list(cpu_group(cpus, cpu_count, group_cpu), list, sizeof list));
        own_core = cpu_placement(cpus, cpu_count, group_cpu, confine, TRUE, TRUE).main_cpu >= 0;
        HANDLE thread = CreateThread(NULL, 0, follow_switches, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
        else error = "CreateThread failed";
    }
    if (!error && confine) {
        api->register_switch("cpuplace", &placement_enabled, NULL);
        api->register_switch("twins", &twins_enabled, NULL);
    }
    api->end_fix(error ? error : confine ? NULL : "off (ccd=off in s2prof.ini; the other threads are left to the engine and Windows)");
    api->begin_fix("maincore");
    if (!error && own_core) api->register_switch("maincore", &main_core_enabled, NULL);
    return error ? error : own_core ? NULL : "off (one core: none left for the other threads)";
}
