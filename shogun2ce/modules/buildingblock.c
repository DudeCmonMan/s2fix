#include "../patchset.h"
#include "../checks/buildingblock.h"

// The building draw runs a pairwise loop per damaged piece whose results nothing reads; a jmp takes the engine's skip
// path. buildingratio also skips the ratio, whose only reader is that test. Either switch is safe alone.

__declspec(dllexport) volatile LONG skip_enabled = 1;
__declspec(dllexport) volatile LONG ratio_skip_enabled = 1;

static CodePatch patches[] = {
    // xor ebx,ebx -> jmp short 0x1803d0ec9; rbx is written again before its next read.
    {0x3d0e7d, 2, {0x33, 0xdb}, {0xeb, 0x4a}},
};

static CodePatch ratio_patches[] = {
    // mov rax,[r14+0x30] -> jmp short 0x1803d0e7b; nop; nop. The ratio only writes volatile registers.
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
