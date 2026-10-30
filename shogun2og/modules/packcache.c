#include "../patchset.h"
#include "../checks/packcache.h"

// The engine maps packs and loose data files through FILE_FLAG_NO_BUFFERING handles, whose pages go when closed, so
// every start reads its data from disk again; dropping the flag lets Windows keep it (from the next start).
__declspec(dllexport) volatile LONG cache_enabled = 1;

static CodePatch patches[] = {
    // push 0x70000001 -> push 0x50000001: the pack reopen.
    {0x12e14f6, 5, {0x68, 0x01, 0x00, 0x00, 0x70}, {0x68, 0x01, 0x00, 0x00, 0x50}},
    // push 0x68000001 -> push 0x48000001: the loose file open.
    {0x1284fd3, 5, {0x68, 0x01, 0x00, 0x00, 0x68}, {0x68, 0x01, 0x00, 0x00, 0x48}},
};

static PatchSet set = {.name = "packcache", .patches = patches, .count = sizeof patches / sizeof *patches, .flag = &cache_enabled};

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    set.api = api;
    const char *error = S2_CHECK_ENGINE(api, packcache_checks);
    return error ? error : start_patch_set(&set);
}
