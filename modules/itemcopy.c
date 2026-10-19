#include "../s2module.h"
#include <string.h>

// The engine's field-by-field copy of a 176-byte render item; it runs for every list push, list growth and sort move.
#define ITEM_COPY_RVA 0x1b08c00
#define RENDER_ITEM_SIZE 0xb0

static const BYTE item_copy_prologue[] = {0x48, 0x8b, 0x02, 0x4c, 0x8b, 0xc1, 0x48, 0x89, 0x01, 0x8b, 0x42, 0x08, 0x89, 0x41, 0x08};
static void *(*original_item_copy)(void *destination, const void *source);
static volatile LONG *fix_enabled;

// The engine's copy skips only padding and unused bitfield bits, which nothing reads, so copying every byte is equivalent.
static void *item_copy(void *destination, const void *source) {
    if (!*fix_enabled) return original_item_copy(destination, source);
    return memmove(destination, source, RENDER_ITEM_SIZE);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    fix_enabled = api->fix_enabled;
    return api->hook(api->engine + ITEM_COPY_RVA, item_copy_prologue, sizeof item_copy_prologue, (const void *)item_copy, (void **)&original_item_copy);
}
