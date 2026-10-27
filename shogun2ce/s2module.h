#pragma once
#include <windows.h>

// A fix module is a DLL exporting s2_module_main; it returns NULL once installed, or why it could not install.
typedef struct {
    BYTE *engine;
    volatile LONG *fix_enabled;
    void (*log_line)(const char *format, ...);
    const char *(*hook)(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
    // Reserved: the layout stays fixed, so the host and the modules can be built separately.
    void *reserved[2];
    // Makes a module's on/off flag available to an A/B capture; report(TRUE) runs before each A/B and report(FALSE) after
    // it, so the module can log its own per-mode counters.
    void (*register_switch)(const char *name, volatile LONG *flag, void (*report)(BOOL start));
    // Writes replacement over the size bytes at target once they hold expected, with the other threads suspended.
    // Swapping expected and replacement reverts it.
    const char *(*patch)(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);
    // The game's main thread, the one that presents frames; 0 until the first frame.
    volatile DWORD *main_thread;
} S2Api;

typedef const char *(*S2ModuleMain)(const S2Api *api);
