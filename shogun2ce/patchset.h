#pragma once
#include "s2module.h"
#include <string.h>

// A set of code patches that follows a switch: a thread applies them while Apply Fixes and the switch are on and puts the
// engine's bytes back otherwise. Patches are rewritten with the game's other threads suspended, so a flip is safe at any
// time; the thread checks every POLL_MS.
#define MAX_PATCH_BYTES 16
#define POLL_MS 20

typedef struct {
    DWORD rva;
    size_t size;
    BYTE original[MAX_PATCH_BYTES], replacement[MAX_PATCH_BYTES];
} CodePatch;

typedef struct {
    const S2Api *api;
    const char *name;
    CodePatch *patches;
    int count;
    volatile LONG *flag;
    BOOL applied;
} PatchSet;

static const char *apply_patch_set(PatchSet *set, BOOL apply) {
    for (int i = 0; i < set->count; i++) {
        CodePatch *patch = &set->patches[i];
        const char *error = set->api->patch(set->api->engine + patch->rva, apply ? patch->original : patch->replacement,
                                            apply ? patch->replacement : patch->original, patch->size);
        if (error) return error;
    }
    set->applied = apply;
    return NULL;
}

static DWORD WINAPI follow_switch(LPVOID context) {
    PatchSet *set = context;
    for (;;) {
        BOOL wanted = *set->api->fix_enabled && *set->flag;
        if (wanted != set->applied) {
            const char *error = apply_patch_set(set, wanted);
            if (error) {
                set->api->log_line("%s: %s the patches failed: %s; switch left as it was", set->name, wanted ? "applying" : "reverting", error);
                *set->flag = set->applied;
            }
        }
        Sleep(POLL_MS);
    }
    return 0;
}

// Checks every site before touching any, so a mismatched build is left untouched.
static const char *start_patch_set(PatchSet *set) {
    for (int i = 0; i < set->count; i++)
        if (memcmp(set->api->engine + set->patches[i].rva, set->patches[i].original, set->patches[i].size)) return "engine code differs from the analysed build";
    HANDLE thread = CreateThread(NULL, 0, follow_switch, set, 0, NULL);
    if (!thread) return "CreateThread failed";
    CloseHandle(thread);
    return NULL;
}
