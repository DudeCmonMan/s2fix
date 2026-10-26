#pragma once
#include <windows.h>
#include <string.h>

// A fix module is a DLL exporting s2_module_main; it returns NULL once installed, or why it could not install.
typedef struct {
    BYTE *engine;
    volatile LONG *fix_enabled;
    void (*log_line)(const char *format, ...);
    // prologue holds the target's bytes as they are in the DLL file; addresses the loader relocates are adjusted before
    // the comparison. The size must end on an instruction boundary, cover at least 5 bytes, and hold no relative branch.
    const char *(*hook)(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
    // Reserved: the layout stays fixed, so the host and the modules can be built separately.
    void *reserved[2];
    // Makes a module's on/off flag available to an A/B capture; report(TRUE) runs before each A/B and report(FALSE) after
    // it, so the module can log its own per-mode counters.
    void (*register_switch)(const char *name, volatile LONG *flag, void (*report)(BOOL start));
    // Writes replacement over the size bytes at target once they hold expected (the file's bytes; relocated addresses in
    // them are adjusted), with the other threads suspended. Swapping expected and replacement reverts it.
    const char *(*patch)(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);
    // Names the module whose hooks follow, for a loader other than the host's (a reload replaces only its own hooks).
    void (*set_hook_owner)(const char *module_name);
    // The game's main thread, the one that presents frames; 0 until the first frame.
    volatile DWORD *main_thread;
} S2Api;

typedef const char *(*S2ModuleMain)(const S2Api *api);

// A quick check before hooking several targets, so a mismatched build is left untouched. A target s2prof already hooked
// starts with its jump; the hook call itself checks the bytes against the file, relocations applied.
static inline BOOL s2_prologue_ready(const BYTE *target, const BYTE *prologue, size_t size) {
    return target[0] == 0xe9 || !memcmp(target, prologue, size);
}
