#include "../patchset.h"
#include "../checks/packcache.h"

// The engine reopens each pack with FILE_FLAG_NO_BUFFERING for its mapping, whose pages go when closed, so
// every start reads its data from disk again; dropping the flag lets Windows keep it (from the next start).
__declspec(dllexport) volatile LONG cache_enabled = 1;

static CodePatch patches[] = {
    // mov dword [rsp+0x28],0x70000001 -> 0x50000001: the pack reopen's flags.
    {0x185a5a9, 8, {0xc7, 0x44, 0x24, 0x28, 0x01, 0x00, 0x00, 0x70}, {0xc7, 0x44, 0x24, 0x28, 0x01, 0x00, 0x00, 0x50}},
};

static PatchSet set = {.name = "packcache", .patches = patches, .count = sizeof patches / sizeof *patches, .flag = &cache_enabled};

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    set.api = api;
    const char *error = S2_CHECK_ENGINE(api, packcache_checks);
    return error ? error : start_patch_set(&set);
}
