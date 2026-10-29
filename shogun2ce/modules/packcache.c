#include "../patchset.h"
#include "../checks/packcache.h"

// The engine reads each pack's index through a buffered handle, closes it, and reopens the pack with
// FILE_FLAG_NO_BUFFERING (0x18185a5a9: flags 0x70000001) for the file mapping it reads the pack's data through. Such a
// mapping keeps its pages only while the game holds it, so every game start reads all its data from disk again, and it
// cannot use what the buffered index reads left in Windows' cache. Without the flag Windows keeps the data between
// starts; the overlapped and access hints stay. Loose files are already opened buffered here. Only packs opened
// afterwards change, so it takes effect from the next start for packs opened before s2fix loads.
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
