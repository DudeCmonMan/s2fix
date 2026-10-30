#pragma once
#include <windows.h>

// Asks the engine to rebuild every GPU resource through its own recovery, and tells the fixes around every rebuild.

typedef struct {
    BYTE *engine;
    void (*log_line)(const char *format, ...);
    const char *(*hook)(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
    const char *(*patch)(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);
} GraphicsResetHost;

// NULL once the engine's recovery is reached, else why requests can't run; listeners are kept either way.
const char *graphics_reset_init(const GraphicsResetHost *host, const char *unavailable);
// FALSE, the reason logged, when a request now would be dropped.
BOOL graphics_reset_allowed(const char *reason);
// Any thread; runs next frame. reason is kept until then: a string literal, or one that lives as long.
void graphics_reset_request(const char *reason);
// On main, any cause: before each GPU release (may repeat), after each re-creation; a load tells only after.
void graphics_reset_listen(void (*before)(void), void (*after)(void));
// For the snapshot.
void graphics_reset_describe(char *out, size_t size);
