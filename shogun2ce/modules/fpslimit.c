#include "../s2module.h"
#include "../checks/fpslimit.h"
#include <stdint.h>
#include <wchar.h>

// A frame rate cap, [fps] fps_limit (frames a second, whole numbers; 0, the default, is none): after the main tick's
// present, the main thread waits for the end of the frame's slot. A frame more than a slot late starts the slots again
// rather than letting the next ones hurry. The loading screen presents from its own redraw, so it isn't held, and the
// debug report measures the game uncapped.
// The main tick's one call of the present: call [rax+0x5a0], the engine object's vtable, the object in rcx.
#define PRESENT_CALL_RVA 0x162d4b
#define PRESENT_SLOT 0x5a0
// The timer wakes this early and the rest is spun: a timer can be late by up to its resolution.
#define SPIN_US_HIGH_RESOLUTION 500
#define SPIN_US 2000
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
#define NEAR_BYTES 0x10000

typedef uint64_t (*PresentFn)(BYTE *object);
typedef UINT(WINAPI *TimePeriodFn)(UINT period);

static const BYTE present_call[] = {0xff, 0x90, 0xa0, 0x05, 0x00, 0x00};

// The cap in frames a second; can be patched live.
__declspec(dllexport) volatile LONG fps_limit;

static const S2Api *api;
static HANDLE timer;
static LONGLONG frequency, spin_ticks, deadline;
static BOOL lifted;

static LONGLONG now_ticks(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

static void wait_until(LONGLONG target) {
    LONGLONG left = target - now_ticks();
    if (left > spin_ticks) {
        // Relative, in 100 ns units.
        LARGE_INTEGER due = {.QuadPart = -((left - spin_ticks) * 10000000 / frequency)};
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    }
    while (now_ticks() < target) YieldProcessor();
}

static uint64_t present_hook(BYTE *object) {
    uint64_t result = (*(PresentFn *)(*(BYTE **)object + PRESENT_SLOT))(object);
    LONG limit = fps_limit;
    BOOL report = api->report_running();
    if (report && !lifted) api->log_line("fpslimit: no cap while the debug report runs");
    lifted = report;
    if (limit <= 0 || report) return result;
    LONGLONG period = frequency / limit, now = now_ticks();
    deadline += period;
    if (now - deadline > period) deadline = now;
    else wait_until(deadline);
    return result;
}

// Whole numbers only: 7.5 or -1 mean no cap rather than 7 or off. FALSE when the ini's value was refused.
static BOOL read_limit(void) {
    fps_limit = 0;
    wchar_t path[MAX_PATH], text[16] = L"";
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)read_limit, &self);
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return TRUE;
    for (int up = 0; up < 2; up++) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) *slash = 0;
    }
    wcscat(path, L"\\s2prof.ini");
    GetPrivateProfileStringW(L"fps", L"fps_limit", L"", text, sizeof text / sizeof *text, path);
    if (!text[0]) return TRUE;
    wchar_t *end;
    unsigned long value = wcstoul(text, &end, 10);
    if (text[0] < L'0' || text[0] > L'9' || *end || value > 0x7fffffff) return FALSE;
    fps_limit = (LONG)value;
    return TRUE;
}

// Before Windows 10 1803: a plain timer, at the timer period, held at 1 ms for the session so it wakes within the spin.
static const char *plain_timer(void) {
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    TimePeriodFn begin_period = winmm ? (TimePeriodFn)(void *)GetProcAddress(winmm, "timeBeginPeriod") : NULL;
    if (!begin_period) return "winmm.dll lacks timeBeginPeriod";
    begin_period(1);
    timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    spin_ticks = frequency * SPIN_US / 1000000;
    return timer ? NULL : "no waitable timer";
}

// Above the engine's image, within reach of the call's 32-bit offset.
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

// The present's call becomes a rel32 call of a jump placed near the engine, which reaches ours.
static const char *patch_present_call(void) {
    BYTE *jump = allocate_near();
    if (!jump) return "no memory free within reach of the engine";
    static const BYTE indirect_jump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(jump, indirect_jump, sizeof indirect_jump);
    void *target = (void *)present_hook;
    memcpy(jump + sizeof indirect_jump, &target, sizeof target);
    DWORD protection;
    if (!VirtualProtect(jump, NEAR_BYTES, PAGE_EXECUTE_READ, &protection)) return "VirtualProtect failed";
    FlushInstructionCache(GetCurrentProcess(), jump, sizeof indirect_jump + sizeof target);
    BYTE *call = api->engine + PRESENT_CALL_RVA;
    BYTE call_jump[sizeof present_call] = {0xe8, 0, 0, 0, 0, 0x90};
    int32_t offset = (int32_t)(jump - (call + 5));
    memcpy(call_jump + 1, &offset, sizeof offset);
    return api->patch(call, present_call, call_jump, sizeof call_jump);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    BOOL whole_number = read_limit();
    if (!whole_number) host->log_line("fpslimit: fps_limit in s2prof.ini is not a whole number; no cap");
    if (!fps_limit) return "off ([fps] fps_limit=0 in s2prof.ini)";
    if (!host->report_running) return "the host is older than this module (no report_running)";
    const char *error = S2_CHECK_ENGINE(host, fpslimit_checks);
    if (error) return error;
    api = host;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&counter);
    frequency = counter.QuadPart;
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    spin_ticks = frequency * SPIN_US_HIGH_RESOLUTION / 1000000;
    if (!timer && (error = plain_timer())) return error;
    if ((error = patch_present_call())) return error;
    api->log_line("fpslimit: frames capped at %ld a second", fps_limit);
    return NULL;
}
