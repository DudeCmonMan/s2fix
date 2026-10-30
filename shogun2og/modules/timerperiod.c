#include "../s2module.h"

// The engine's timestamp wraps each read in timeBeginPeriod(1)/timeEndPeriod(1), two kernel calls that also flip the
// timer resolution; holding the period here keeps the count above 0, so those pairs only count.
#define POLL_MS 100

typedef UINT(WINAPI *TimePeriodFn)(UINT period);

__declspec(dllexport) volatile LONG hold_enabled = 1;

static const S2Api *api;
static TimePeriodFn begin_period, end_period;

static DWORD WINAPI follow_switch(LPVOID unused) {
    (void)unused;
    BOOL held = FALSE, logged = FALSE;
    for (;; Sleep(POLL_MS)) {
        BOOL wanted = *api->fix_enabled && hold_enabled;
        if (wanted == held) continue;
        if (wanted) begin_period(1);
        else end_period(1);
        held = wanted;
        if (held && !logged) {
            api->log_line("timerperiod: holding the 1 ms timer period");
            logged = TRUE;
        }
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    HMODULE winmm = GetModuleHandleW(L"winmm.dll");
    if (!winmm) winmm = LoadLibraryW(L"winmm.dll");
    if (!winmm) return "winmm.dll could not be loaded";
    begin_period = (TimePeriodFn)(void *)GetProcAddress(winmm, "timeBeginPeriod");
    end_period = (TimePeriodFn)(void *)GetProcAddress(winmm, "timeEndPeriod");
    if (!begin_period || !end_period) return "winmm.dll lacks timeBeginPeriod or timeEndPeriod";
    HANDLE thread = CreateThread(NULL, 0, follow_switch, NULL, 0, NULL);
    if (!thread) return "CreateThread failed";
    CloseHandle(thread);
    api->register_switch("timerperiod", &hold_enabled, NULL);
    return NULL;
}
