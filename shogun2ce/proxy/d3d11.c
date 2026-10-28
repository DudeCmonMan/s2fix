#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "exports.h"

// Loaded from the game folder in place of the system d3d11.dll, which the engine loads by bare name. Every export jumps to
// the system DLL's. It loads s2fix (s2prof.dll beside it) as it loads, so s2fix's device-time hook is in before the
// engine's own LoadLibrary("d3d11.dll") returns; s2fix does the device work itself, so any other loader that brings it
// in before the device is created works too.
#define HOST_FILE L"s2prof.dll"
#define LOG_FOLDER L"perf-logs"
#define LOG_FILE L"proxy.log"

#define DECLARE_POINTER(name, ordinal) void *real_##name;
D3D11_EXPORTS(DECLARE_POINTER, DECLARE_POINTER)

#define DEFINE_STUB(name, ordinal) \
    __attribute__((naked)) void proxy_##name(void) { __asm__("jmp *real_" #name "(%rip)"); }
#define NO_STUB(name, ordinal)
D3D11_EXPORTS(DEFINE_STUB, NO_STUB)

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
    MessageBoxW(NULL, L"The game called a d3d11 function that the system d3d11.dll does not have. See perf-logs\\proxy.log.", L"s2prof d3d11 proxy",
                MB_ICONERROR);
    ExitProcess(1);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    GetModuleFileNameW(instance, folder, MAX_PATH);
    *wcsrchr(folder, L'\\') = 0;
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\d3d11.dll");
    HMODULE system = LoadLibraryW(path);
    if (!system) {
        log_line("cannot load %ls: error %lu", path, GetLastError());
        return FALSE;
    }
#define RESOLVE(name, ordinal)                                       \
    if (!(real_##name = (void *)GetProcAddress(system, #name))) {   \
        real_##name = (void *)missing_export;                       \
        log_line("the system d3d11.dll has no %s", #name);          \
    }
    D3D11_EXPORTS(RESOLVE, RESOLVE)
    swprintf(path, MAX_PATH, L"%ls\\%ls", folder, HOST_FILE);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        log_line("no %ls next to the proxy; forwarding only", HOST_FILE);
    else if (!LoadLibraryW(path))
        log_line("loading %ls failed: error %lu (a second s2fix in this process refuses to load)", HOST_FILE, GetLastError());
    else
        log_line("proxy loaded s2fix");
    return TRUE;
}
