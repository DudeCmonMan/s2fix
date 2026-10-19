#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdarg.h>
#include <stdio.h>
#include "exports.h"
#include "wrap.h"

// Loaded from the game folder in place of the system d3d11.dll, which the engine loads by bare name. Every export jumps to
// the system DLL's; the two device-creation calls can also drop D3D11_CREATE_DEVICE_SINGLETHREADED, which forbids deferred
// contexts. The host (s2prof.dll) is started from here; without it the proxy only forwards.
#define CONFIG_FILE L"s2prof.ini"
#define HOST_FILE L"s2prof.dll"
#define LOG_FOLDER L"perf-logs"
#define LOG_FILE L"proxy.log"

typedef HRESULT(WINAPI *CreateDeviceFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
                                        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *CreateDeviceAndSwapChainFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                    const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                                    ID3D11DeviceContext **);

#define DECLARE_POINTER(name, ordinal) void *real_##name;
D3D11_EXPORTS(DECLARE_POINTER, DECLARE_POINTER)

#define DEFINE_STUB(name, ordinal) \
    __attribute__((naked)) void proxy_##name(void) { __asm__("jmp *real_" #name "(%rip)"); }
#define NO_STUB(name, ordinal)
D3D11_EXPORTS(DEFINE_STUB, NO_STUB)

static wchar_t folder[MAX_PATH];
static BOOL multithreaded_device, wrap_context;

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

static UINT device_flags(UINT flags) {
    return multithreaded_device ? flags & ~D3D11_CREATE_DEVICE_SINGLETHREADED : flags;
}

HRESULT WINAPI proxy_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
                                       const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count, UINT sdk_version, ID3D11Device **device,
                                       D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context) {
    HRESULT result = ((CreateDeviceFn)real_D3D11CreateDevice)(adapter, driver_type, software, device_flags(flags), feature_levels, feature_level_count,
                                                              sdk_version, device, feature_level, context);
    log_line("D3D11CreateDevice: flags 0x%x passed as 0x%x -> 0x%lx", flags, device_flags(flags), result);
    if (SUCCEEDED(result) && wrap_context && device) wrap_device(device, context);
    return result;
}

HRESULT WINAPI proxy_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
                                                   const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count, UINT sdk_version,
                                                   const DXGI_SWAP_CHAIN_DESC *swap_chain_desc, IDXGISwapChain **swap_chain, ID3D11Device **device,
                                                   D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context) {
    HRESULT result = ((CreateDeviceAndSwapChainFn)real_D3D11CreateDeviceAndSwapChain)(adapter, driver_type, software, device_flags(flags), feature_levels,
                                                                                      feature_level_count, sdk_version, swap_chain_desc, swap_chain,
                                                                                      device, feature_level, context);
    log_line("D3D11CreateDeviceAndSwapChain: flags 0x%x passed as 0x%x -> 0x%lx", flags, device_flags(flags), result);
    if (SUCCEEDED(result) && wrap_context && device) wrap_device(device, context);
    return result;
}

// Runs once DllMain has returned, so the host's own loading happens outside the loader lock.
static DWORD WINAPI start_host(LPVOID unused) {
    (void)unused;
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
    swprintf(path, MAX_PATH, L"%ls\\%ls", folder, CONFIG_FILE);
    multithreaded_device = GetPrivateProfileIntW(L"proxy", L"multithreaded_device", 0, path);
    wrap_context = GetPrivateProfileIntW(L"proxy", L"wrap_context", 0, path);
    log_line("proxy loaded; multithreaded_device=%d wrap_context=%d", multithreaded_device, wrap_context);
    HANDLE thread = CreateThread(NULL, 0, start_host, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    return TRUE;
}
