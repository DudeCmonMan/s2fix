#include "graphicsreset.h"
#include <stdint.h>
#include <stdio.h>

// The engine marks its device lost (engine +0x6995f) after a failed status check. As each frame starts, its frame
// render (0x1819ee180, called from the frame step at 0x1819dfebe; before the scene is drawn) recovers a lost device:
// with the device still working it releases what every resource manager holds (0x181a85a70), clears the mark, creates
// it all again (0x181a85950) and draws the frame; a device that is gone is created first. Marking the device lost is the
// engine's own way to ask for that rebuild, so a requested reset marks it inside that call, just before the frame render
// reads it (marked earlier, the frame step's callbacks and the presents a load makes as it goes would see a lost
// device), and shows the engine's loading screen while the rebuild runs, as its graphics options do when they apply a
// change. The screen's own shown byte can't tell whether it is up: the options show it and never hide it. The call is
// reached through a jump placed within rel32 reach of the engine. Listeners are told at the engine's releases of GPU
// resources (all of them, 0x181a85a70, or by kind, 0x181a85b10) and at its creation (0x181a85950), which the recovery,
// the graphics options and a shadow, depth-of-field or distortion change go through. A load's scene teardown releases
// through its own copy of that walk (0x181a5d7c0), which isn't followed: listeners hear only the load's creation. The
// release of all leaves no room for a jump before its first RIP-relative load, so it is replaced by the same walk.
#define ENGINE_GLOBAL_RVA 0x3457888
#define DEVICE_LOST 0x6995f
#define SCENE_SLOT (0x138 / 8)
#define WINDOW_RVA 0x3450228
#define LOADING_SCREEN 0x69928
#define FRAME_RENDER_RVA 0x19ee180
#define FRAME_RENDER_CALL_RVA 0x19dfebe
#define RELEASE_ALL_RVA 0x1a85a70
#define RELEASE_KIND_RVA 0x1a85b10
#define CREATE_RVA 0x1a85950
#define RESOURCES_READY_RVA 0x3457863
#define MANAGERS_RVA 0x3450da0
#define MANAGER_COUNT_RVA 0x3450da8
#define CALLBACKS_RVA 0x34591b8
#define CALLBACK_COUNT_RVA 0x34591b4
#define MANAGER_ACTIVE 0x8
#define MANAGER_RELEASE_SLOT (0x20 / 8)
#define CALLBACK_RELEASE_SLOT (0x8 / 8)
#define LOADING_SHOW_SLOT (0xa8 / 8)
#define LOADING_HIDE_SLOT (0xb0 / 8)
#define LOADING_SHOW_RVA 0x1a97680
#define LOADING_HIDE_RVA 0x1a690c0
#define DEVICE_GLOBAL_RVA 0x3450230
#define D3D11_DEVICE_VTABLE_RVA 0x23de040
// The device's status (0 working, 3 removed or gone, 4 another failure), as the engine's frame render reads it.
#define DEVICE_STATUS_SLOT (0x298 / 8)
#define MAX_LISTENERS 16
#define NEAR_BYTES 0x10000

typedef BYTE (*FrameRenderFn)(BYTE *engine_object, void *callback);
typedef void *(*SceneFn)(BYTE *engine_object);
typedef BYTE (*CreateFn)(int mask);
typedef void (*ReleaseKindFn)(int kind, int mask);
typedef void (*ManagerReleaseFn)(BYTE *manager, int mask);
typedef void (*CallbackReleaseFn)(BYTE *callback);
typedef void (*LoadingScreenFn)(BYTE *screen);
typedef int (*DeviceStatusFn)(BYTE *device);

static const BYTE frame_render_call[] = {0xe8, 0xbd, 0xe2, 0x00, 0x00};
static const BYTE release_all_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0x05, 0x27, 0xb3, 0x9c, 0x01};
static const BYTE release_kind_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20};
static const BYTE create_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00};
static GraphicsResetHost host;
static FrameRenderFn frame_render;
static BOOL reachable;
static void *unused_release_all;
static ReleaseKindFn original_release_kind;
static CreateFn original_create;
static struct {
    void (*before)(void);
    void (*after)(void);
} listeners[MAX_LISTENERS];
static volatile LONG listener_count;
static SRWLOCK listen_lock = SRWLOCK_INIT;
static const char *volatile requested_reason;
static volatile LONG requested;
// Main thread only: the running rebuild, and the reason it was asked for (NULL for the engine's own).
static const char *reason;
static BOOL rebuilding, screen_shown;
static LARGE_INTEGER started;

static BYTE *engine_object(void) {
    return *(BYTE **)(host.engine + ENGINE_GLOBAL_RVA);
}

// The loading screen the engine made at its start, shown or hidden through its own methods; FALSE if it is not the
// engine's own kind.
static BOOL loading_screen(BOOL show) {
    BYTE *screen = *(BYTE **)(engine_object() + LOADING_SCREEN);
    void **vtable = screen ? *(void ***)screen : NULL;
    void *method = vtable ? vtable[show ? LOADING_SHOW_SLOT : LOADING_HIDE_SLOT] : NULL;
    if (!method || method != host.engine + (show ? LOADING_SHOW_RVA : LOADING_HIDE_RVA)) return FALSE;
    ((LoadingScreenFn)method)(screen);
    return TRUE;
}

static void notify(BOOL before) {
    for (LONG i = 0; i < listener_count; i++) {
        void (*callback)(void) = before ? listeners[i].before : listeners[i].after;
        if (callback) callback();
    }
}

// The engine's walk: the managers last to first, each counted afresh as a release can drop one, then the callbacks.
static void release_all_hook(int mask) {
    notify(TRUE);
    BYTE *engine = host.engine;
    *(volatile BYTE *)(engine + RESOURCES_READY_RVA) = 0;
    uint64_t count = *(volatile uint64_t *)(engine + MANAGER_COUNT_RVA);
    for (uint32_t i = (uint32_t)count - 1; i < count; i--) {
        BYTE *manager = (*(BYTE ***)(engine + MANAGERS_RVA))[i];
        if (*(DWORD *)(manager + MANAGER_ACTIVE)) ((ManagerReleaseFn)(*(void ***)manager)[MANAGER_RELEASE_SLOT])(manager, mask);
        count = *(volatile uint64_t *)(engine + MANAGER_COUNT_RVA);
    }
    for (BYTE **at = *(BYTE ***)(engine + CALLBACKS_RVA); at != *(BYTE ***)(engine + CALLBACKS_RVA) + *(volatile DWORD *)(engine + CALLBACK_COUNT_RVA); at++)
        ((CallbackReleaseFn)(*(void ***)*at)[CALLBACK_RELEASE_SLOT])(*at);
}

static void release_kind_hook(int kind, int mask) {
    notify(TRUE);
    original_release_kind(kind, mask);
}

static BYTE create_hook(int mask) {
    BYTE result = original_create(mask);
    notify(FALSE);
    return result;
}

// The frame render's own test for recovering now: a scene, in a window that isn't minimised. A request waits otherwise,
// so the device is never marked lost while nothing would rebuild it.
static BOOL recovers_now(BYTE *engine) {
    return ((SceneFn)(*(void ***)engine)[SCENE_SLOT])(engine) && !IsIconic(*(HWND *)(host.engine + WINDOW_RVA));
}

static BYTE frame_render_hook(BYTE *engine, void *callback) {
    if (requested && !rebuilding && recovers_now(engine)) {
        InterlockedExchange(&requested, 0);
        reason = requested_reason;
        engine[DEVICE_LOST] = 1;
    }
    if (!engine[DEVICE_LOST]) return frame_render(engine, callback);
    if (!rebuilding) {
        rebuilding = TRUE;
        QueryPerformanceCounter(&started);
        if (reason) screen_shown = loading_screen(TRUE);
    }
    BYTE result = frame_render(engine, callback);
    // Still marked: the device could not be made, is lost, or the window is minimised; the engine tries again next frame.
    if (engine[DEVICE_LOST]) return result;
    if (screen_shown) loading_screen(FALSE);
    LARGE_INTEGER ended, frequency;
    QueryPerformanceCounter(&ended);
    QueryPerformanceFrequency(&frequency);
    host.log_line("graphics reset: %s: the engine rebuilt its graphics resources in %.0f ms%s",
                  reason ? reason : "the engine's own (a lost device)",
                  1000.0 * (double)(ended.QuadPart - started.QuadPart) / (double)frequency.QuadPart,
                  reason && !screen_shown ? " (without the loading screen: not the engine's own kind)" : "");
    rebuilding = screen_shown = FALSE;
    reason = NULL;
    return result;
}

// 64 KB in the first free spot above the engine's image, within reach of its rel32 offsets.
static BYTE *allocate_near(void) {
    IMAGE_NT_HEADERS64 *headers = (IMAGE_NT_HEADERS64 *)(host.engine + ((IMAGE_DOS_HEADER *)host.engine)->e_lfanew);
    uintptr_t at = ((uintptr_t)host.engine + headers->OptionalHeader.SizeOfImage + 0xffff) & ~(uintptr_t)0xffff;
    for (; at < (uintptr_t)host.engine + 0x7f000000; at += 0x10000) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)at, &info, sizeof info)) break;
        if (info.State != MEM_FREE || info.RegionSize < NEAR_BYTES) continue;
        BYTE *memory = VirtualAlloc((void *)at, NEAR_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory) return memory;
    }
    return NULL;
}

// jmp [rip+0] to frame_render_hook, then the frame step's call aimed at it.
static const char *reach_frame_render(void) {
    BYTE *jump = allocate_near();
    if (!jump) return "no memory free within reach of the engine";
    static const BYTE indirect_jump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(jump, indirect_jump, sizeof indirect_jump);
    void *target = (void *)frame_render_hook;
    memcpy(jump + sizeof indirect_jump, &target, sizeof target);
    DWORD protection;
    if (!VirtualProtect(jump, NEAR_BYTES, PAGE_EXECUTE_READ, &protection)) return "VirtualProtect failed";
    FlushInstructionCache(GetCurrentProcess(), jump, sizeof indirect_jump + sizeof target);
    BYTE *call = host.engine + FRAME_RENDER_CALL_RVA;
    BYTE call_jump[sizeof frame_render_call] = {0xe8};
    int32_t offset = (int32_t)(jump - (call + sizeof call_jump));
    memcpy(call_jump + 1, &offset, sizeof offset);
    return host.patch(call, frame_render_call, call_jump, sizeof call_jump);
}

const char *graphics_reset_init(const GraphicsResetHost *reset_host, const char *unavailable) {
    host = *reset_host;
    if (unavailable) return unavailable;
    frame_render = (FrameRenderFn)(void *)(host.engine + FRAME_RENDER_RVA);
    const char *error = host.hook(host.engine + RELEASE_ALL_RVA, release_all_prologue, sizeof release_all_prologue, (const void *)release_all_hook,
                                  &unused_release_all);
    if (!error)
        error = host.hook(host.engine + RELEASE_KIND_RVA, release_kind_prologue, sizeof release_kind_prologue, (const void *)release_kind_hook,
                          (void **)&original_release_kind);
    if (!error) error = host.hook(host.engine + CREATE_RVA, create_prologue, sizeof create_prologue, (const void *)create_hook, (void **)&original_create);
    if (!error) error = reach_frame_render();
    reachable = !error;
    return error;
}

void graphics_reset_request(const char *why) {
    if (!reachable) {
        host.log_line("graphics reset: %s: unavailable this session", why);
        return;
    }
    requested_reason = why;
    InterlockedExchange(&requested, 1);
    host.log_line("graphics reset: %s: asked for; it runs at the next frame", why);
}

void graphics_reset_listen(void (*before)(void), void (*after)(void)) {
    AcquireSRWLockExclusive(&listen_lock);
    LONG count = listener_count;
    if (count < MAX_LISTENERS) {
        listeners[count].before = before;
        listeners[count].after = after;
        InterlockedExchange(&listener_count, count + 1);
    }
    ReleaseSRWLockExclusive(&listen_lock);
    if (count == MAX_LISTENERS) host.log_line("graphics reset: more than %d listeners; one is not told", MAX_LISTENERS);
}

void graphics_reset_describe(char *out, size_t size) {
    BYTE *engine = host.engine ? engine_object() : NULL;
    BYTE *device = host.engine ? *(BYTE *volatile *)(host.engine + DEVICE_GLOBAL_RVA) : NULL;
    if (!engine) {
        snprintf(out, size, "the engine is not running");
        return;
    }
    int status = -1;
    if (device && *(BYTE **)device == host.engine + D3D11_DEVICE_VTABLE_RVA) status = ((DeviceStatusFn)(*(void ***)device)[DEVICE_STATUS_SLOT])(device);
    snprintf(out, size, "the engine's device-lost mark %s; a rebuild %s; a request %s; the device's status %d (0 working, 3 removed or gone, 4 another "
                        "failure, -1 not a D3D11 device); the reset %s",
             engine[DEVICE_LOST] ? "set" : "clear", rebuilding ? "running" : "not running", requested ? "waiting" : "none", status,
             reachable ? "available" : "unavailable");
}
