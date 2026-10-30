#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "exports.h"

// Stands in for the system d3d11.dll, which the engine loads by bare name in DX11 mode, and loads the host, so its
// device-time patches are in before the engine creates its device.
#define HOST_FILE L"s2prof.dll"
#define ENGINE_MODULE L"empire.retail.dll"
#define LOG_FOLDER L"perf-logs"
#define LOG_FILE L"proxy.log"

#define DECLARE_POINTER(name, ordinal) void *real_##name;
D3D11_EXPORTS(DECLARE_POINTER)

#define DEFINE_STUB(name, ordinal) \
    __attribute__((naked)) void proxy_##name(void) { __asm__("jmp *_real_" #name); }
D3D11_EXPORTS(DEFINE_STUB)

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

// Any other process that loads this file (the HTML process imports d3d11.dll) gets the forwarding only.
static void load_host(void) {
    if (!GetModuleHandleW(ENGINE_MODULE)) {
        log_line("proxy loaded outside the game; forwarding only");
        return;
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%ls", folder, HOST_FILE);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        log_line("no %ls next to the proxy; forwarding only", HOST_FILE);
        return;
    }
    if (!LoadLibraryW(path)) log_line("loading %ls failed: error %lu (another s2fix may be loaded already)", HOST_FILE, GetLastError());
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
    D3D11_EXPORTS(RESOLVE)
    log_line("proxy loaded");
    // Before the engine's LoadLibrary returns, so the host's patches precede anything the engine does next.
    load_host();
    return TRUE;
}
