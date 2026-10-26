#include "../s2module.h"
#include <stdint.h>

// The engine's "is key down" query (sub_180e9ead0) calls GetForegroundWindow, a system call, on every query before
// reading its per-frame keyboard snapshot: ~1.4% of the main thread. This answers GetForegroundWindow once per frame,
// so a focus change is seen at the next frame, the granularity the key snapshot already has.
#define GET_FOREGROUND_WINDOW_IAT_RVA 0x215aac0
#define FRAME_STAMP_RVA 0x2ead830

typedef HWND(WINAPI *GetForegroundWindowFn)(void);

// 0 always asks Windows; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG cache_enabled = 1;

static GetForegroundWindowFn real_get_foreground_window;
static volatile LONG *fix_enabled;
static volatile uint64_t *frame_stamp;
// Written window first, stamp second; x86 keeps stores in order, so a reader that sees the stamp also sees its window.
static volatile HWND cached_window;
// The stamp is 0 until the first frame, so starting at 0 would answer "no window" without asking Windows.
static volatile uint64_t cached_stamp = UINT64_MAX;

static HWND WINAPI get_foreground_window_hook(void) {
    if (!*fix_enabled || !cache_enabled) return real_get_foreground_window();
    uint64_t stamp = *frame_stamp;
    if (stamp == cached_stamp) return cached_window;
    HWND window = real_get_foreground_window();
    cached_window = window;
    cached_stamp = stamp;
    return window;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    fix_enabled = api->fix_enabled;
    frame_stamp = (volatile uint64_t *)(api->engine + FRAME_STAMP_RVA);
    void **slot = (void **)(api->engine + GET_FOREGROUND_WINDOW_IAT_RVA);
    if (*slot != (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow")) return "the import slot does not hold GetForegroundWindow";
    real_get_foreground_window = (GetForegroundWindowFn)*slot;
    DWORD protection;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protection)) return "VirtualProtect on the import slot failed";
    InterlockedExchangePointer(slot, (void *)get_foreground_window_hook);
    VirtualProtect(slot, sizeof *slot, protection, &protection);
    return NULL;
}
