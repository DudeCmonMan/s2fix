#pragma once
#include <windows.h>

// The graphics reset: asks the engine to release and rebuild every GPU resource it holds through its own recovery
// path, and tells the fixes around every such rebuild, the engine's own included. Shared verbatim by the development and
// the release host.

typedef struct {
    BYTE *engine;
    void (*log_line)(const char *format, ...);
    const char *(*hook)(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
    const char *(*patch)(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);
} GraphicsResetHost;

// NULL once the engine's recovery is reached, else why requests can't run: unavailable when the host already knows
// (an engine check failed), or the hook's or patch's error. Listeners are kept either way.
const char *graphics_reset_init(const GraphicsResetHost *host, const char *unavailable);
// Any thread: the rebuild runs at the start of the next frame, with the engine's loading screen. reason is kept until
// then: a string literal, or one that lives as long.
void graphics_reset_request(const char *reason);
// before runs on the main thread each time the engine releases GPU resources, after each time it creates them again,
// whatever started it, but for a load's release (its creation is told); a rebuild can release more than once before it
// creates.
void graphics_reset_listen(void (*before)(void), void (*after)(void));
// One line on the engine's view of its device, for the snapshot.
void graphics_reset_describe(char *out, size_t size);
