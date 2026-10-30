#include "../s2module.h"
#include "../checks/itemcopy.h"
#include <stdint.h>
#include <string.h>

// The engine copies its render item field by field on every list push, growth and sort move; it skips only padding
// nothing reads, so one copy of every byte is equivalent.
#define COPY_CONSTRUCTOR_RVA 0x14b1880
#define ASSIGNMENT_RVA 0x14b4530
#define RENDER_ITEM_SIZE 0x138

typedef void *(__attribute__((thiscall)) *CopyFn)(void *destination, const void *source);

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
    const char *mismatch = S2_CHECK_ENGINE(api, itemcopy_checks);
    if (mismatch) return mismatch;
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
