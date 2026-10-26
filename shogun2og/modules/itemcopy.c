#include "../s2module.h"
#include <stdint.h>
#include <string.h>

// The engine's copy constructor (0x114b1880, 46 call sites) and assignment (0x114b4530, 78 call sites) of its 312-byte
// render item copy it field by field in 94 moves; they run for every list push, list growth and sort move, 12% of the
// main thread in an army view. Both skip only the padding at +0x121..+0x123 and +0x135..+0x137, which nothing reads, so
// copying every byte is equivalent.
#define COPY_CONSTRUCTOR_RVA 0x14b1880
#define ASSIGNMENT_RVA 0x14b4530
#define RENDER_ITEM_SIZE 0x138

typedef void *(__attribute__((thiscall)) *CopyFn)(void *destination, const void *source);

// 0 runs the engine's copies; exported, and a switch, so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG whole_copy_enabled = 1;

static const BYTE copy_prologue[] = {0x8b, 0x54, 0x24, 0x04, 0x8b, 0x02};
static volatile LONG *fix_enabled;
static CopyFn original_constructor, original_assignment;

static void *__attribute__((thiscall)) constructor_hook(void *destination, const void *source) {
    if (!*fix_enabled || !whole_copy_enabled) return original_constructor(destination, source);
    memcpy(destination, source, RENDER_ITEM_SIZE);
    return destination;
}

static void *__attribute__((thiscall)) assignment_hook(void *destination, const void *source) {
    if (!*fix_enabled || !whole_copy_enabled) return original_assignment(destination, source);
    if (destination != source) memcpy(destination, source, RENDER_ITEM_SIZE);
    return destination;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    fix_enabled = api->fix_enabled;
    if (!s2_prologue_ready(api->engine + COPY_CONSTRUCTOR_RVA, copy_prologue, sizeof copy_prologue) ||
        !s2_prologue_ready(api->engine + ASSIGNMENT_RVA, copy_prologue, sizeof copy_prologue))
        return "the item copies differ from the analysed build";
    const char *error = api->hook(api->engine + COPY_CONSTRUCTOR_RVA, copy_prologue, sizeof copy_prologue, (const void *)constructor_hook,
                                  (void **)&original_constructor);
    if (!error)
        error = api->hook(api->engine + ASSIGNMENT_RVA, copy_prologue, sizeof copy_prologue, (const void *)assignment_hook, (void **)&original_assignment);
    if (!error) api->register_switch("itemcopy", &whole_copy_enabled, NULL);
    return error;
}
