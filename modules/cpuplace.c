#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

// mingw-w64's headers lack these; its kernel32 import library has them.
WINBASEAPI BOOL WINAPI GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
WINBASEAPI BOOL WINAPI SetProcessDefaultCpuSets(HANDLE, const ULONG *, ULONG);

// Keeps the game's threads on one CCD, set by [cpu] in s2prof.ini. Windows may run the main thread on a dual-CCD chip's
// higher-clocked CCD, while the engine's cache-missing render code runs far faster on a 3D V-Cache CCD. Checked every
// frame from the frame function so it follows Apply Fixes.
#define FRAME_RVA 0x1a84970
#define CONFIG_FILE L"s2prof.ini"
#define MAX_CPUS 64

typedef uint64_t (*FrameFn)(void *, void *);

// 0 leaves placement to Windows; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG placement_enabled = 1;

static const BYTE frame_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x81, 0xec, 0xe0, 0x03, 0x00, 0x00};
static const S2Api *api;
static FrameFn original_frame;
static ULONG ccd_ids[MAX_CPUS], ccd_count;
static BOOL applied;

typedef struct {
    ULONG id, cache;
} Cpu;

static Cpu cpus[MAX_CPUS];
static int cpu_count;

static const char *read_cpus(void) {
    ULONG size;
    GetSystemCpuSetInformation(NULL, 0, &size, GetCurrentProcess(), 0);
    BYTE *sets = malloc(size);
    if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)sets, size, &size, GetCurrentProcess(), 0)) return "GetSystemCpuSetInformation failed";
    for (BYTE *at = sets; at < sets + size; at += ((PSYSTEM_CPU_SET_INFORMATION)at)->Size) {
        PSYSTEM_CPU_SET_INFORMATION set = (PSYSTEM_CPU_SET_INFORMATION)at;
        if (set->CpuSet.Group || set->CpuSet.LogicalProcessorIndex >= MAX_CPUS) return "more than one processor group is not supported";
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
    if (GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)caches, &length))
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

static void collect_ccd(ULONG cache) {
    for (int cpu = 0; cpu < cpu_count; cpu++)
        if (cpus[cpu].cache == cache) ccd_ids[ccd_count++] = cpus[cpu].id;
}

static uint64_t frame_hook(void *engine_state, void *frame) {
    BOOL wanted = *api->fix_enabled && placement_enabled;
    if (wanted != applied) {
        applied = wanted;
        if (!SetProcessDefaultCpuSets(GetCurrentProcess(), wanted ? ccd_ids : NULL, wanted ? ccd_count : 0))
            api->log_line("cpuplace: setting CPU sets failed: error %lu", GetLastError());
    }
    return original_frame(engine_state, frame);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *s2_api) {
    api = s2_api;
    HMODULE self;
    wchar_t path[MAX_PATH], ccd_setting[16];
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)s2_module_main, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    // The module sits in fixes\ next to s2prof.ini.
    *wcsrchr(path, L'\\') = 0;
    *wcsrchr(path, L'\\') = 0;
    wcscat(path, L"\\" CONFIG_FILE);
    GetPrivateProfileStringW(L"cpu", L"ccd", L"off", ccd_setting, 16, path);
    if (!wcscmp(ccd_setting, L"off")) return "ccd=off in s2prof.ini; CPU placement left to Windows";
    const char *error = read_cpus();
    ULONG cache;
    if (!error) error = choose_ccd(ccd_setting, &cache);
    if (error) return error;
    collect_ccd(cache);
    api->log_line("cpuplace: game threads on the CCD at cpu %lu (%lu cpus, L3 %lu MB)", cache, ccd_count, l3_bytes(cache) >> 20);
    return api->hook(api->engine + FRAME_RVA, frame_prologue, sizeof frame_prologue, (const void *)frame_hook, (void **)&original_frame);
}
