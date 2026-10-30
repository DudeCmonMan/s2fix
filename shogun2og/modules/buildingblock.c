#include "../patchset.h"
#include "../checks/buildingblock.h"

// The building draw runs a pairwise loop per damaged piece whose results nothing reads; a jmp takes the engine's skip
// path. buildingratio also skips the ratio, whose only reader is that test. Either switch is safe alone.

__declspec(dllexport) volatile LONG skip_enabled = 1;
__declspec(dllexport) volatile LONG ratio_skip_enabled = 1;

static CodePatch patches[] = {
    // jnp 0x102bfe4f -> jmp 0x102bfe4f; nop.
    {0x2bfdb9, 6, {0x0f, 0x8b, 0x90, 0x00, 0x00, 0x00}, {0xe9, 0x91, 0x00, 0x00, 0x00, 0x90}},
};

static CodePatch ratio_patches[] = {
    // mov eax,[ecx+0x18] -> jmp short 0x102bfda6; nop. What it skips is written again before any read.
    {0x2bfd33, 3, {0x8b, 0x41, 0x18}, {0xeb, 0x71, 0x90}},
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
