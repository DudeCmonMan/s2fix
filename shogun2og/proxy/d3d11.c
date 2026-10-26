#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdarg.h>
#include <stdio.h>
#include "exports.h"

// Loaded from the game folder in place of the system d3d11.dll, which the engine loads by bare name as it creates its
// D3D11 device (0x1144fc42), so only in DirectX 11 mode. Every export jumps to the system DLL's. The host (s2prof.dll)
// is started from here, and the two device-creation calls wait until it has installed the fixes: some must be in place
// before the device and its swap chain exist (the flip-model swap chain). They also drop D3D11_CREATE_DEVICE_SINGLETHREADED
// when [device] multithreaded_device asks, since it forbids the overhaul's deferred contexts. Without the host the proxy
// only forwards.
#define CONFIG_FILE L"s2prof.ini"
#define HOST_FILE L"s2prof.dll"
#define ENGINE_MODULE L"empire.retail.dll"
#define LOG_FOLDER L"perf-logs"
#define LOG_FILE L"proxy.log"
// The host installs the fixes in well under a second; past this the device is created without them.
#define HOST_WAIT_MS 30000

typedef HRESULT(WINAPI *CreateDeviceFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
                                        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *CreateDeviceAndSwapChainFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                    const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                                    ID3D11DeviceContext **);
typedef BOOL (*WaitReadyFn)(DWORD milliseconds);

#define DECLARE_POINTER(name, ordinal) void *real_##name;
D3D11_EXPORTS(DECLARE_POINTER, DECLARE_POINTER)

#define DEFINE_STUB(name, ordinal) \
    __attribute__((naked)) void proxy_##name(void) { __asm__("jmp *_real_" #name); }
#define NO_STUB(name, ordinal)
D3D11_EXPORTS(DEFINE_STUB, NO_STUB)

static wchar_t folder[MAX_PATH];
static BOOL multithreaded_device;
static INIT_ONCE host_once = INIT_ONCE_STATIC_INIT;
static WaitReadyFn host_wait_ready;

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

static BOOL CALLBACK load_host(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once, (void)parameter, (void)context;
    // Any other process that loads this file (the HTML process imports d3d11.dll) gets the forwarding only.
    if (!GetModuleHandleW(ENGINE_MODULE)) {
        log_line("proxy loaded outside the game; forwarding only");
        return TRUE;
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%ls", folder, HOST_FILE);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        log_line("no %ls next to the proxy; forwarding only", HOST_FILE);
        return TRUE;
    }
    HMODULE host = LoadLibraryW(path);
    if (!host) log_line("loading %ls failed: error %lu", HOST_FILE, GetLastError());
    else host_wait_ready = (WaitReadyFn)(void *)GetProcAddress(host, "s2prof_wait_ready");
    return TRUE;
}

static void wait_for_host(void) {
    InitOnceExecuteOnce(&host_once, load_host, NULL, NULL);
    if (host_wait_ready && !host_wait_ready(HOST_WAIT_MS))
        log_line("the fixes were not installed after %d s; the device is created without waiting", HOST_WAIT_MS / 1000);
}

static UINT device_flags(UINT flags) {
    return multithreaded_device ? flags & ~D3D11_CREATE_DEVICE_SINGLETHREADED : flags;
}

// Exported under the system DLL's names through d3d11.def, so they keep undecorated symbols, as the stubs have.
HRESULT WINAPI proxy_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
                                       const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count, UINT sdk_version, ID3D11Device **device,
                                       D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context) __asm__("_proxy_D3D11CreateDevice");
HRESULT WINAPI proxy_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
                                                   const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count, UINT sdk_version,
                                                   const DXGI_SWAP_CHAIN_DESC *swap_chain_desc, IDXGISwapChain **swap_chain, ID3D11Device **device,
                                                   D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context)
    __asm__("_proxy_D3D11CreateDeviceAndSwapChain");

HRESULT WINAPI proxy_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
                                       const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count, UINT sdk_version, ID3D11Device **device,
                                       D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context) {
    wait_for_host();
    HRESULT result = ((CreateDeviceFn)real_D3D11CreateDevice)(adapter, driver_type, software, device_flags(flags), feature_levels, feature_level_count,
                                                              sdk_version, device, feature_level, context);
    log_line("D3D11CreateDevice: flags 0x%x passed as 0x%x -> 0x%lx", flags, device_flags(flags), result);
    return result;
}

HRESULT WINAPI proxy_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
                                                   const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count, UINT sdk_version,
                                                   const DXGI_SWAP_CHAIN_DESC *swap_chain_desc, IDXGISwapChain **swap_chain, ID3D11Device **device,
                                                   D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context) {
    wait_for_host();
    HRESULT result = ((CreateDeviceAndSwapChainFn)real_D3D11CreateDeviceAndSwapChain)(adapter, driver_type, software, device_flags(flags), feature_levels,
                                                                                      feature_level_count, sdk_version, swap_chain_desc, swap_chain,
                                                                                      device, feature_level, context);
    log_line("D3D11CreateDeviceAndSwapChain: flags 0x%x passed as 0x%x -> 0x%lx", flags, device_flags(flags), result);
    return result;
}

// Starts the host as soon as the loader lock is free, so the fixes are usually installed before the first device call.
static DWORD WINAPI start_host(LPVOID unused) {
    (void)unused;
    InitOnceExecuteOnce(&host_once, load_host, NULL, NULL);
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
    multithreaded_device = GetPrivateProfileIntW(L"device", L"multithreaded_device", 0, path);
    log_line("proxy loaded; multithreaded_device=%d", multithreaded_device);
    HANDLE thread = CreateThread(NULL, 0, start_host, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    return TRUE;
}
