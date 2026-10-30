#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#include "../s2module.h"
#include "../checks/flipmodel.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_3.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

// The game's windowed swap chain is the 2011 blt model (DXGI_SWAP_EFFECT_DISCARD, one buffer): every Present copies the
// frame through the desktop compositor, and the copy stalls main for ~20 ms two or three times a second in a battle (with
// or without the fixes). This creates it as a flip-model chain instead (FLIP_DISCARD, 3 buffers, tearing allowed when
// the system supports it), which a borderless full-screen window can present without the compositor's copy. The swap
// chain is made once, as the device is created, so this takes effect from the next start.
// Remove it by deleting fixes\flipmodel.dll or with [flipmodel] enabled=0 in s2prof.ini.
//
// - Creation: 0x1147c050 fills the description at device+0xc0 and calls factory->CreateSwapChain through ecx; the two
//   instructions loading ecx (0x1147c123) call a stub that loads the wrapper instead.
// - A display change tears the device and its swap chain down (0x114677f0) and creates both again. A window holds one
//   flip-model chain, so this module's own reference to the chain goes in that teardown, with the engine's. If the new
//   chain still can't be made, the player is told to restart and the game closes: there is no fallback to the blt model.
// - Present: the device's present (0x11487180, vtbl+0xfc) calls swap_chain->Present(sync, 0); with tearing allowed and a
//   sync interval of 0 it needs DXGI_PRESENT_ALLOW_TEARING. Flip-model Present also unbinds the swap chain's buffers
//   from the pipeline, so the targets bound before it are bound again after it, as the engine's state cache has them.
// - ResizeBuffers keeps the flip chain's buffer count and flags.
// - Frame latency: the engine never sets one, so DXGI queues up to 3 frames and Present blocks once the queue is full.
//   With [flipmodel] latency=N the chain is made waitable; after each Present main waits until the queue has room for
//   the next frame, at most N frames queued. A GPU spike then shows as that wait instead of a stall inside Present, and
//   input latency drops. The latency switch (and Apply Fixes) A/Bs it against DXGI's 3 without the wait; the exported
//   frame_latency can be patched live.
#define CREATE_CALL_RVA 0x147c123
#define DEVICE_TEARDOWN_RVA 0x14677f0
#define PRESENT_RVA 0x1487180
#define HRESULT_CHECK_RVA 0x13e0ef0
#define RENDER_DEVICE_RVA 0x1c9a93c
#define SWAP_CHAIN_OFFSET 0xbc
#define SYNC_FLAG_OFFSET 0x1bf1
#define RENDERER_CONTEXT_OFFSET 0x108
#define RESIZE_BUFFERS_SLOT 13
#define FLIP_DISCARD 4
#define FLAG_ALLOW_TEARING 2048
#define PRESENT_ALLOW_TEARING 0x200
#define FEATURE_PRESENT_ALLOW_TEARING 0
#define DXGI_DEFAULT_LATENCY 3
#define MAX_LATENCY 16
// Long enough for the slowest frames seen (~100 ms while the camera moves), short enough that a missed signal can't hang.
#define LATENCY_WAIT_MS 250
#define CREATE_FAILED_MESSAGE                                                                                                 \
    L"Failed to initialize d3d11 device after settings change, please restart the game. This can happen when changing " \
    L"settings. Should be fine after restart."

typedef BYTE(__attribute__((thiscall)) *PresentFn)(BYTE *device, void *a, void *b);
typedef BYTE (*HresultCheckFn)(HRESULT result);
typedef void(__attribute__((thiscall)) *DeviceTeardownFn)(BYTE *render_device);
typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainFn)(IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swap_chain);
typedef HRESULT(STDMETHODCALLTYPE *ResizeBuffersFn)(IDXGISwapChain *swap_chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags);
// IDXGIFactory5::CheckFeatureSupport is slot 28 (after IDXGIFactory4's 27 slots); mingw's dxgi.h lacks the interface.
typedef HRESULT(STDMETHODCALLTYPE *CheckFeatureSupportFn)(void *factory, UINT feature, void *data, UINT size);
static const GUID factory5_iid = {0x7632e1f5, 0xee65, 0x4dca, {0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d}};
static const GUID swap_chain2_iid = {0xa8be2ac4, 0x199f, 0x4946, {0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7}};

static const BYTE create_call_expected[] = {0x8b, 0x02, 0x8b, 0x48, 0x28};
static const BYTE present_prologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x96, 0xbc, 0x00, 0x00, 0x00};
static const BYTE device_teardown_prologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x8e, 0x6c, 0x19, 0x00, 0x00};
static const S2Api *api;
static PresentFn original_present;
static DeviceTeardownFn original_device_teardown;
static ResizeBuffersFn original_resize_buffers;
static int buffer_count = 3, tearing_wanted = 1, latency_wanted = 2;
static BOOL flip_active, tearing_active;
static UINT flip_flags;

__declspec(dllexport) volatile LONG latency_enabled = 1;
__declspec(dllexport) volatile LONG frame_latency = 2;

static IDXGISwapChain2 *latency_chain;
static HANDLE latency_event;
static UINT applied_latency;

HRESULT STDMETHODCALLTYPE create_swap_chain(IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swap_chain);

// At 0x1147c123, in place of "mov eax,[edx]; mov ecx,[eax+0x28]" (edx: the factory): the same eax, and ecx the wrapper.
void create_call_stub(void);
__asm__(".text\n"
        ".globl _create_call_stub\n"
        "_create_call_stub:\n"
        "    movl (%edx), %eax\n"
        "    movl $_create_swap_chain@16, %ecx\n"
        "    ret\n");

static BOOL tearing_supported(IDXGIFactory *factory) {
    void *factory5 = NULL;
    if (FAILED(IDXGIFactory_QueryInterface(factory, &factory5_iid, &factory5))) return FALSE;
    BOOL allowed = FALSE;
    CheckFeatureSupportFn check = (CheckFeatureSupportFn)(*(void ***)factory5)[28];
    HRESULT result = check(factory5, FEATURE_PRESENT_ALLOW_TEARING, &allowed, sizeof allowed);
    IUnknown_Release((IUnknown *)factory5);
    return SUCCEEDED(result) && allowed;
}

static BOOL flip_format(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

static HRESULT STDMETHODCALLTYPE resize_buffers(IDXGISwapChain *swap_chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    if (flip_active) {
        count = 0;
        flags |= flip_flags;
    }
    return original_resize_buffers(swap_chain, count, width, height, format, flags);
}

static void hook_resize_buffers(IDXGISwapChain *swap_chain) {
    void **slot = &(*(void ***)swap_chain)[RESIZE_BUFFERS_SLOT];
    if (*slot == (void *)resize_buffers) return;
    DWORD protection;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protection)) {
        api->log_line("flipmodel: VirtualProtect of the swap chain's vtable failed (%lu); ResizeBuffers keeps the engine's arguments", GetLastError());
        return;
    }
    original_resize_buffers = (ResizeBuffersFn)*slot;
    *slot = (void *)resize_buffers;
    VirtualProtect(slot, sizeof *slot, protection, &protection);
}

static void release_latency(void) {
    if (latency_chain) IDXGISwapChain2_Release(latency_chain);
    if (latency_event) CloseHandle(latency_event);
    latency_chain = NULL;
    latency_event = NULL;
    applied_latency = 0;
}

static void setup_latency(IDXGISwapChain *swap_chain) {
    if (!(flip_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) return;
    if (FAILED(IDXGISwapChain_QueryInterface(swap_chain, &swap_chain2_iid, (void **)&latency_chain))) {
        latency_chain = NULL;
        api->log_line("flipmodel: the swap chain has no IDXGISwapChain2; no frame latency wait");
        return;
    }
    latency_event = IDXGISwapChain2_GetFrameLatencyWaitableObject(latency_chain);
    if (!latency_event) {
        IDXGISwapChain2_Release(latency_chain);
        latency_chain = NULL;
        api->log_line("flipmodel: no frame latency waitable object; no frame latency wait");
        return;
    }
    api->log_line("flipmodel: frame latency %ld with a wait after each Present (the latency switch compares DXGI's %d without it)", (long)frame_latency,
                  DXGI_DEFAULT_LATENCY);
}

// On main after each Present, so the next frame starts once the queue has room for it.
static void wait_for_frame_room(void) {
    BOOL waiting = *api->fix_enabled && latency_enabled;
    LONG latency = frame_latency < 1 ? 1 : frame_latency > MAX_LATENCY ? MAX_LATENCY : frame_latency;
    UINT wanted = waiting ? (UINT)latency : DXGI_DEFAULT_LATENCY;
    if (wanted != applied_latency && SUCCEEDED(IDXGISwapChain2_SetMaximumFrameLatency(latency_chain, wanted))) applied_latency = wanted;
    if (!waiting) return;
    WaitForSingleObject(latency_event, LATENCY_WAIT_MS);
}

// The engine can't go on without its swap chain, and a restart makes a fresh device.
static void close_game(HRESULT result) {
    api->log_line("flipmodel: CreateSwapChain with the flip model failed (0x%08lx); the game is closed", (unsigned long)result);
    MessageBoxW(NULL, CREATE_FAILED_MESSAGE, L"Shogun 2 fixes", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    TerminateProcess(GetCurrentProcess(), 1);
}

HRESULT STDMETHODCALLTYPE create_swap_chain(IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swap_chain) {
    CreateSwapChainFn create = (CreateSwapChainFn)(*(void ***)factory)[10];
    flip_active = FALSE;
    if (!desc->Windowed || desc->SampleDesc.Count != 1 || !flip_format(desc->BufferDesc.Format)) {
        api->log_line("flipmodel: kept the blt model (windowed %d, %u samples, format %d)", desc->Windowed, desc->SampleDesc.Count, desc->BufferDesc.Format);
        return create(factory, device, desc, swap_chain);
    }
    DXGI_SWAP_CHAIN_DESC flip = *desc;
    tearing_active = tearing_wanted && tearing_supported(factory);
    flip_flags = (tearing_active ? FLAG_ALLOW_TEARING : 0) | (latency_wanted ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
    flip.SwapEffect = (DXGI_SWAP_EFFECT)FLIP_DISCARD;
    flip.BufferCount = (UINT)buffer_count;
    flip.Flags = desc->Flags | flip_flags;
    HRESULT result = create(factory, device, &flip, swap_chain);
    if (FAILED(result)) close_game(result);
    flip_active = TRUE;
    hook_resize_buffers(*swap_chain);
    setup_latency(*swap_chain);
    api->log_line("flipmodel: %ux%u flip-model swap chain, %d buffers, tearing %s", flip.BufferDesc.Width, flip.BufferDesc.Height, buffer_count,
                  tearing_active ? "allowed" : tearing_wanted ? "not supported" : "off");
    return result;
}

static void __attribute__((thiscall)) device_teardown_hook(BYTE *render_device) {
    release_latency();
    original_device_teardown(render_device);
}

static BYTE __attribute__((thiscall)) present_hook(BYTE *device, void *a, void *b) {
    IDXGISwapChain *swap_chain = *(IDXGISwapChain **)(device + SWAP_CHAIN_OFFSET);
    if (!flip_active || !swap_chain) return original_present(device, a, b);
    UINT sync = device[SYNC_FLAG_OFFSET] != 0, flags = 0;
    if (!sync && tearing_active) {
        BOOL fullscreen = FALSE;
        IDXGISwapChain_GetFullscreenState(swap_chain, &fullscreen, NULL);
        if (!fullscreen) flags = PRESENT_ALLOW_TEARING;
    }
    ID3D11DeviceContext *context = *(ID3D11DeviceContext **)(device + RENDERER_CONTEXT_OFFSET);
    ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {0};
    ID3D11DepthStencilView *depth = NULL;
    if (context) ID3D11DeviceContext_OMGetRenderTargets(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
    HRESULT result = IDXGISwapChain_Present(swap_chain, sync, flags);
    if (context) {
        ID3D11DeviceContext_OMSetRenderTargets(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
        for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
            if (targets[i]) ID3D11RenderTargetView_Release(targets[i]);
        if (depth) ID3D11DepthStencilView_Release(depth);
    }
    if (latency_chain && result != DXGI_STATUS_OCCLUDED) wait_for_frame_room();
    return ((HresultCheckFn)(api->engine + HRESULT_CHECK_RVA))(result);
}

// s2prof.ini sits next to the game, two folders up from fixes\flipmodel.dll.
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
    if (!GetPrivateProfileIntW(L"flipmodel", L"enabled", 1, path)) buffer_count = 0;
    else buffer_count = GetPrivateProfileIntW(L"flipmodel", L"buffers", 3, path);
    tearing_wanted = GetPrivateProfileIntW(L"flipmodel", L"tearing", 1, path) != 0;
    latency_wanted = GetPrivateProfileIntW(L"flipmodel", L"latency", 2, path);
    if (latency_wanted < 0) latency_wanted = 0;
    if (latency_wanted > MAX_LATENCY) latency_wanted = MAX_LATENCY;
    if (latency_wanted) frame_latency = latency_wanted;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    read_settings();
    if (!buffer_count) return "off ([flipmodel] enabled=0 in s2prof.ini)";
    const char *mismatch = S2_CHECK_ENGINE(host, flipmodel_checks);
    if (mismatch) return mismatch;
    if (buffer_count < 2) buffer_count = 2;
    if (buffer_count > 4) buffer_count = 4;
    BYTE *device = *(BYTE **)(api->engine + RENDER_DEVICE_RVA);
    if (device && *(void **)(device + SWAP_CHAIN_OFFSET)) return "the swap chain already exists; it is made with the device, so this takes effect from the next start";
    BYTE call[5] = {0xe8};
    int32_t offset = (int32_t)((BYTE *)(void *)create_call_stub - (api->engine + CREATE_CALL_RVA + 5));
    memcpy(call + 1, &offset, 4);
    const char *error = api->patch(api->engine + CREATE_CALL_RVA, create_call_expected, call, sizeof call);
    if (!error) error = api->hook(api->engine + PRESENT_RVA, present_prologue, sizeof present_prologue, (const void *)present_hook, (void **)&original_present);
    if (!error)
        error = api->hook(api->engine + DEVICE_TEARDOWN_RVA, device_teardown_prologue, sizeof device_teardown_prologue, (const void *)device_teardown_hook,
                          (void **)&original_device_teardown);
    if (!error && latency_wanted) api->register_switch("latency", &latency_enabled, NULL);
    return error;
}
