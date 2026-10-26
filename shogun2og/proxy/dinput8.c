#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "exports.h"

// Loaded from the game folder in place of the system dinput8.dll. Only the engine imports it, statically, so it is loaded
// into the game process in DX9 and DX11 mode alike, before any render device exists; D3D11 itself is loaded later and
// only in DX11 mode, and the HTML process loads it too. Every export jumps to the system DLL's. The host (s2prof.dll) is
// started from here; without it the proxy only forwards.
#define HOST_FILE L"s2prof.dll"
#define ENGINE_MODULE L"empire.retail.dll"
#define LOG_FOLDER L"perf-logs"
#define LOG_FILE L"proxy.log"

#define DECLARE_POINTER(name, ordinal) void *real_##name;
DINPUT8_EXPORTS(DECLARE_POINTER)

#define DEFINE_STUB(name, ordinal) \
    __attribute__((naked)) void proxy_##name(void) { __asm__("jmp *_real_" #name); }
DINPUT8_EXPORTS(DEFINE_STUB)

static wchar_t folder[MAX_PATH];

static void log_line(const char *format, ...) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%ls", folder, LOG_FOLDER);
    CreateDirectoryW(path, NULL);
    swprintf(path, MAX_PATH, L"%ls\\%ls\\%ls", folder, LOG_FOLDER, LOG_FILE);
    FILE *file = _wfopen(path, L"a");
    if (!file) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

// Stands in for an export the system DLL lacks; the missing names are in proxy.log.
static void missing_export(void) {
    MessageBoxW(NULL, L"The game called a dinput8 function that the system dinput8.dll does not have. See perf-logs\\proxy.log.", L"s2prof dinput8 proxy",
                MB_ICONERROR);
    ExitProcess(1);
}

// Runs once DllMain has returned, so the host's own loading happens outside the loader lock.
static DWORD WINAPI start_host(LPVOID unused) {
    (void)unused;
    // The engine imports this DLL, so it has finished loading by the time the loader lock is free; any other process that
    // happens to load this file gets the forwarding only.
    if (!GetModuleHandleW(ENGINE_MODULE)) {
        log_line("proxy loaded outside the game; forwarding only");
        return 0;
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%ls", folder, HOST_FILE);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        log_line("no %ls next to the proxy; forwarding only", HOST_FILE);
        return 0;
    }
    if (!LoadLibraryW(path)) log_line("loading %ls failed: error %lu", HOST_FILE, GetLastError());
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    GetModuleFileNameW(instance, folder, MAX_PATH);
    *wcsrchr(folder, L'\\') = 0;
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\dinput8.dll");
    HMODULE system = LoadLibraryW(path);
    if (!system) {
        log_line("cannot load %ls: error %lu", path, GetLastError());
        return FALSE;
    }
#define RESOLVE(name, ordinal)                                       \
    if (!(real_##name = (void *)GetProcAddress(system, #name))) {   \
        real_##name = (void *)missing_export;                       \
        log_line("the system dinput8.dll has no %s", #name);        \
    }
    DINPUT8_EXPORTS(RESOLVE)
    log_line("proxy loaded");
    HANDLE thread = CreateThread(NULL, 0, start_host, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    return TRUE;
}
