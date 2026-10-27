#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include "../checks/borderless.h"
#include <stdio.h>
#include <wchar.h>
#include <windowsx.h>

// Borderless windowed, set by [window] borderless=1 in s2prof.ini (read at start): the game's windowed mode
// (gfx_fullscreen false) in a window without a frame that covers its monitor. The game renders at the resolution chosen
// in its options and DXGI scales the frame to the window. Fullscreen mode is left alone. [window] clip_cursor=1 also
// keeps the cursor on that monitor while the game has focus: the engine never confines it, so with several monitors edge
// scrolling would push it onto the next one.
//
// - The window is restyled on the main thread, from the engine's PeekMessageW, once the swap chain exists: restyling a
//   window sends it messages synchronously, and the main thread is blocked in D3D11CreateDevice while the fixes install.
// - The engine restyles and moves its window again on a mode or resolution change (around 0x114166ef); its imports of
//   SetWindowLongW, SetWindowPos and MoveWindow keep the frame off and the window on its monitor. A popup style the engine
//   sets itself is fullscreen, which hands the window back to it.
// - The engine takes the window's client size as its resolution (the display change at 0x114167de, WM_SIZE 0x11401695,
//   the settings refresh 0x113fd793), so its GetClientRect of the window reports the size the engine asked for: the size
//   it created the window with, then each MoveWindow's size less the frame of the style it set.
// - The engine reads the cursor from mouse messages in client coordinates (window procedure 0x1009d2e0) and never scales
//   them, so they are scaled from the window to that size.
#define WINDOW_RVA 0x1c9a938
#define SETTINGS_RVA 0x1c9a924
#define SETTINGS_WINDOWED_OFFSET 0x1c
#define RENDER_DEVICE_RVA 0x1c9a93c
#define SWAP_CHAIN_OFFSET 0xbc
#define SET_WINDOW_LONG_IAT_RVA 0x15a1618
#define SET_WINDOW_POS_IAT_RVA 0x15a15a4
#define MOVE_WINDOW_IAT_RVA 0x15a1530
#define PEEK_MESSAGE_IAT_RVA 0x15a163c
#define GET_CLIENT_RECT_IAT_RVA 0x15a1578
#define CHECK_INTERVAL_MS 100
#define FRAME_STYLES (WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define FRAME_EX_STYLES (WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE)

typedef LONG(WINAPI *SetWindowLongFn)(HWND window, int index, LONG value);
typedef BOOL(WINAPI *SetWindowPosFn)(HWND window, HWND after, int x, int y, int width, int height, UINT flags);
typedef BOOL(WINAPI *MoveWindowFn)(HWND window, int x, int y, int width, int height, BOOL repaint);
typedef BOOL(WINAPI *PeekMessageFn)(MSG *message, HWND window, UINT first, UINT last, UINT remove);
typedef BOOL(WINAPI *GetClientRectFn)(HWND window, RECT *rect);

static const S2Api *api;
static SetWindowLongFn real_set_window_long;
static SetWindowPosFn real_set_window_pos;
static MoveWindowFn real_move_window;
static PeekMessageFn real_peek_message;
static GetClientRectFn real_get_client_rect;
static BOOL borderless_wanted, clip_wanted, cursor_clipped;
// The window this module made borderless; NULL while the engine runs it fullscreen.
static volatile HWND borderless_window;
static LONG engine_style;
static SIZE game_resolution;
static ULONGLONG next_check;
static RECT logged_rect;
static SIZE logged_resolution;

static HWND game_window(void) {
    return *(HWND *)(api->engine + WINDOW_RVA);
}

static BOOL engine_windowed(void) {
    BYTE *settings = *(BYTE **)(api->engine + SETTINGS_RVA);
    return settings && settings[SETTINGS_WINDOWED_OFFSET];
}

static BOOL swap_chain_exists(void) {
    BYTE *device = *(BYTE **)(api->engine + RENDER_DEVICE_RVA);
    return device && *(void **)(device + SWAP_CHAIN_OFFSET);
}

static RECT monitor_rect(HWND window) {
    MONITORINFO info = {.cbSize = sizeof info};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
    return info.rcMonitor;
}

static void log_layout(HWND window) {
    RECT monitor = monitor_rect(window);
    if (EqualRect(&monitor, &logged_rect) && game_resolution.cx == logged_resolution.cx && game_resolution.cy == logged_resolution.cy) return;
    logged_rect = monitor;
    logged_resolution = game_resolution;
    api->log_line("borderless: the window covers its monitor, %ldx%ld at %ld,%ld; the game renders at %ldx%ld, scaled to it",
                  monitor.right - monitor.left, monitor.bottom - monitor.top, monitor.left, monitor.top, game_resolution.cx, game_resolution.cy);
}

// The engine sized the window for its resolution before handing it over.
static void take_window(HWND window) {
    if (window == borderless_window) return;
    RECT client;
    real_get_client_rect(window, &client);
    game_resolution = (SIZE){client.right, client.bottom};
    engine_style = GetWindowLongW(window, GWL_STYLE);
    borderless_window = window;
}

static void make_borderless(HWND window) {
    LONG style = GetWindowLongW(window, GWL_STYLE), ex_style = GetWindowLongW(window, GWL_EXSTYLE);
    RECT monitor = monitor_rect(window), current;
    GetWindowRect(window, &current);
    BOOL framed = (style & FRAME_STYLES) || (ex_style & FRAME_EX_STYLES);
    if (!framed && window == borderless_window && EqualRect(&current, &monitor)) return;
    take_window(window);
    if (framed) {
        real_set_window_long(window, GWL_STYLE, (style & ~FRAME_STYLES) | WS_POPUP);
        real_set_window_long(window, GWL_EXSTYLE, ex_style & ~FRAME_EX_STYLES);
    }
    real_set_window_pos(window, HWND_TOP, monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
                        SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    log_layout(window);
}

static void update_cursor_clip(HWND window) {
    if (clip_wanted && window && window == borderless_window && GetForegroundWindow() == window && !IsIconic(window)) {
        RECT monitor = monitor_rect(window), clip;
        if (!GetClipCursor(&clip) || !EqualRect(&clip, &monitor)) ClipCursor(&monitor);
        cursor_clipped = TRUE;
    } else if (cursor_clipped) {
        ClipCursor(NULL);
        cursor_clipped = FALSE;
    }
}

// Mouse messages carry client coordinates, except the wheels' screen coordinates.
static void scale_mouse(MSG *message) {
    if (message->hwnd != borderless_window || message->message < WM_MOUSEFIRST || message->message > WM_MOUSELAST ||
        message->message == WM_MOUSEWHEEL || message->message == WM_MOUSEHWHEEL)
        return;
    RECT client;
    real_get_client_rect(message->hwnd, &client);
    int x = GET_X_LPARAM(message->lParam) * game_resolution.cx / client.right, y = GET_Y_LPARAM(message->lParam) * game_resolution.cy / client.bottom;
    message->lParam = MAKELPARAM(x, y);
}

static BOOL WINAPI peek_message_hook(MSG *message, HWND window, UINT first, UINT last, UINT remove) {
    if (GetCurrentThreadId() == *api->main_thread) {
        ULONGLONG now = GetTickCount64();
        if (now >= next_check) {
            next_check = now + CHECK_INTERVAL_MS;
            HWND game = game_window();
            if (game && engine_windowed() && swap_chain_exists() && GetWindowThreadProcessId(game, NULL) == GetCurrentThreadId())
                make_borderless(game);
            update_cursor_clip(game);
        }
    }
    BOOL found = real_peek_message(message, window, first, last, remove);
    if (found) scale_mouse(message);
    return found;
}

static BOOL WINAPI get_client_rect_hook(HWND window, RECT *rect) {
    if (window != borderless_window) return real_get_client_rect(window, rect);
    *rect = (RECT){0, 0, game_resolution.cx, game_resolution.cy};
    return TRUE;
}

static LONG WINAPI set_window_long_hook(HWND window, int index, LONG value) {
    if (window == game_window()) {
        if (index == GWL_STYLE && (value & WS_CAPTION)) {
            take_window(window);
            engine_style = value;
            value = (value & ~FRAME_STYLES) | WS_POPUP;
        } else if (index == GWL_STYLE) {
            borderless_window = NULL;
        } else if (index == GWL_EXSTYLE && window == borderless_window) {
            value &= ~FRAME_EX_STYLES;
        }
    }
    return real_set_window_long(window, index, value);
}

static BOOL WINAPI set_window_pos_hook(HWND window, HWND after, int x, int y, int width, int height, UINT flags) {
    if (window == borderless_window && window == game_window()) {
        RECT monitor = monitor_rect(window);
        x = monitor.left, y = monitor.top, width = monitor.right - monitor.left, height = monitor.bottom - monitor.top;
        flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
    }
    return real_set_window_pos(window, after, x, y, width, height, flags);
}

static BOOL WINAPI move_window_hook(HWND window, int x, int y, int width, int height, BOOL repaint) {
    if (window == borderless_window && window == game_window()) {
        RECT frame = {0}, monitor = monitor_rect(window);
        AdjustWindowRect(&frame, engine_style, FALSE);
        game_resolution = (SIZE){width - (frame.right - frame.left), height - (frame.bottom - frame.top)};
        x = monitor.left, y = monitor.top, width = monitor.right - monitor.left, height = monitor.bottom - monitor.top;
    }
    BOOL moved = real_move_window(window, x, y, width, height, repaint);
    if (window == borderless_window) log_layout(window);
    return moved;
}

static const struct {
    DWORD rva;
    const char *name;
    void *hook;
    void **real;
} imports[] = {{SET_WINDOW_LONG_IAT_RVA, "SetWindowLongW", (void *)set_window_long_hook, (void **)&real_set_window_long},
               {SET_WINDOW_POS_IAT_RVA, "SetWindowPos", (void *)set_window_pos_hook, (void **)&real_set_window_pos},
               {MOVE_WINDOW_IAT_RVA, "MoveWindow", (void *)move_window_hook, (void **)&real_move_window},
               {PEEK_MESSAGE_IAT_RVA, "PeekMessageW", (void *)peek_message_hook, (void **)&real_peek_message},
               {GET_CLIENT_RECT_IAT_RVA, "GetClientRect", (void *)get_client_rect_hook, (void **)&real_get_client_rect}};

// s2prof.ini sits next to the game, two folders up from fixes\borderless.dll.
static void read_settings(void) {
    wchar_t path[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)read_settings, &self);
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return;
    for (int up = 0; up < 2; up++) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) *slash = 0;
    }
    wcscat(path, L"\\s2prof.ini");
    borderless_wanted = GetPrivateProfileIntW(L"window", L"borderless", 0, path) != 0;
    clip_wanted = GetPrivateProfileIntW(L"window", L"clip_cursor", 0, path) != 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    read_settings();
    if (!borderless_wanted) return "off ([window] borderless=0 in s2prof.ini)";
    const char *mismatch = S2_CHECK_ENGINE(host, borderless_checks);
    if (mismatch) return mismatch;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static char error[96];
    for (size_t i = 0; i < sizeof imports / sizeof *imports; i++)
        if (*(void **)(api->engine + imports[i].rva) != (void *)GetProcAddress(user32, imports[i].name)) {
            snprintf(error, sizeof error, "the import slot does not hold %s", imports[i].name);
            return error;
        }
    for (size_t i = 0; i < sizeof imports / sizeof *imports; i++) {
        BYTE *slot = api->engine + imports[i].rva;
        *imports[i].real = *(void **)slot;
        const char *patch_error = api->patch(slot, (const BYTE *)imports[i].real, (const BYTE *)&imports[i].hook, sizeof imports[i].hook);
        if (patch_error) return patch_error;
    }
    api->log_line("borderless: on, cursor clip %s; applied once the swap chain exists (windowed mode only)", clip_wanted ? "on" : "off");
    return NULL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance, (void)reserved;
    if (reason == DLL_PROCESS_DETACH && cursor_clipped) ClipCursor(NULL);
    return TRUE;
}
