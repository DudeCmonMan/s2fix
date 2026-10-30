#pragma once
#include <windows.h>
#include <stdint.h>

// expected holds the DLL file's bytes; the loader's relocations are applied before comparing.
BOOL code_matches(const BYTE *target, const BYTE *expected, size_t size);
const char *patch_code(BYTE *target, const BYTE *patch, size_t size);
const char *hook_function(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
// Names the module whose hooks the following hook_function calls install.
void set_hook_owner(const char *module_name);
const char *patch_checked(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);

// Groups this thread's hooks and checked patches (group > 0), so a fix that could not fully install can be undone.
void begin_write_group(int group);
void end_write_group(void);
const char *undo_write_group(int group, int *hooks_undone, int *patches_undone);

typedef struct {
    const BYTE *at;
    size_t size;
    int group;
} LiveWrite;
int list_live_writes(LiveWrite *out, int capacity);

// FNV-1a 64, less s2prof's writes and relocations, as the checks were made; jump_allowed passes another mod's jump.
BOOL engine_hash_matches(const BYTE *engine, DWORD rva, DWORD size, uint64_t hash, BOOL jump_allowed);
