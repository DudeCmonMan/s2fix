#include "../patchset.h"

// The building draw (0x102bfc50) runs, for every piece whose damage ratio x 0.7 is not exactly 1, a loop over every other
// piece of the building: a pure distance function (0x101aaed0, float math, no stores), plus 5 and a bit-trick square
// root, into two stack slots. Neither slot is read afterwards: every later read of the first follows a write on its own
// path, and nothing reads the second. So the loop, O(pieces squared) per building per frame, does no work the frame
// uses. The jnp that skips it when the ratio is exactly 1 becomes a jmp, so every building takes the engine's own
// skip path.
//
// That ratio is then computed for nothing, per piece per frame: four dependent loads (piece+0x18, its level list
// +0x6c/+0x70, the next-to-last level, +0x90), each a likely cache miss: a fifth of the function's own time in a siege.
// Its only reader is the compare feeding the jnp. buildingratio jumps from its first load (0x102bfd33) to 0x102bfda6,
// which reloads esi and the 1.0 the code after it needs; nothing after reads eax, xmm0, xmm1, [esp+0x14] or [esp+0x24]
// before writing them. Without the jmp a skipped ratio only decides whether the dead loop runs, so either switch is safe
// alone.

// 0 runs the loop (0 computes the ratio); exported, and switches, so each can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG skip_enabled = 1;
__declspec(dllexport) volatile LONG ratio_skip_enabled = 1;

static CodePatch patches[] = {
    // jnp 0x102bfe4f -> jmp 0x102bfe4f; nop.
    {0x2bfdb9, 6, {0x0f, 0x8b, 0x90, 0x00, 0x00, 0x00}, {0xe9, 0x91, 0x00, 0x00, 0x00, 0x90}},
};

static CodePatch ratio_patches[] = {
    // mov eax,[ecx+0x18] -> jmp short 0x102bfda6; nop.
    {0x2bfd33, 3, {0x8b, 0x41, 0x18}, {0xeb, 0x71, 0x90}},
};

static PatchSet set = {.name = "buildingblock", .patches = patches, .count = sizeof patches / sizeof *patches, .flag = &skip_enabled};
static PatchSet ratio_set = {.name = "buildingratio", .patches = ratio_patches, .count = sizeof ratio_patches / sizeof *ratio_patches,
                             .flag = &ratio_skip_enabled};

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    set.api = ratio_set.api = api;
    const char *error = start_patch_set(&set);
    if (error) return error;
    api->register_switch("buildingblock", &skip_enabled, NULL);
    error = start_patch_set(&ratio_set);
    if (error) return error;
    api->register_switch("buildingratio", &ratio_skip_enabled, NULL);
    return NULL;
}
