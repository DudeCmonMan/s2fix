#include "../patchset.h"
#include "../checks/instancecopy.h"

// The instance gathers use their 16-byte movups loop only when source, destination and size are 16-aligned, else
// memcpy; movups needs no alignment, so each check is narrowed to the size (the same loop copies the same bytes).

__declspec(dllexport) volatile LONG loop_copies_enabled = 1;

static CodePatch patches[] = {
    // Variant gatherer, instancing: mov eax,esi; or eax,ecx -> mov eax,edx (the size), then or eax,edx; test al,0xf.
    {0x14c57cc, 4, {0x8b, 0xc6, 0x0b, 0xc1}, {0x8b, 0xc2, 0x90, 0x90}},
    // Variant gatherer, no instancing: mov ecx,esi; or ecx,eax -> mov ecx,edx (the size).
    {0x14c584b, 4, {0x8b, 0xce, 0x0b, 0xc8}, {0x8b, 0xca, 0x90, 0x90}},
    // 0x114c56d0: or ecx,eax (ecx held the source) -> mov ecx,edx (the size), then or ecx,edx; test cl,0xf.
    {0x14c5719, 2, {0x0b, 0xc8}, {0x8b, 0xca}},
    // 0x1146c3b0: edx holds the size; or edx,eax; or edx,ecx (source, destination) -> nothing.
    {0x146c3f3, 4, {0x0b, 0xd0, 0x0b, 0xd1}, {0x90, 0x90, 0x90, 0x90}},
    // Group gatherer, whose sizes are 32-byte multiples: test cl,0xf; test al,0xf (source, destination) -> always zero.
    {0x146c088, 3, {0xf6, 0xc1, 0x0f}, {0xf6, 0xc1, 0x00}},
    {0x146c0fb, 2, {0xa8, 0x0f}, {0xa8, 0x00}},
};

static PatchSet set = {.name = "instancecopy", .patches = patches, .count = sizeof patches / sizeof *patches, .flag = &loop_copies_enabled};

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, instancecopy_checks);
    if (mismatch) return mismatch;
    set.api = api;
    const char *error = start_patch_set(&set);
    if (!error) api->register_switch("instancecopy", &loop_copies_enabled, NULL);
    return error;
}
