#include "graphicsreset.h"
#include <stdio.h>

// A reset sets the engine's reset flag, its own way to ask for a rebuild, just as its recovery reads it: set earlier, it
// would stop a load's presents. Listeners hear the one release and creation every rebuild goes through.
#define ENGINE_GLOBAL_RVA 0x1c9a920
#define RESET_FLAG 0x69872
#define LOADING_SCREEN 0x69848
#define RECOVERY_RVA 0x13f86d0
#define RELEASE_RVA 0x140fcc0
#define CREATE_RVA 0x140fbc0
#define LOADING_SHOW_SLOT (0x50 / 4)
#define LOADING_HIDE_SLOT (0x54 / 4)
#define LOADING_SHOW_RVA 0x141fa00
#define LOADING_HIDE_RVA 0x13ecde0
#define DEVICE_GLOBAL_RVA 0x1c9a93c
#define D3D11_DEVICE_VTABLE_RVA 0x17666f8
// The device's status, as the engine's present path reads it.
#define DEVICE_STATUS_SLOT (0x104 / 4)
#define MAX_LISTENERS 16
// Back-to-back rebuilds, from a held key or a module asking again, can leave the Complete Edition's shadows flickering.
#define MIN_GAP_MS 5000

typedef BYTE(__attribute__((thiscall)) *RecoveryFn)(BYTE *engine_object);
typedef DWORD(__cdecl *ReleaseFn)(int first, int mask);
typedef DWORD(__cdecl *CreateFn)(DWORD argument);
typedef void(__attribute__((thiscall)) *LoadingScreenFn)(BYTE *screen);
typedef int(__attribute__((thiscall)) *DeviceStatusFn)(BYTE *device);

static const BYTE recovery_prologue[] = {0x56, 0x8b, 0xf1, 0x80, 0xbe, 0x72, 0x98, 0x06, 0x00, 0x00};
static const BYTE release_prologue[] = {0x8b, 0x15, 0x8c, 0xae, 0xc9, 0x11, 0x53};
static const BYTE create_prologue[] = {0x83, 0xec, 0x58, 0x8d, 0x0c, 0x24};
static GraphicsResetHost host;
static RecoveryFn original_recovery;
static ReleaseFn original_release;
static CreateFn original_create;
static struct {
    void (*before)(void);
    void (*after)(void);
} listeners[MAX_LISTENERS];
static volatile LONG listener_count;
static SRWLOCK listen_lock = SRWLOCK_INIT;
static const char *volatile requested_reason;
static volatile LONG requested;
// Main thread only; NULL for the engine's own rebuild.
static const char *reason;
static BOOL rebuilding, screen_shown;
static LARGE_INTEGER started;
static volatile DWORD last_rebuild_end;
static volatile LONG rebuilt_once;

static BYTE *engine_object(void) {
    return *(BYTE **)(host.engine + ENGINE_GLOBAL_RVA);
}

// FALSE if the loading screen is not the engine's own kind.
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

static DWORD __cdecl release_hook(int first, int mask) {
    notify(TRUE);
    return original_release(first, mask);
}

static DWORD __cdecl create_hook(DWORD argument) {
    DWORD result = original_create(argument);
    notify(FALSE);
    return result;
}

static BYTE __attribute__((thiscall)) recovery_hook(BYTE *engine) {
    if (requested && !rebuilding) {
        InterlockedExchange(&requested, 0);
        reason = requested_reason;
        engine[RESET_FLAG] = 1;
    }
    if (!engine[RESET_FLAG]) return original_recovery(engine);
    if (!rebuilding) {
        rebuilding = TRUE;
        QueryPerformanceCounter(&started);
        if (reason) screen_shown = loading_screen(TRUE);
    }
    BYTE result = original_recovery(engine);
    // Still flagged: the device could not be made or is lost; the engine tries again next frame.
    if (engine[RESET_FLAG]) return result;
    if (screen_shown) loading_screen(FALSE);
    LARGE_INTEGER ended, frequency;
    QueryPerformanceCounter(&ended);
    QueryPerformanceFrequency(&frequency);
    host.log_line("graphics reset: %s: the engine rebuilt its graphics resources in %.0f ms%s",
                  reason ? reason : "the engine's own (a window resize or a failed present)",
                  1000.0 * (double)(ended.QuadPart - started.QuadPart) / (double)frequency.QuadPart,
                  reason && !screen_shown ? " (without the loading screen: not the engine's own kind)" : "");
    rebuilding = screen_shown = FALSE;
    last_rebuild_end = GetTickCount();
    InterlockedExchange(&rebuilt_once, 1);
    reason = NULL;
    return result;
}

const char *graphics_reset_init(const GraphicsResetHost *reset_host, const char *unavailable) {
    host = *reset_host;
    if (unavailable) return unavailable;
    const char *error = host.hook(host.engine + RELEASE_RVA, release_prologue, sizeof release_prologue, (const void *)release_hook, (void **)&original_release);
    if (!error) error = host.hook(host.engine + CREATE_RVA, create_prologue, sizeof create_prologue, (const void *)create_hook, (void **)&original_create);
    if (!error)
        error = host.hook(host.engine + RECOVERY_RVA, recovery_prologue, sizeof recovery_prologue, (const void *)recovery_hook, (void **)&original_recovery);
    return error;
}

BOOL graphics_reset_allowed(const char *why) {
    if (!original_recovery) {
        host.log_line("graphics reset: %s: unavailable this session", why);
        return FALSE;
    }
    if (requested || rebuilding) {
        host.log_line("graphics reset: %s: not run, one is already asked for or running", why);
        return FALSE;
    }
    DWORD since = GetTickCount() - last_rebuild_end;
    if (rebuilt_once && since < MIN_GAP_MS) {
        host.log_line("graphics reset: %s: not run, %.1f s after the last one (resets are at least %d s apart)", why, since / 1000.0, MIN_GAP_MS / 1000);
        return FALSE;
    }
    return TRUE;
}

void graphics_reset_request(const char *why) {
    if (!graphics_reset_allowed(why)) return;
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
    snprintf(out, size, "the engine's reset flag %s; a rebuild %s; a request %s; the device's status %d (0 working, 3 removed or gone, 4 another "
                        "failure, -1 not a D3D11 device); the reset %s",
             engine[RESET_FLAG] ? "set" : "clear", rebuilding ? "running" : "not running", requested ? "waiting" : "none", status,
             original_recovery ? "available" : "unavailable");
}
