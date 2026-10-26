#include "../patchset.h"

// The building draw (0x102bfc50) runs, for every piece whose damage ratio x 0.7 is not exactly 1, a loop over every other
// piece of the building: a pure distance function (0x101aaed0, float math, no stores), plus 5 and a bit-trick square
// root, into two stack slots. Neither slot is read afterwards: every later read of the first follows a write on its own
// path, and nothing reads the second. So the loop, O(pieces squared) per building per frame, does no work the frame
// uses. The jnp that skips it when the ratio is exactly 1 becomes a jmp, so every building takes the engine's own
// skip path.

// 0 runs the loop; exported, and a switch, so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG skip_enabled = 1;

static CodePatch patches[] = {
    // jnp 0x102bfe4f -> jmp 0x102bfe4f; nop.
    {0x2bfdb9, 6, {0x0f, 0x8b, 0x90, 0x00, 0x00, 0x00}, {0xe9, 0x91, 0x00, 0x00, 0x00, 0x90}},
};

static PatchSet set = {.name = "buildingblock", .patches = patches, .count = sizeof patches / sizeof *patches, .flag = &skip_enabled};

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    set.api = api;
    const char *error = start_patch_set(&set);
    if (!error) api->register_switch("buildingblock", &skip_enabled, NULL);
    return error;
}
