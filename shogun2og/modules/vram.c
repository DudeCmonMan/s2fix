#define COBJMACROS
#include "../s2module.h"
#include "../checks/vram.h"
#include <dxgi.h>
#include <stdint.h>
#include <wchar.h>

// The engine's video memory total (0x113f1360, engine vtable +0x158) is the texture manager's budget (over it, textures
// lose their top mips and quality settings are lowered), sizes the imposter and unit-model pools, and is what the options
// screen shows. It is 95% of DXGI's DedicatedVideoMemory, which DXGI caps at 3 GiB for a 32-bit process: 2918 MB on a
// 24 GB card. This answers with the adapter's own local video memory budget instead (IDXGIAdapter3, 64-bit even here),
// clamped to the most the engine's 32-bit fields hold, whenever that is more than the engine's figure. Every user of the
// total checked on the disassembly treats it as unsigned up to 4 GiB - 1: the texture budget (0x113df9d6, it subtracts),
// the pools (0x113f5865, through double), the thresholds (0x113911f3, 0x113f9134) and the display (0x111d0c2f). The
// first readers are logged, so new ones can be checked. A total the texture manager pinned after a real allocation
// failure (engine +0x69ce0) is kept. Read at start: [vram] enabled=0 in s2prof.ini keeps the game's figure.
#define TOTAL_RVA 0x13f1360
#define DEVICE_GLOBAL_RVA 0x1c9a93c
#define D3D11_DEVICE_VTABLE_RVA 0x17666f8
#define ADAPTER_OFFSET 0x170c
#define PINNED_TOTAL_OFFSET 0x69ce0
// 1 MB under the 32-bit limit, so a reader that adds a little to the total cannot wrap it.
#define ENGINE_MAXIMUM 0xfff00000u
#define QUERY_VIDEO_MEMORY_SLOT 14
#define LOGGED_READERS 32

typedef struct {
    UINT64 budget, usage, available_for_reservation, reservation;
} VideoMemoryInfo;
typedef HRESULT(STDMETHODCALLTYPE *QueryVideoMemoryFn)(void *adapter, UINT node, int group, VideoMemoryInfo *info);
typedef uint32_t(__attribute__((thiscall)) *TotalFn)(BYTE *engine_object);

static const GUID adapter3_iid = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};
static const BYTE total_prologue[] = {0x57, 0x8b, 0xf9, 0x8b, 0x87, 0xdc, 0x9c, 0x06, 0x00};
static const S2Api *api;
static TotalFn original_total;
static volatile LONG unlocked_total, logged_total;
static volatile LONG readers[LOGGED_READERS];

// The adapter the engine chose, once its device has one: its local budget, clamped. 0 until it can be read.
static uint32_t adapter_total(void) {
    BYTE *device = *(BYTE *volatile *)(api->engine + DEVICE_GLOBAL_RVA);
    if (!device || *(BYTE **)device != api->engine + D3D11_DEVICE_VTABLE_RVA) return 0;
    IDXGIAdapter *adapter = *(IDXGIAdapter **)(device + ADAPTER_OFFSET);
    void *adapter3 = NULL;
    if (!adapter || FAILED(IDXGIAdapter_QueryInterface(adapter, &adapter3_iid, &adapter3))) return 0;
    VideoMemoryInfo local = {0};
    HRESULT result = ((QueryVideoMemoryFn)(*(void ***)adapter3)[QUERY_VIDEO_MEMORY_SLOT])(adapter3, 0, 0, &local);
    ((IUnknown *)adapter3)->lpVtbl->Release((IUnknown *)adapter3);
    if (FAILED(result) || !local.budget) return 0;
    return local.budget > ENGINE_MAXIMUM ? ENGINE_MAXIMUM : (uint32_t)local.budget;
}

static void log_reader(void *return_address) {
    LONG file_address = (LONG)((BYTE *)return_address - api->engine + 0x10000000);
    for (int i = 0; i < LOGGED_READERS; i++) {
        LONG seen = readers[i];
        if (seen == file_address) return;
        if (!seen && InterlockedCompareExchange(&readers[i], file_address, 0) == 0) {
            api->log_line("vram: the total is read from 0x%08lx", (unsigned long)file_address);
            return;
        }
    }
}

static uint32_t __attribute__((thiscall)) total_hook(BYTE *engine_object) {
    log_reader(__builtin_return_address(0));
    uint32_t engine_total = original_total(engine_object), pinned = *(uint32_t *)(engine_object + PINNED_TOTAL_OFFSET);
    if (pinned) return engine_total;
    uint32_t total = (uint32_t)unlocked_total;
    if (!total && (total = adapter_total())) InterlockedExchange(&unlocked_total, (LONG)total);
    if (total && !InterlockedExchange(&logged_total, 1))
        api->log_line("vram: the game's video memory total is %lu MB (the adapter's local budget, at most %lu MB) instead of the %lu MB it computed",
                      (unsigned long)((total > engine_total ? total : engine_total) >> 20), (unsigned long)(ENGINE_MAXIMUM >> 20),
                      (unsigned long)(engine_total >> 20));
    return total > engine_total ? total : engine_total;
}

// s2prof.ini sits next to the game, two folders up from fixes\vram.dll.
static BOOL enabled_in_ini(void) {
    wchar_t path[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)enabled_in_ini, &self);
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return TRUE;
    for (int up = 0; up < 2; up++) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) *slash = 0;
    }
    wcscat(path, L"\\s2prof.ini");
    return GetPrivateProfileIntW(L"vram", L"enabled", 1, path) != 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    if (!enabled_in_ini()) return "off ([vram] enabled=0 in s2prof.ini; the game keeps its own video memory total)";
    const char *mismatch = S2_CHECK_ENGINE(host, vram_checks);
    if (mismatch) return mismatch;
    return api->hook(api->engine + TOTAL_RVA, total_prologue, sizeof total_prologue, (const void *)total_hook, (void **)&original_total);
}
