#pragma once
#include <windows.h>
#include <stdint.h>
#include <string.h>

// [rva, rva + size) must hash (FNV-1a 64) to hash, less relocations and s2prof's writes. Generated into checks/.
typedef struct {
    DWORD rva, size;
    uint64_t hash;
    DWORD flags;
    const char *what;
} S2Check;

// A function the fix only calls: it must still be there, and another mod's jump at its entry still runs it.
#define S2_CHECK_ENTRY 1

// A fix module is a DLL exporting s2_module_main; it returns NULL once installed, or why it could not install.
typedef struct {
    BYTE *engine;
    volatile LONG *fix_enabled;
    void (*log_line)(const char *format, ...);
    // prologue: the target's file bytes; whole instructions, at least 5 bytes, no relative branch.
    const char *(*hook)(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
    // Reserved: the layout stays fixed, so the host and the modules can be built separately.
    void *reserved[2];
    // For A/B captures; report(TRUE) runs before each and report(FALSE) after, to log per-mode counters.
    void (*register_switch)(const char *name, volatile LONG *flag, void (*report)(BOOL start));
    // expected holds the file's bytes; the other threads are suspended. Swapping expected and replacement reverts it.
    const char *(*patch)(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);
    // For a loader other than the host's: a reload replaces only the named module's hooks.
    void (*set_hook_owner)(const char *module_name);
    // The game's main thread, the one that presents frames; 0 until the first frame.
    volatile DWORD *main_thread;
    // An error undoes the fix's hooks, patches and switches; a module with several fixes ends each, begins the next.
    const char *(*end_fix)(const char *error);
    void (*begin_fix)(const char *name);
    // NULL when every check holds, else which engine code differs; fixes call it before installing anything.
    const char *(*check_engine)(const S2Check *checks, int count);
    // Any thread; dropped if one is pending or within 5 s of the last. reason is logged later: a string literal.
    void (*request_graphics_reset)(const char *reason);
    // On main, whatever the cause: before each GPU release (may repeat), after each re-creation. Either may be NULL.
    void (*on_graphics_reset)(void (*before)(void), void (*after)(void));
} S2Api;

typedef const char *(*S2ModuleMain)(const S2Api *api);

// Leaves a mismatched build untouched; a jump passes, as the hook call itself then checks or refuses it.
static inline BOOL s2_prologue_ready(const BYTE *target, const BYTE *prologue, size_t size) {
    return target[0] == 0xe9 || !memcmp(target, prologue, size);
}

#define S2_CHECK_ENGINE(api, checks) (api)->check_engine(checks, (int)(sizeof checks / sizeof *checks))
