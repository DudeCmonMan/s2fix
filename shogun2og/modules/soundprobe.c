#include "../s2module.h"
#include "../checks/soundprobe.h"
#include <objbase.h>

// The engine's sound start-up (on the main thread, before the packs load) starts Miles and finds which speaker layouts
// work by opening and closing the audio driver once for each of 12 (0x10f5dc90), then opens the first that works for
// good (the rest of 0x10f5cb30). The probes mostly wait on the audio driver, and nothing uses the sound system until
// sound part 2 (0x10095eb0), after the packs and the database have loaded. Here Miles' start-up and the probes run on a
// helper thread, with the main thread's floating-point modes and COM apartment type, and the main thread opens the
// driver as the engine does when part 2 starts. The shutdown the engine registers after part 1 waits for the helper
// too, in case start-up ends first.
#define START_CALL_RVA 0xf5bfd5
#define SOUND_START_RVA 0xf5cb30
#define MILES_START_RVA 0xf5dc90
#define DRIVER_OPEN_RVA 0xf5cb90
#define PART2_RVA 0x95eb0
#define SHUTDOWN_PUSH_RVA 0x9d5f1
#define SOUND_SHUTDOWN_RVA 0x93230
#define LAYOUT_COUNT 0x3c
#define LAYOUTS 0x40
#define LAYOUT_SIZE 16
#define LAST_START_CALL_SLOT (0x120 / 4)
#define DRIVER_RATE 44100
#define DRIVER_BITS 16

typedef BYTE(__attribute__((thiscall)) *SoundFn)(BYTE *sound);
typedef BYTE(__attribute__((thiscall)) *DriverOpenFn)(BYTE *sound, BYTE *layout, DWORD rate, DWORD bits);
typedef void(__attribute__((thiscall)) *LastStartCallFn)(BYTE *sound, DWORD which, DWORD on);
typedef void(__cdecl *Part2Fn)(DWORD a, void *b);
typedef void(__cdecl *ShutdownFn)(void);
typedef HRESULT(WINAPI *ApartmentTypeFn)(APTTYPE *type, APTTYPEQUALIFIER *qualifier);
typedef HRESULT(WINAPI *ComInitFn)(LPVOID reserved, DWORD mode);
typedef void(WINAPI *ComUninitFn)(void);

// 0 runs the engine's sound start-up on the main thread; read when the game starts its sound.
__declspec(dllexport) volatile LONG probe_enabled = 1;

static const S2Api *api;
static SoundFn engine_start, miles_start;
static DriverOpenFn driver_open;
static Part2Fn original_part2;
static ShutdownFn sound_shutdown;
static ApartmentTypeFn get_apartment_type;
static ComInitFn com_init;
static ComUninitFn com_uninit;
static const BYTE start_call_original[5] = {0xe8, 0x56, 0x0b, 0x00, 0x00};
static BYTE start_call_replacement[5];
static const BYTE part2_prologue[10] = {0x83, 0xec, 0x70, 0x83, 0x3d, 0x70, 0x75, 0x7c, 0x11, 0x00};
static const BYTE shutdown_push_original[5] = {0x68, 0x30, 0x32, 0x09, 0x10};
static BYTE shutdown_push_replacement[5];

static HANDLE helper;
static BYTE *helper_sound;
static WORD main_x87;
static DWORD main_mxcsr;
static BOOL main_in_apartment;
static DWORD main_apartment_mode;
static BYTE miles_started;
static double helper_ms;

static double now_ms(void) {
    static LARGE_INTEGER frequency;
    LARGE_INTEGER count;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static DWORD WINAPI probe_thread(LPVOID unused) {
    (void)unused;
    __asm__ volatile("fldcw %0" : : "m"(main_x87));
    __asm__ volatile("ldmxcsr %0" : : "m"(main_mxcsr));
    BOOL in_apartment = main_in_apartment && SUCCEEDED(com_init(NULL, main_apartment_mode));
    double started = now_ms();
    miles_started = miles_start(helper_sound);
    helper_ms = now_ms() - started;
    if (in_apartment) com_uninit();
    return 0;
}

static BYTE __attribute__((thiscall)) start_hook(BYTE *sound) {
    if (!probe_enabled || !*api->fix_enabled) return engine_start(sound);
    __asm__ volatile("fnstcw %0" : "=m"(main_x87));
    __asm__ volatile("stmxcsr %0" : "=m"(main_mxcsr));
    APTTYPE type = APTTYPE_STA;
    APTTYPEQUALIFIER qualifier;
    main_in_apartment = get_apartment_type(&type, &qualifier) == S_OK;
    main_apartment_mode = type == APTTYPE_MTA ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED;
    helper_sound = sound;
    helper = CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
    return helper ? 1 : engine_start(sound);
}

// Sent messages are handled while waiting, in case the audio driver sends one to the game's window.
static void finish(void) {
    if (!helper) return;
    double started = now_ms();
    while (MsgWaitForMultipleObjects(1, &helper, FALSE, INFINITE, QS_SENDMESSAGE) != WAIT_OBJECT_0) {
        MSG message;
        PeekMessageW(&message, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    double waited = now_ms() - started;
    CloseHandle(helper);
    helper = NULL;
    if (!miles_started) {
        api->log_line("soundprobe: Miles found no audio driver it can open (%.0f ms on a helper); the game goes on without sound, as it would have",
                      helper_ms);
        sound_shutdown();
        return;
    }
    BYTE *sound = helper_sound;
    for (DWORD i = 0; i < *(DWORD *)(sound + LAYOUT_COUNT); i++)
        if (driver_open(sound, *(BYTE **)(sound + LAYOUTS) + i * LAYOUT_SIZE, DRIVER_RATE, DRIVER_BITS)) break;
    ((LastStartCallFn)(*(void ***)sound)[LAST_START_CALL_SLOT])(sound, 5, 1);
    api->log_line("soundprobe: Miles' start-up and speaker probes took %.0f ms on a helper (%lu layouts work); sound part 2 waited %.0f ms",
                  helper_ms, *(DWORD *)(sound + LAYOUT_COUNT), waited);
}

static void __cdecl part2_hook(DWORD a, void *b) {
    finish();
    original_part2(a, b);
}

static void __cdecl shutdown_hook(void) {
    finish();
    sound_shutdown();
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    const char *error = S2_CHECK_ENGINE(api, soundprobe_checks);
    if (error) return error;
    HMODULE ole32 = GetModuleHandleW(L"ole32.dll");
    if (!ole32) return "ole32.dll is not loaded";
    get_apartment_type = (ApartmentTypeFn)(void *)GetProcAddress(ole32, "CoGetApartmentType");
    com_init = (ComInitFn)(void *)GetProcAddress(ole32, "CoInitializeEx");
    com_uninit = (ComUninitFn)(void *)GetProcAddress(ole32, "CoUninitialize");
    if (!get_apartment_type || !com_init || !com_uninit) return "ole32.dll lacks CoGetApartmentType, CoInitializeEx or CoUninitialize";
    engine_start = (SoundFn)(void *)(api->engine + SOUND_START_RVA);
    miles_start = (SoundFn)(void *)(api->engine + MILES_START_RVA);
    driver_open = (DriverOpenFn)(void *)(api->engine + DRIVER_OPEN_RVA);
    sound_shutdown = (ShutdownFn)(void *)(api->engine + SOUND_SHUTDOWN_RVA);
    BYTE *start_call = api->engine + START_CALL_RVA;
    start_call_replacement[0] = 0xe8;
    *(LONG *)(start_call_replacement + 1) = (LONG)((BYTE *)start_hook - (start_call + 5));
    shutdown_push_replacement[0] = 0x68;
    *(void **)(shutdown_push_replacement + 1) = (void *)shutdown_hook;
    error = api->hook(api->engine + PART2_RVA, part2_prologue, sizeof part2_prologue, (const void *)part2_hook, (void **)&original_part2);
    if (!error) error = api->patch(api->engine + SHUTDOWN_PUSH_RVA, shutdown_push_original, shutdown_push_replacement, sizeof shutdown_push_original);
    if (!error) error = api->patch(start_call, start_call_original, start_call_replacement, sizeof start_call_original);
    if (error) return error;
    api->register_switch("soundprobe", &probe_enabled, NULL);
    api->log_line("soundprobe: Miles' start-up and its speaker probes will run on a helper thread");
    return NULL;
}
