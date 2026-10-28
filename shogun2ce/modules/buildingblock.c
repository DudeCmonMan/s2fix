#include "../patchset.h"
#include "../checks/buildingblock.h"

// The building draw (0x1803d0c40) runs, for every piece whose damage ratio x 0.7 is not exactly 1 (or whose level holds a
// zero at +0xe0, which jumps straight in), a loop over every other piece of the building: a pure distance function
// (0x18027e6a0) plus 5 and a square root (0x1802d64a0), whose results are dropped. So the loop, O(pieces squared) per
// building per frame, does no work the frame uses. Its entry (0x1803d0e7d, reached both ways) becomes a jmp to the join at
// 0x1803d0ec9, the engine's own skip path: rbx is written before it is read again, and the volatile registers are the
// loop's calls' to clobber.
//
// That ratio is then computed for nothing, per piece per frame: dependent loads through the piece's level list, each a
// likely cache miss. Its only reader is the compare feeding the je before the loop. buildingratio jumps from its first
// load (0x1803d0e0d) to that je, whose flags then always skip the loop; the ratio only writes volatile registers, so
// nothing after the join reads them. Either switch is safe alone.

// 0 runs the loop (0 computes the ratio); exported, and switches, so each can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG skip_enabled = 1;
__declspec(dllexport) volatile LONG ratio_skip_enabled = 1;

static CodePatch patches[] = {
    // xor ebx,ebx -> jmp short 0x1803d0ec9.
    {0x3d0e7d, 2, {0x33, 0xdb}, {0xeb, 0x4a}},
};

static CodePatch ratio_patches[] = {
    // mov rax,[r14+0x30] -> jmp short 0x1803d0e7b; nop; nop.
    {0x3d0e0d, 4, {0x49, 0x8b, 0x46, 0x30}, {0xeb, 0x6c, 0x90, 0x90}},
};

static PatchSet set = {.name = "buildingblock", .patches = patches, .count = sizeof patches / sizeof *patches, .flag = &skip_enabled};
static PatchSet ratio_set = {.name = "buildingratio", .patches = ratio_patches, .count = sizeof ratio_patches / sizeof *ratio_patches,
                             .flag = &ratio_skip_enabled};

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    set.api = ratio_set.api = api;
    const char *error = S2_CHECK_ENGINE(api, buildingblock_checks);
    if (!error) error = start_patch_set(&set);
    if (!error) api->register_switch("buildingblock", &skip_enabled, NULL);
    api->end_fix(error);
    api->begin_fix("buildingratio");
    error = S2_CHECK_ENGINE(api, buildingratio_checks);
    if (!error) error = start_patch_set(&ratio_set);
    if (!error) api->register_switch("buildingratio", &ratio_skip_enabled, NULL);
    return error;
}
