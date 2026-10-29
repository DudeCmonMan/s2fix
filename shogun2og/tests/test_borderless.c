// Usage: test_borderless.exe <folder holding fixes\borderless.dll and s2prof.ini with [window] borderless=1>
//
// The borderless module against a stand-in engine: a block holding the engine's import slots (the real user32
// functions), its window, settings and render device globals, and a window made as the engine makes its windowed one,
// for a resolution wider than the desktop (as at a downsampling resolution on one monitor), which Windows makes smaller.
// Through the slots, as the engine calls them: the window is restyled to cover its monitor while GetClientRect reports
// the engine's resolution; mouse messages are scaled to it, the monitor's last pixel to the resolution's last; a display
// change (SetWindowLongW, AdjustWindowRect, MoveWindow, GetClientRect, as at 0x11416729) keeps the window on its monitor
// and takes the new resolution; a popup style hands the window back. Runs on a desktop of its own, since the window
// covers the monitor.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <stdarg.h>
#include <stdio.h>
#include "s2module.h"

#define ENGINE_BYTES 0x1d00000
#define WINDOW_RVA 0x1c9a938
#define SETTINGS_RVA 0x1c9a924
#define RENDER_DEVICE_RVA 0x1c9a93c
#define SET_WINDOW_LONG_IAT_RVA 0x15a1618
#define SET_WINDOW_POS_IAT_RVA 0x15a15a4
#define MOVE_WINDOW_IAT_RVA 0x15a1530
#define PEEK_MESSAGE_IAT_RVA 0x15a163c
#define GET_CLIENT_RECT_IAT_RVA 0x15a1578
#define WINDOWED_STYLE 0x14c20000
#define FULLSCREEN_STYLE 0x90000000
#define DESKTOP L"s2fix_borderless_check"

typedef LONG(WINAPI *SetWindowLongFn)(HWND window, int index, LONG value);
typedef BOOL(WINAPI *MoveWindowFn)(HWND window, int x, int y, int width, int height, BOOL repaint);
typedef BOOL(WINAPI *PeekMessageFn)(MSG *message, HWND window, UINT first, UINT last, UINT remove);
typedef BOOL(WINAPI *GetClientRectFn)(HWND window, RECT *rect);

static BYTE *engine;
static volatile LONG fix_enabled = 1;
static volatile DWORD main_thread;
static BYTE settings[0x40], device[0x100];
static int failures;

static void log_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printf("  log: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

static const char *patch(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size) {
    if (memcmp(target, expected, size)) return "expected bytes differ";
    memcpy(target, replacement, size);
    return NULL;
}

static const char *check_engine(const S2Check *checks, int count) {
    (void)checks, (void)count;
    return NULL;
}

static void register_switch(const char *name, volatile LONG *flag, void (*report)(BOOL start)) {
    (void)name, (void)flag, (void)report;
}

static void *slot(DWORD rva) {
    return *(void **)(engine + rva);
}

static void expect(BOOL ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "FAILED", what);
    failures += !ok;
}

// The engine's loop, long enough for the module's 100 ms check to run.
static void pump(DWORD milliseconds) {
    MSG message;
    ULONGLONG until = GetTickCount64() + milliseconds;
    while (GetTickCount64() < until) {
        while (((PeekMessageFn)slot(PEEK_MESSAGE_IAT_RVA))(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(10);
    }
}

static BOOL client_is(HWND window, LONG width, LONG height) {
    RECT client;
    ((GetClientRectFn)slot(GET_CLIENT_RECT_IAT_RVA))(window, &client);
    printf("  GetClientRect: %ldx%ld\n", client.right, client.bottom);
    return client.left == 0 && client.top == 0 && client.right == width && client.bottom == height;
}

static BOOL covers_monitor(HWND window, RECT monitor) {
    RECT current;
    GetWindowRect(window, &current);
    return EqualRect(&current, &monitor) && !(GetWindowLongW(window, GWL_STYLE) & WS_CAPTION);
}

static LPARAM mouse_through_engine(HWND window, UINT message, int x, int y) {
    MSG received = {0};
    PostMessageW(window, message, 0, MAKELPARAM(x, y));
    while (!((PeekMessageFn)slot(PEEK_MESSAGE_IAT_RVA))(&received, window, message, message, PM_REMOVE)) Sleep(1);
    return received.lParam;
}

// The display change's windowed branch: style, window size for the client size, the move, then the client size read
// back as the resolution.
static void change_resolution(HWND window, LONG width, LONG height) {
    ((SetWindowLongFn)slot(SET_WINDOW_LONG_IAT_RVA))(window, GWL_STYLE, WINDOWED_STYLE);
    RECT rect = {0, 0, width, height};
    AdjustWindowRect(&rect, WINDOWED_STYLE, FALSE);
    ((MoveWindowFn)slot(MOVE_WINDOW_IAT_RVA))(window, 100, 100, rect.right - rect.left, rect.bottom - rect.top, TRUE);
}

static int child(const wchar_t *folder) {
    main_thread = GetCurrentThreadId();
    engine = VirtualAlloc(NULL, ENGINE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const struct {
        DWORD rva;
        const char *name;
    } imports[] = {{SET_WINDOW_LONG_IAT_RVA, "SetWindowLongW"}, {SET_WINDOW_POS_IAT_RVA, "SetWindowPos"}, {MOVE_WINDOW_IAT_RVA, "MoveWindow"},
                   {PEEK_MESSAGE_IAT_RVA, "PeekMessageW"}, {GET_CLIENT_RECT_IAT_RVA, "GetClientRect"}};
    for (size_t i = 0; i < sizeof imports / sizeof *imports; i++) *(FARPROC *)(engine + imports[i].rva) = GetProcAddress(user32, imports[i].name);
    settings[0x1c] = 1;
    *(void **)(device + 0xbc) = device;
    *(BYTE **)(engine + SETTINGS_RVA) = settings;
    *(BYTE **)(engine + RENDER_DEVICE_RVA) = device;

    WNDCLASSW window_class = {.lpfnWndProc = DefWindowProcW, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"s2fix_borderless_check"};
    RegisterClassW(&window_class);
    LONG first_width = GetSystemMetrics(SM_CXVIRTUALSCREEN) + 256, first_height = 720;
    *(LONG *)(settings + 0x20) = first_width;
    *(LONG *)(settings + 0x24) = first_height;
    RECT rect = {0, 0, first_width, first_height};
    AdjustWindowRect(&rect, WINDOWED_STYLE, FALSE);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Shogun 2", WINDOWED_STYLE, 50, 50, rect.right - rect.left, rect.bottom - rect.top,
                                  NULL, NULL, window_class.hInstance, NULL);
    *(HWND *)(engine + WINDOW_RVA) = window;

    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\fixes\\borderless.dll", folder);
    HMODULE module = LoadLibraryW(path);
    S2ModuleMain module_main = module ? (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main") : NULL;
    S2Api api = {.engine = engine,
                 .fix_enabled = &fix_enabled,
                 .log_line = log_line,
                 .register_switch = register_switch,
                 .patch = patch,
                 .main_thread = &main_thread,
                 .check_engine = check_engine};
    const char *error = module_main ? module_main(&api) : "cannot load the module";
    printf("module: %s\n", error ? error : "installed");
    if (error) return 1;

    MONITORINFO info = {.cbSize = sizeof info};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
    RECT monitor = info.rcMonitor;
    LONG monitor_width = monitor.right - monitor.left, monitor_height = monitor.bottom - monitor.top;
    printf("monitor %ldx%ld\n", monitor_width, monitor_height);

    pump(300);
    expect(covers_monitor(window, monitor), "the window lost its frame and covers its monitor");
    printf("  the engine made the window for %ldx%ld\n", first_width, first_height);
    expect(client_is(window, first_width, first_height), "GetClientRect reports the resolution the engine made the window for, wider than the desktop");
    LPARAM corner = mouse_through_engine(window, WM_MOUSEMOVE, monitor_width - 1, monitor_height - 1);
    printf("  mouse at %ld,%ld arrives at %d,%d\n", monitor_width - 1, monitor_height - 1, GET_X_LPARAM(corner), GET_Y_LPARAM(corner));
    expect(GET_X_LPARAM(corner) == first_width - 1 && GET_Y_LPARAM(corner) == first_height - 1,
           "a mouse message's position is scaled to the resolution: the last pixel to the last");
    LPARAM wheel = mouse_through_engine(window, WM_MOUSEWHEEL, 900, 500);
    expect(GET_X_LPARAM(wheel) == 900 && GET_Y_LPARAM(wheel) == 500, "the wheel's screen position is left alone");

    change_resolution(window, 1600, 900);
    pump(300);
    expect(covers_monitor(window, monitor), "after a change to 1600x900 the window still covers its monitor");
    expect(client_is(window, 1600, 900), "GetClientRect reports 1600x900, so the engine takes it as its resolution");
    LPARAM middle = mouse_through_engine(window, WM_LBUTTONDOWN, monitor_width / 2, monitor_height / 2);
    expect(GET_X_LPARAM(middle) == monitor_width / 2 * 1600 / monitor_width && GET_Y_LPARAM(middle) == monitor_height / 2 * 900 / monitor_height,
           "a click is scaled to 1600x900");
    corner = mouse_through_engine(window, WM_MOUSEMOVE, monitor_width - 1, monitor_height - 1);
    expect(GET_X_LPARAM(corner) == 1599 && GET_Y_LPARAM(corner) == 899, "at 1600x900 the last pixel goes to the last too");

    change_resolution(window, monitor_width, monitor_height);
    pump(300);
    expect(client_is(window, monitor_width, monitor_height), "a change to the monitor's resolution is taken too");
    LPARAM same = mouse_through_engine(window, WM_MOUSEMOVE, 123, 456);
    expect(GET_X_LPARAM(same) == 123 && GET_Y_LPARAM(same) == 456, "at the monitor's resolution a position is unchanged");

    ((SetWindowLongFn)slot(SET_WINDOW_LONG_IAT_RVA))(window, GWL_STYLE, (LONG)FULLSCREEN_STYLE);
    RECT real;
    GetClientRect(window, &real);
    expect(client_is(window, real.right, real.bottom), "fullscreen hands the window back: GetClientRect is the real one");

    DestroyWindow(window);
    printf("%d borderless checks failed\n", failures);
    return failures != 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && !wcscmp(argv[2], L"--child")) return child(argv[1]);
    if (argc != 2) {
        fwprintf(stderr, L"usage: test_borderless <folder holding fixes\\borderless.dll and s2prof.ini>\n");
        return 2;
    }
    HDESK desktop = CreateDesktopW(DESKTOP, NULL, NULL, 0, GENERIC_ALL, NULL);
    wchar_t self[MAX_PATH], command[3 * MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(command, sizeof command / sizeof *command, L"\"%ls\" \"%ls\" --child", self, argv[1]);
    STARTUPINFOW startup = {.cb = sizeof startup, .lpDesktop = DESKTOP};
    PROCESS_INFORMATION process;
    if (!desktop || !CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        wprintf(L"FAIL: cannot run the check on a desktop of its own: error %lu\n", GetLastError());
        return 1;
    }
    DWORD code = 1;
    if (WaitForSingleObject(process.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseDesktop(desktop);
    return (int)code;
}
