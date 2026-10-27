#include "../patchset.h"

// The building draw (0x1803d0c40) runs, for every piece whose damage ratio x 0.7 is not exactly 1, a loop over every other
// piece of the building: a pure distance function (0x18027e6a0) plus 5 and a square root (0x1802d64a0), whose results
// are dropped. So the loop, O(pieces squared) per building per frame, does no work the frame uses. The je that skips it
// when the ratio is exactly 1 becomes a jmp, so every building takes the engine's own skip path.
//
// That ratio is then computed for nothing, per piece per frame: dependent loads through the piece's level list, each a
// likely cache miss. Its only reader is the compare feeding the je. buildingratio jumps from its first load (0x1803d0e0d)
// to the je; the ratio only writes volatile registers, which the loop's calls clobber before the same join at
// 0x1803d0ec9, so nothing after it reads them. Without the jmp a skipped ratio only decides whether the dead loop runs,
// so either switch is safe alone.

// 0 runs the loop (0 computes the ratio); exported, and switches, so each can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG skip_enabled = 1;
__declspec(dllexport) volatile LONG ratio_skip_enabled = 1;

static CodePatch patches[] = {
    // je 0x1803d0ec9 -> jmp 0x1803d0ec9.
    {0x3d0e7b, 2, {0x74, 0x4c}, {0xeb, 0x4c}},
};

static CodePatch ratio_patches[] = {
    // mov rax,[r14+0x30] -> jmp short 0x1803d0e7b; nop; nop.
    {0x3d0e0d, 4, {0x49, 0x8b, 0x46, 0x30}, {0xeb, 0x6c, 0x90, 0x90}},
};

static PatchSet set = {.name = "buildingblock", .patches = patches, .count = sizeof patches / sizeof *patches, .flag = &skip_enabled};
static PatchSet ratio_set = {.name = "buildingratio", .patches = ratio_patches, .count = sizeof ratio_patches / sizeof *ratio_patches,
                             .flag = &ratio_skip_enabled};

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    if (!api->patch) return "the host is older than this module (no patch)";
    set.api = ratio_set.api = api;
    const char *error = start_patch_set(&set);
    if (!error) api->register_switch("buildingblock", &skip_enabled, NULL);
    const char *ratio_error = start_patch_set(&ratio_set);
    if (!ratio_error) api->register_switch("buildingratio", &ratio_skip_enabled, NULL);
    return error ? error : ratio_error;
}
