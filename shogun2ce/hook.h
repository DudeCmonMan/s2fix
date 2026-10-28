#pragma once
#include <windows.h>
#include <stdint.h>

// TRUE when the code at target holds expected, the bytes the DLL file has there, once the loader's relocations are applied.
BOOL code_matches(const BYTE *target, const BYTE *expected, size_t size);
const char *patch_code(BYTE *target, const BYTE *patch, size_t size);
const char *hook_function(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
// Names the module whose hooks the following hook_function calls install.
void set_hook_owner(const char *module_name);
const char *patch_checked(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);

// The hooks and checked patches this thread makes until end_write_group belong to group (> 0), so a fix that could not
// install completely can be undone.
void begin_write_group(int group);
void end_write_group(void);
const char *undo_write_group(int group, int *hooks_undone, int *patches_undone);

typedef struct {
    const BYTE *at;
    size_t size;
    int group;
} LiveWrite;
int list_live_writes(LiveWrite *out, int capacity);

// TRUE when the engine's bytes at [rva, rva + size), with s2prof's own writes and the loader's relocations taken out,
// hash (FNV-1a 64) to hash: the bytes of the file the checks were made from. jump_allowed: another mod's jump at rva
// passes too.
BOOL engine_hash_matches(const BYTE *engine, DWORD rva, DWORD size, uint64_t hash, BOOL jump_allowed);
