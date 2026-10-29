#include "../patchset.h"
#include "../checks/packcache.h"

// The engine reads packs and loose data files through file mappings made from FILE_FLAG_NO_BUFFERING handles: each pack
// is reopened at 0x112e14e0 with 0x70000001, loose files are opened at 0x11284e50 with 0x68000001. Such a mapping keeps
// its pages only while the game holds it, so every game start reads all its data from disk again (200 MB of a pack: 205
// disk faults and ~120 ms on every run; through a cached handle none from the second run, 35 ms). Without the flag
// Windows keeps the data between starts; the overlapped and access hints stay. Only files opened afterwards change, so
// it takes effect from the next start (boot.pack is reopened before s2fix loads).
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
