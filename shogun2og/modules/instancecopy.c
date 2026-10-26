#include "../patchset.h"

// The engine's per-draw instance gathers copy each instance's record into the mapped instance buffer: the Variant
// gatherer (0x114c5770, from item + 0x54 with instancing, + 0x04 without), its lookup-slot-0 twins (0x114c56d0,
// 0x1146c3b0) and the rigid group-instance gatherer (0x1146bfc0). Each copy takes a 16-byte movups loop only when the
// source, destination and size are all 16-aligned and calls memcpy otherwise; 312-byte items put the Variant records 4
// bytes off alignment, so nearly every record went through a memcpy call. The loop's movups needs no alignment, only a
// size that is a multiple of 16, so each check is narrowed to the size: the same bytes are copied by the same loop.
// The group gatherer's sizes are whole 32-byte records, so its two checks always take the loop.

// 0 leaves the engine's alignment checks; exported, and a switch, so it can be A/B tested while the game runs.
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
    // Group gatherer: test cl,0xf and test al,0xf over source and destination -> always zero.
    {0x146c088, 3, {0xf6, 0xc1, 0x0f}, {0xf6, 0xc1, 0x00}},
    {0x146c0fb, 2, {0xa8, 0x0f}, {0xa8, 0x00}},
};

static PatchSet set = {.name = "instancecopy", .patches = patches, .count = sizeof patches / sizeof *patches, .flag = &loop_copies_enabled};

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    set.api = api;
    const char *error = start_patch_set(&set);
    if (!error) api->register_switch("instancecopy", &loop_copies_enabled, NULL);
    return error;
}
