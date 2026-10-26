#pragma once
#include <windows.h>

// TRUE when the code at target holds expected, the bytes the DLL file has there, once the loader's relocations are applied.
BOOL code_matches(const BYTE *target, const BYTE *expected, size_t size);
const char *patch_code(BYTE *target, const BYTE *patch, size_t size);
const char *hook_function(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
// Names the module whose hooks the following hook_function calls install.
void set_hook_owner(const char *module_name);
const char *patch_checked(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);
