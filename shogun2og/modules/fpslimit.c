#include "../s2module.h"
#include "../checks/fpslimit.h"
#include <wchar.h>

// A frame rate cap, [fps] battle_fps_limit (battles and the menus) and campaign_fps_limit (the campaign map), frames a
// second, whole numbers; 0, the default, is none: after each tick's rendering, which presents the frame, the main thread
// waits for the end of the frame's slot. A frame more than a slot late starts the slots again rather than letting the
// next ones hurry. The loading screen presents from elsewhere, so it isn't held, and the debug report measures the game
// uncapped.
// The main tick's one call of its rendering (thiscall; three arguments).
#define RENDER_CALL_RVA 0xb7433
#define RENDER_RVA 0xafaf0
// The main tick runs the handler on top of the app's stack; the campaign map's has its own class.
#define APP_RVA 0x17c755c
#define APP_HANDLER_COUNT 0x38
#define APP_HANDLERS 0x3c
#define CAMPAIGN_HANDLER_VTABLE_RVA 0x16068b0
// The timer wakes this early and the rest is spun: a timer can be late by up to its resolution.
#define SPIN_US_HIGH_RESOLUTION 500
#define SPIN_US 2000
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

typedef DWORD(__attribute__((thiscall)) *RenderFn)(BYTE *self, DWORD a, DWORD b, DWORD c);
typedef UINT(WINAPI *TimePeriodFn)(UINT period);

static const BYTE render_call[] = {0xe8, 0xb8, 0x86, 0xff, 0xff};

// The caps in frames a second; can be patched live.
__declspec(dllexport) volatile LONG battle_fps_limit, campaign_fps_limit;

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

static BOOL on_campaign_map(void) {
    BYTE *app = *(BYTE **)(api->engine + APP_RVA);
    BYTE *handler = (*(BYTE ***)(app + APP_HANDLERS))[*(LONG *)(app + APP_HANDLER_COUNT) - 1];
    return handler && *(BYTE **)handler == api->engine + CAMPAIGN_HANDLER_VTABLE_RVA;
}

static DWORD __attribute__((thiscall)) render_hook(BYTE *self, DWORD a, DWORD b, DWORD c) {
    DWORD result = ((RenderFn)(void *)(api->engine + RENDER_RVA))(self, a, b, c);
    LONG limit = on_campaign_map() ? campaign_fps_limit : battle_fps_limit;
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

// Whole numbers only: 7.5 or -1 mean no cap rather than 7 or off. A key the ini lacks leaves the limit as it was.
static void read_key(const wchar_t *path, const wchar_t *key, volatile LONG *limit) {
    wchar_t text[16] = L"";
    GetPrivateProfileStringW(L"fps", key, L"", text, sizeof text / sizeof *text, path);
    if (!text[0]) return;
    wchar_t *end;
    unsigned long value = wcstoul(text, &end, 10);
    if (text[0] < L'0' || text[0] > L'9' || *end || value > 0x7fffffff) {
        api->log_line("fpslimit: %ls in s2prof.ini is not a whole number; no cap there", key);
        value = 0;
    }
    *limit = (LONG)value;
}

static void read_limits(void) {
    battle_fps_limit = campaign_fps_limit = 0;
    wchar_t path[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)read_limits, &self);
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return;
    for (int up = 0; up < 2; up++) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) *slash = 0;
    }
    wcscat(path, L"\\s2prof.ini");
    // Deprecated: the one cap of older inis, the default for both.
    read_key(path, L"fps_limit", &battle_fps_limit);
    campaign_fps_limit = battle_fps_limit;
    read_key(path, L"battle_fps_limit", &battle_fps_limit);
    read_key(path, L"campaign_fps_limit", &campaign_fps_limit);
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

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    read_limits();
    if (!battle_fps_limit && !campaign_fps_limit) return "off ([fps] battle_fps_limit and campaign_fps_limit are 0 in s2prof.ini)";
    if (!host->report_running) return "the host is older than this module (no report_running)";
    const char *error = S2_CHECK_ENGINE(host, fpslimit_checks);
    if (error) return error;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&counter);
    frequency = counter.QuadPart;
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    spin_ticks = frequency * SPIN_US_HIGH_RESOLUTION / 1000000;
    if (!timer && (error = plain_timer())) return error;
    BYTE call[5] = {0xe8};
    *(LONG *)(call + 1) = (LONG)((BYTE *)(void *)render_hook - (api->engine + RENDER_CALL_RVA + 5));
    if ((error = api->patch(api->engine + RENDER_CALL_RVA, render_call, call, sizeof call))) return error;
    api->log_line("fpslimit: frames capped at %ld a second in battles and the menus, %ld on the campaign map (0: no cap)", battle_fps_limit,
                  campaign_fps_limit);
    return NULL;
}
