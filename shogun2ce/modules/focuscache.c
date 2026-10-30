#include "../s2module.h"
#include "../checks/focuscache.h"
#include <stdint.h>

// The engine's "is key down" query calls GetForegroundWindow on every call; this answers it once per frame, the
// granularity of the key snapshot it reads anyway.
#define GET_FOREGROUND_WINDOW_IAT_RVA 0x215aac0
#define FRAME_STAMP_RVA 0x2ead830

typedef HWND(WINAPI *GetForegroundWindowFn)(void);

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
    const char *mismatch = S2_CHECK_ENGINE(api, focuscache_checks);
    if (mismatch) return mismatch;
    fix_enabled = api->fix_enabled;
    frame_stamp = (volatile uint64_t *)(api->engine + FRAME_STAMP_RVA);
    void **slot = (void **)(api->engine + GET_FOREGROUND_WINDOW_IAT_RVA);
    if (*slot != (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow")) return "the import slot does not hold GetForegroundWindow";
    real_get_foreground_window = (GetForegroundWindowFn)*slot;
    // The loader fills the slot and no relocation covers it, so its current value is what the patch expects.
    void *hook = (void *)get_foreground_window_hook;
    return api->patch((BYTE *)slot, (const BYTE *)slot, (const BYTE *)&hook, sizeof hook);
}
