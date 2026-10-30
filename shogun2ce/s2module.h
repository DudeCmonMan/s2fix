#pragma once
#include <windows.h>
#include <stdint.h>

// The engine bytes at [rva, rva + size), relocations and s2prof's own writes taken out, hash (FNV-1a 64) to hash in the
// build the fixes were made for. Generated into checks/<module>.h.
typedef struct {
    DWORD rva, size;
    uint64_t hash;
    DWORD flags;
    const char *what;
} S2Check;

// A function the fix only calls: it must still be there, and another mod's jump at its entry still runs it.
#define S2_CHECK_ENTRY 1
// Where the engine's file puts it; its relocated pointers in the file hold addresses at this base.
#define ENGINE_PREFERRED_BASE 0x180000000ull

// A fix module is a DLL exporting s2_module_main; it returns NULL once installed, or why it could not install.
typedef struct {
    BYTE *engine;
    volatile LONG *fix_enabled;
    void (*log_line)(const char *format, ...);
    // prologue holds the target's bytes as they are in the DLL file; addresses the loader relocates are adjusted before
    // the comparison. The size must end on an instruction boundary and cover at least 14 bytes; a relative branch or a
    // RIP-relative operand in it only works when the trampoline is never called.
    const char *(*hook)(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
    // Reserved: the layout stays fixed, so the host and the modules can be built separately.
    void *reserved[2];
    // Makes a module's on/off flag available to an A/B capture; report(TRUE) runs before each A/B and report(FALSE) after
    // it, so the module can log its own per-mode counters.
    void (*register_switch)(const char *name, volatile LONG *flag, void (*report)(BOOL start));
    // Writes replacement over the size bytes at target once they hold expected (the file's bytes; relocated addresses in
    // them are adjusted), with the other threads suspended. Swapping expected and replacement reverts it.
    const char *(*patch)(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);
    // The game's main thread, the one that presents frames; 0 until the first frame.
    volatile DWORD *main_thread;
    // Names the module whose hooks follow, for a loader other than the host's (a reload replaces only its own hooks).
    void (*set_hook_owner)(const char *module_name);
    // The host begins one fix per module and ends it with s2_module_main's result. A fix that ends with an error has its
    // hooks, patches and switches undone. A module holding several independent fixes ends each one and begins the next
    // by name.
    const char *(*end_fix)(const char *error);
    void (*begin_fix)(const char *name);
    // NULL when every check holds, else which engine code differs; fixes call it before installing anything.
    const char *(*check_engine)(const S2Check *checks, int count);
    // Asks the engine to release and rebuild every GPU resource it holds, through its own recovery, at the start of the
    // next frame, with its loading screen shown; any thread may ask.
    // The reason goes to the log, so it must outlive the request: a string literal.
    void (*request_graphics_reset)(const char *reason);
    // before runs on the main thread each time the engine releases GPU resources and after each time it creates them
    // again, whatever started it: a requested reset, a lost device, the graphics options; of a load, only its creation is
    // told. A rebuild can release more than once before it creates, and a device the engine can't use yet delays the
    // after. Either may be NULL.
    void (*on_graphics_reset)(void (*before)(void), void (*after)(void));
} S2Api;

typedef const char *(*S2ModuleMain)(const S2Api *api);

#define S2_CHECK_ENGINE(api, checks) (api)->check_engine(checks, (int)(sizeof checks / sizeof *checks))
