#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include <winternl.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

// mingw-w64's headers lack these; its kernel32 import library has them.
WINBASEAPI BOOL WINAPI GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
WINBASEAPI BOOL WINAPI SetProcessDefaultCpuSets(HANDLE, const ULONG *, ULONG);
// Walks this process's threads only; a system-wide thread snapshot takes tens of milliseconds on a busy desktop.
NTSTATUS NTAPI NtGetNextThread(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags, PHANDLE next);

// Keeps the game's threads on one CCD, set by [cpu] ccd in s2prof.ini (off, cache = the largest L3, or a CCD number; CCD
// 0 holds CPU 0). The engine pins its threads itself, main to CPU 0 and one pool thread to each other logical CPU, so
// every thread with a narrower mask than the process's is remapped into the CCD keeping the layout (CPU c goes to the
// CCD's CPU c mod its size: a pool thread on main's SMT twin, as on a CPU with one CCD). Main, the thread that presents
// frames, gets the CCD's first CPU to itself; the pool threads beyond the CCD's size, which a CPU with one CCD would not
// have, keep off it. Every other thread is held to the CCD by the process's default CPU sets. With the engine's own
// placement on a two-CCD CPU, frames stalled 40-60 ms about 2.5 times a second, fixes on or off; on CCD 1, 68 frames over
// 15 ms fell to 2 and the frame time by 1.0-1.2 ms (army view, 2026-10-25). It follows Apply Fixes and its own switch; off
// puts the engine's own masks back. A thread rechecks every POLL_MS, so it follows the switches within a bench block's
// skipped first 200 ms, and rescans every RESCAN_MS for threads the engine pins later.
#define CONFIG_FILE L"s2prof.ini"
#define MAX_CPUS 32
#define MAX_PINNED 256
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
    DWORD_PTR engine_mask;
} PinnedThread;

typedef struct {
    ULONG id, cache;
} Cpu;

// 0 leaves placement to the engine and Windows; exported, and a switch, so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG placement_enabled = 1;

static const S2Api *api;
static Cpu cpus[MAX_CPUS];
static int cpu_count;
static int ccd_cpus[MAX_CPUS], ccd_size;
static ULONG ccd_set_ids[MAX_CPUS];
static PinnedThread pinned[MAX_PINNED];
static int pinned_count;
static BOOL applied;

static const char *read_cpus(void) {
    ULONG size;
    GetSystemCpuSetInformation(NULL, 0, &size, GetCurrentProcess(), 0);
    BYTE *sets = malloc(size);
    if (!sets || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)sets, size, &size, GetCurrentProcess(), 0)) return "GetSystemCpuSetInformation failed";
    for (BYTE *at = sets; at < sets + size; at += ((PSYSTEM_CPU_SET_INFORMATION)at)->Size) {
        PSYSTEM_CPU_SET_INFORMATION set = (PSYSTEM_CPU_SET_INFORMATION)at;
        if (set->CpuSet.Group || set->CpuSet.LogicalProcessorIndex >= MAX_CPUS) return "more than 32 CPUs or one processor group is not supported";
        cpus[set->CpuSet.LogicalProcessorIndex] = (Cpu){set->CpuSet.Id, set->CpuSet.LastLevelCacheIndex};
        if (set->CpuSet.LogicalProcessorIndex >= cpu_count) cpu_count = set->CpuSet.LogicalProcessorIndex + 1;
    }
    free(sets);
    return NULL;
}

static ULONG l3_bytes(ULONG cache) {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationCache, NULL, &length);
    BYTE *caches = malloc(length);
    ULONG bytes = 0;
    if (caches && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)caches, &length))
        for (BYTE *at = caches; at < caches + length; at += ((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)at)->Size) {
            CACHE_RELATIONSHIP *relation = &((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)at)->Cache;
            if (relation->Level == 3 && relation->GroupMask.Mask & (1ull << cache)) bytes = relation->CacheSize;
        }
    free(caches);
    return bytes;
}

// CCDs are numbered by their first logical CPU; a CCD's cache index is that CPU's index.
static const char *choose_ccd(const wchar_t *setting, ULONG *chosen) {
    ULONG ccds[MAX_CPUS] = {0}, ccd_count = 0;
    for (int cpu = 0; cpu < cpu_count; cpu++)
        if (cpus[cpu].cache == (ULONG)cpu) ccds[ccd_count++] = cpu;
    if (!wcscmp(setting, L"cache")) {
        ULONG best = 0, best_bytes = l3_bytes(ccds[0]);
        BOOL tie = FALSE;
        for (ULONG i = 1; i < ccd_count; i++) {
            ULONG bytes = l3_bytes(ccds[i]);
            if (bytes == best_bytes) tie = TRUE;
            if (bytes > best_bytes) best = i, best_bytes = bytes, tie = FALSE;
        }
        if (tie) return "ccd=cache: no CCD has a larger L3 than the others; set ccd to a number";
        *chosen = ccds[best];
        return NULL;
    }
    wchar_t *end;
    ULONG index = wcstoul(setting, &end, 10);
    if (*end || end == setting || index >= ccd_count) return "ccd must be off, cache or a CCD number that exists";
    *chosen = ccds[index];
    return NULL;
}

static PinnedThread *pinned_entry(DWORD thread) {
    for (int i = 0; i < pinned_count; i++)
        if (pinned[i].thread == thread) return &pinned[i];
    if (pinned_count == MAX_PINNED) return NULL;
    pinned[pinned_count] = (PinnedThread){thread, 0};
    return &pinned[pinned_count++];
}

// Before the first frame main is known only as the thread the engine pins to CPU 0.
static BOOL is_main(DWORD thread, DWORD_PTR engine_mask) {
    return *api->main_thread ? thread == *api->main_thread : engine_mask == 1;
}

static DWORD_PTR ccd_mask_for(DWORD thread, DWORD_PTR engine_mask) {
    DWORD_PTR mask = 0, main_cpu = (DWORD_PTR)1 << ccd_cpus[0];
    if (is_main(thread, engine_mask)) return main_cpu;
    for (int cpu = 0; cpu < cpu_count; cpu++)
        if (engine_mask & (DWORD_PTR)1 << cpu) mask |= (DWORD_PTR)1 << ccd_cpus[cpu % ccd_size];
    mask &= ~main_cpu;
    if (mask) return mask;
    for (int i = 1; i < ccd_size; i++) mask |= (DWORD_PTR)1 << ccd_cpus[i];
    return mask;
}

// Remaps the engine's pins into the CCD, or puts them back; returns how many threads it moved.
static int place_pinned_threads(BOOL into_ccd) {
    int moved = 0;
    DWORD_PTR process_mask, system_mask;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) return 0;
    HANDLE handle = NULL, next;
    while (NtGetNextThread(GetCurrentProcess(), handle, THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION, 0, 0, &next) >= 0) {
        if (handle) CloseHandle(handle);
        handle = next;
        DWORD thread_id = GetThreadId(handle);
        if (thread_id == GetCurrentThreadId()) continue;
        ThreadBasicInfo basic;
        if (NtQueryInformationThread(handle, (THREADINFOCLASS)THREAD_BASIC_INFORMATION_CLASS, &basic, sizeof basic, NULL) >= 0) {
            PinnedThread *known = NULL;
            for (int i = 0; i < pinned_count; i++)
                if (pinned[i].thread == thread_id) known = &pinned[i];
            // A mask that is neither the recorded one nor its remap belongs to a new thread with a reused id, or the
            // engine pinned the thread again: it becomes the engine's mask.
            if (known && basic.affinity != known->engine_mask && basic.affinity != ccd_mask_for(thread_id, known->engine_mask))
                known->engine_mask = basic.affinity;
            BOOL main = thread_id == *api->main_thread;
            if (!known && (main || (basic.affinity & process_mask) != process_mask) && (known = pinned_entry(thread_id))) known->engine_mask = basic.affinity;
            if (known && (main || (known->engine_mask & process_mask) != process_mask)) {
                DWORD_PTR wanted = into_ccd ? ccd_mask_for(thread_id, known->engine_mask) : known->engine_mask;
                if (basic.affinity != wanted && SetThreadAffinityMask(handle, wanted)) moved++;
            }
        }
    }
    if (handle) CloseHandle(handle);
    return moved;
}

static DWORD WINAPI follow_switch(LPVOID unused) {
    (void)unused;
    for (DWORD since_scan = RESCAN_MS;; since_scan += POLL_MS) {
        BOOL wanted = placement_enabled && *api->fix_enabled;
        if (wanted == applied && since_scan < RESCAN_MS) {
            Sleep(POLL_MS);
            continue;
        }
        since_scan = 0;
        int moved = place_pinned_threads(wanted);
        if (wanted != applied) {
            applied = wanted;
            if (!SetProcessDefaultCpuSets(GetCurrentProcess(), wanted ? ccd_set_ids : NULL, wanted ? ccd_size : 0))
                api->log_line("cpuplace: setting CPU sets failed: error %lu", GetLastError());
            api->log_line("cpuplace: %s (%d pinned threads moved)", wanted ? "game threads on the chosen CCD" : "placement back to the engine and Windows", moved);
        }
        Sleep(POLL_MS);
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    HMODULE self;
    wchar_t path[MAX_PATH], ccd_setting[16];
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)s2_module_main, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    // The module sits in fixes\ next to s2prof.ini.
    *wcsrchr(path, L'\\') = 0;
    *wcsrchr(path, L'\\') = 0;
    wcscat(path, L"\\" CONFIG_FILE);
    GetPrivateProfileStringW(L"cpu", L"ccd", L"off", ccd_setting, 16, path);
    if (!wcscmp(ccd_setting, L"off")) return "off (ccd=off in s2prof.ini; placement left to the engine and Windows)";
    const char *error = read_cpus();
    ULONG cache;
    if (!error) error = choose_ccd(ccd_setting, &cache);
    if (error) return error;
    for (int cpu = 0; cpu < cpu_count; cpu++)
        if (cpus[cpu].cache == cache) {
            ccd_set_ids[ccd_size] = cpus[cpu].id;
            ccd_cpus[ccd_size++] = cpu;
        }
    api->log_line("cpuplace: game threads go to the CCD at CPU %lu (CPUs %d-%d, L3 %lu MB)", cache, ccd_cpus[0], ccd_cpus[ccd_size - 1], l3_bytes(cache) >> 20);
    HANDLE thread = CreateThread(NULL, 0, follow_switch, NULL, 0, NULL);
    if (!thread) return "CreateThread failed";
    CloseHandle(thread);
    api->register_switch("cpuplace", &placement_enabled, NULL);
    return NULL;
}
