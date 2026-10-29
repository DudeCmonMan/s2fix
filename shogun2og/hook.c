#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include "hook.h"
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JUMP_SIZE 5
#define MAX_PATCH_THREADS 2048
#define MAX_HOOKS 256
// Trampolines share executable blocks: a VirtualAlloc of its own would take 64 KB of the 32-bit address space each.
#define TRAMPOLINE_BLOCK_BYTES 0x10000
#define MAX_HOOK_PATCH 64
#define PATCH_ATTEMPTS 100
#define MAX_RELOCATED_MODULES 8

// Walks this process's threads only; a system-wide thread snapshot takes tens of milliseconds on a busy desktop.
NTSTATUS NTAPI NtGetNextThread(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags, PHANDLE next);

#define MAX_LAYERS 4
#define MAX_OWNER 32
#define MAX_PATCH_RECORDS 256

// One module's hook on a target; each layer calls the one below it, the lowest calls the trampoline.
typedef struct {
    char owner[MAX_OWNER];
    const void *replacement;
    void **original;
    int group;
} HookLayer;

typedef struct {
    BYTE *target;
    // The trampoline starts with the size bytes the jump replaced, as they were in memory.
    void *trampoline;
    size_t size;
    HookLayer layers[MAX_LAYERS];
    int layer_count;
} InstalledHook;

// A patch s2prof holds: an undo restores replaced, and the engine checks read through it.
typedef struct {
    BYTE *target;
    size_t size;
    BYTE replaced[MAX_HOOK_PATCH];
    int group;
} PatchRecord;

typedef struct {
    HMODULE module;
    ULONG_PTR preferred_base;
} RelocatedModule;

static InstalledHook installed_hooks[MAX_HOOKS];
static PatchRecord patch_records[MAX_PATCH_RECORDS];
static int patch_record_count;
// Only the installing thread's writes join its group: patch-set threads flip their patches on their own.
static int write_group;
static DWORD write_group_thread;
static BYTE *trampoline_block;
static size_t trampoline_block_used;

static BYTE *allocate_trampoline(size_t bytes) {
    bytes = (bytes + 15) & ~(size_t)15;
    if (!trampoline_block || trampoline_block_used + bytes > TRAMPOLINE_BLOCK_BYTES) {
        trampoline_block = VirtualAlloc(NULL, TRAMPOLINE_BLOCK_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        trampoline_block_used = 0;
        if (!trampoline_block) return NULL;
    }
    BYTE *trampoline = trampoline_block + trampoline_block_used;
    trampoline_block_used += bytes;
    return trampoline;
}
static int installed_hook_count;
static char hook_owner[MAX_OWNER] = "s2prof";
static RelocatedModule relocated_modules[MAX_RELOCATED_MODULES];
static int relocated_module_count;

// The loader may rewrite the in-memory header's ImageBase, so the preferred base is read from the file itself.
static ULONG_PTR preferred_base(HMODULE module) {
    for (int i = 0; i < relocated_module_count; i++)
        if (relocated_modules[i].module == module) return relocated_modules[i].preferred_base;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return 0;
    FILE *file = _wfopen(path, L"rb");
    if (!file) return 0;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;
    ULONG_PTR base = 0;
    if (fread(&dos, sizeof dos, 1, file) == 1 && !fseek(file, dos.e_lfanew, SEEK_SET) && fread(&nt, sizeof nt, 1, file) == 1 &&
        nt.Signature == IMAGE_NT_SIGNATURE)
        base = nt.OptionalHeader.ImageBase;
    fclose(file);
    if (base && relocated_module_count < MAX_RELOCATED_MODULES) relocated_modules[relocated_module_count++] = (RelocatedModule){module, base};
    return base;
}

// Adds delta to every HIGHLOW-relocated dword inside [rva, rva + size) of bytes. Strict: a relocated dword crossing the
// window's edge means the window does not end on an instruction boundary; otherwise such a dword is left alone.
static BOOL relocate(const BYTE *base, DWORD rva, BYTE *bytes, size_t size, ULONG_PTR delta, BOOL strict) {
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *directory = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!directory->VirtualAddress || !directory->Size) return delta == 0;
    const BYTE *at = base + directory->VirtualAddress, *end = at + directory->Size;
    while (at + sizeof(IMAGE_BASE_RELOCATION) <= end) {
        const IMAGE_BASE_RELOCATION *block = (const IMAGE_BASE_RELOCATION *)at;
        if (block->SizeOfBlock < sizeof *block) break;
        // A dword relocated at the end of the previous page can still reach into the window.
        if (block->VirtualAddress + 0x1000 + 3 > rva && block->VirtualAddress < rva + size) {
            const WORD *entries = (const WORD *)(block + 1);
            size_t count = (block->SizeOfBlock - sizeof *block) / sizeof(WORD);
            for (size_t i = 0; i < count; i++) {
                if (entries[i] >> 12 != IMAGE_REL_BASED_HIGHLOW) continue;
                DWORD at_rva = block->VirtualAddress + (entries[i] & 0xfff);
                if (at_rva + 4 <= rva || at_rva >= rva + size) continue;
                if (at_rva < rva || at_rva + 4 > rva + size) {
                    if (strict) return FALSE;
                    continue;
                }
                DWORD value;
                memcpy(&value, bytes + (at_rva - rva), 4);
                value += (DWORD)delta;
                memcpy(bytes + (at_rva - rva), &value, 4);
            }
        }
        at += block->SizeOfBlock;
    }
    return TRUE;
}

BOOL code_matches(const BYTE *target, const BYTE *expected, size_t size) {
    BYTE adjusted[MAX_HOOK_PATCH];
    if (size > sizeof adjusted) return FALSE;
    memcpy(adjusted, expected, size);
    HMODULE module;
    // Code outside every loaded image is never a hook target, and reading it could fault.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)target, &module) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(target + size - 1), &module))
        return FALSE;
    ULONG_PTR base = preferred_base(module);
    if (!base || !relocate((const BYTE *)module, (DWORD)(target - (const BYTE *)module), adjusted, size, (ULONG_PTR)module - base, TRUE)) return FALSE;
    return !memcmp(target, adjusted, size);
}

// The jump is relative, so it is computed for the address it will execute at, which differs from out when out is a
// staging buffer.
static void write_jump(BYTE *out, const BYTE *located_at, const void *destination) {
    out[0] = 0xe9;
    LONG offset = (LONG)((const BYTE *)destination - (located_at + JUMP_SIZE));
    memcpy(out + 1, &offset, sizeof offset);
}

// Other threads are suspended so none can execute a half-written prologue.
// One patcher at a time: two threads each suspending all others could suspend each other and stop the process.
static SRWLOCK patch_lock = SRWLOCK_INIT;

static const char *patch_code_locked(BYTE *target, const BYTE *patch, size_t size) {
    static HANDLE handles[MAX_PATCH_THREADS];
    int count = 0;
    DWORD self = GetCurrentThreadId();
    HANDLE cursor = NULL, next, own = NULL;
    while (count < MAX_PATCH_THREADS &&
           NtGetNextThread(GetCurrentProcess(), cursor, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &next) >= 0) {
        if (GetThreadId(next) == self) own = next;
        else handles[count++] = next;
        cursor = next;
    }
    const char *result = "a thread kept executing the patch site";
    for (int attempt = 0; attempt < PATCH_ATTEMPTS && result; attempt++) {
        for (int i = 0; i < count; i++) SuspendThread(handles[i]);
        BOOL busy = FALSE;
        for (int i = 0; i < count && !busy; i++) {
            CONTEXT context = {.ContextFlags = CONTEXT_CONTROL};
            busy = GetThreadContext(handles[i], &context) && context.Eip >= (DWORD)target && context.Eip < (DWORD)(target + size);
        }
        if (!busy) {
            DWORD protection;
            if (VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &protection)) {
                memcpy(target, patch, size);
                VirtualProtect(target, size, protection, &protection);
                FlushInstructionCache(GetCurrentProcess(), target, size);
                result = NULL;
            } else {
                result = "VirtualProtect failed";
                attempt = PATCH_ATTEMPTS;
            }
        }
        for (int i = 0; i < count; i++) ResumeThread(handles[i]);
        if (result && attempt < PATCH_ATTEMPTS) Sleep(1);
    }
    for (int i = 0; i < count; i++) CloseHandle(handles[i]);
    if (own) CloseHandle(own);
    return result;
}

const char *patch_code(BYTE *target, const BYTE *patch, size_t size) {
    AcquireSRWLockExclusive(&patch_lock);
    const char *result = patch_code_locked(target, patch, size);
    ReleaseSRWLockExclusive(&patch_lock);
    return result;
}

static int current_group(void) {
    return write_group && GetCurrentThreadId() == write_group_thread ? write_group : 0;
}

// A write that puts back the bytes s2prof found there ends the record; any other write keeps the first bytes found.
static const char *record_patch_locked(BYTE *target, const BYTE *replacement, size_t size) {
    int found = -1;
    for (int i = 0; i < patch_record_count && found < 0; i++)
        if (patch_records[i].target == target && patch_records[i].size == size) found = i;
    if (found < 0 && patch_record_count == MAX_PATCH_RECORDS) return "all patch records are used";
    BYTE before[MAX_HOOK_PATCH];
    memcpy(before, target, size);
    const char *error = patch_code_locked(target, replacement, size);
    if (error) return error;
    if (found < 0) {
        PatchRecord *record = &patch_records[patch_record_count++];
        *record = (PatchRecord){.target = target, .size = size, .group = current_group()};
        memcpy(record->replaced, before, size);
    } else if (!memcmp(target, patch_records[found].replaced, size)) {
        patch_records[found] = patch_records[--patch_record_count];
    }
    return NULL;
}

const char *patch_checked(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size) {
    AcquireSRWLockExclusive(&patch_lock);
    const char *result = code_matches(target, expected, size) ? record_patch_locked(target, replacement, size) : "engine code differs from the analysed build";
    ReleaseSRWLockExclusive(&patch_lock);
    return result;
}

// A copy named <module>_<digits>.dll counts as the same owner as <module>.dll.
void set_hook_owner(const char *module_name) {
    size_t length = strcspn(module_name, ".");
    size_t digits = length;
    while (digits && module_name[digits - 1] >= '0' && module_name[digits - 1] <= '9') digits--;
    if (digits < length && digits && module_name[digits - 1] == '_') length = digits - 1;
    snprintf(hook_owner, sizeof hook_owner, "%.*s", (int)length, module_name);
}

// Hooks from different modules on one target stack, the newest outermost. A module hooking a target again (a reload)
// replaces its own layer: the layer above it, or the jump, is pointed at the new replacement.
static const char *add_layer(InstalledHook *hook, const void *replacement, void **original) {
    BYTE patch[MAX_HOOK_PATCH];
    for (int i = 0; i < hook->layer_count; i++)
        if (!strcmp(hook->layers[i].owner, hook_owner)) {
            *original = i ? (void *)hook->layers[i - 1].replacement : hook->trampoline;
            hook->layers[i].replacement = replacement;
            hook->layers[i].original = original;
            if (i + 1 < hook->layer_count) {
                *hook->layers[i + 1].original = (void *)replacement;
                return NULL;
            }
            write_jump(patch, hook->target, replacement);
            return patch_code_locked(hook->target, patch, JUMP_SIZE);
        }
    if (hook->layer_count == MAX_LAYERS) return "all hook layers on this target are used";
    *original = (void *)hook->layers[hook->layer_count - 1].replacement;
    write_jump(patch, hook->target, replacement);
    const char *error = patch_code_locked(hook->target, patch, JUMP_SIZE);
    if (!error) {
        HookLayer *layer = &hook->layers[hook->layer_count++];
        snprintf(layer->owner, sizeof layer->owner, "%s", hook_owner);
        layer->replacement = replacement;
        layer->original = original;
        layer->group = current_group();
    }
    return error;
}

static const char *hook_function_locked(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original) {
    BYTE patch[MAX_HOOK_PATCH];
    for (int i = 0; i < installed_hook_count; i++)
        if (installed_hooks[i].target == target) return add_layer(&installed_hooks[i], replacement, original);
    if (installed_hook_count == MAX_HOOKS) return "all hook slots used";
    if (size < JUMP_SIZE || size > MAX_HOOK_PATCH) return "prologue size must fit the jump and the patch buffer";
    if (!code_matches(target, prologue, size)) return "engine code differs from the analysed build";
    BYTE *trampoline = allocate_trampoline(size + JUMP_SIZE);
    if (!trampoline) return "VirtualAlloc for a trampoline failed";
    // The copied bytes are the relocated ones, so absolute addresses in them stay correct.
    memcpy(trampoline, target, size);
    write_jump(trampoline + size, trampoline + size, target + size);
    FlushInstructionCache(GetCurrentProcess(), trampoline, size + JUMP_SIZE);
    *original = trampoline;
    write_jump(patch, target, replacement);
    memset(patch + JUMP_SIZE, 0xcc, size - JUMP_SIZE);
    const char *error = patch_code_locked(target, patch, size);
    if (!error) {
        InstalledHook *hook = &installed_hooks[installed_hook_count++];
        *hook = (InstalledHook){.target = target, .trampoline = trampoline, .size = size, .layer_count = 1};
        snprintf(hook->layers[0].owner, sizeof hook->layers[0].owner, "%s", hook_owner);
        hook->layers[0].replacement = replacement;
        hook->layers[0].original = original;
        hook->layers[0].group = current_group();
    }
    return error;
}

const char *hook_function(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original) {
    AcquireSRWLockExclusive(&patch_lock);
    const char *result = hook_function_locked(target, prologue, size, replacement, original);
    ReleaseSRWLockExclusive(&patch_lock);
    return result;
}

void begin_write_group(int group) {
    AcquireSRWLockExclusive(&patch_lock);
    write_group = group;
    write_group_thread = GetCurrentThreadId();
    ReleaseSRWLockExclusive(&patch_lock);
}

void end_write_group(void) {
    AcquireSRWLockExclusive(&patch_lock);
    write_group = 0;
    ReleaseSRWLockExclusive(&patch_lock);
}

static const char *remove_layer_locked(InstalledHook *hook, int index) {
    if (index + 1 < hook->layer_count) {
        *hook->layers[index + 1].original = index ? (void *)hook->layers[index - 1].replacement : hook->trampoline;
    } else if (index) {
        BYTE patch[JUMP_SIZE];
        write_jump(patch, hook->target, hook->layers[index - 1].replacement);
        const char *error = patch_code_locked(hook->target, patch, JUMP_SIZE);
        if (error) return error;
    } else {
        const char *error = patch_code_locked(hook->target, hook->trampoline, hook->size);
        if (error) return error;
    }
    memmove(&hook->layers[index], &hook->layers[index + 1], (hook->layer_count - index - 1) * sizeof *hook->layers);
    hook->layer_count--;
    return NULL;
}

const char *undo_write_group(int group, int *hooks_undone, int *patches_undone) {
    *hooks_undone = *patches_undone = 0;
    const char *error = NULL;
    AcquireSRWLockExclusive(&patch_lock);
    for (int i = patch_record_count - 1; i >= 0 && !error; i--) {
        if (patch_records[i].group != group) continue;
        error = patch_code_locked(patch_records[i].target, patch_records[i].replaced, patch_records[i].size);
        if (error) break;
        memmove(&patch_records[i], &patch_records[i + 1], (patch_record_count - i - 1) * sizeof *patch_records);
        patch_record_count--;
        ++*patches_undone;
    }
    for (int i = installed_hook_count - 1; i >= 0 && !error; i--) {
        InstalledHook *hook = &installed_hooks[i];
        for (int layer = hook->layer_count - 1; layer >= 0 && !error; layer--) {
            if (hook->layers[layer].group != group) continue;
            error = remove_layer_locked(hook, layer);
            if (!error) ++*hooks_undone;
        }
        if (!error && !hook->layer_count) {
            memmove(hook, hook + 1, (installed_hook_count - i - 1) * sizeof *hook);
            installed_hook_count--;
        }
    }
    ReleaseSRWLockExclusive(&patch_lock);
    return error;
}

int list_live_writes(LiveWrite *out, int capacity) {
    int count = 0;
    AcquireSRWLockExclusive(&patch_lock);
    for (int i = 0; i < patch_record_count && count < capacity; i++)
        out[count++] = (LiveWrite){patch_records[i].target, patch_records[i].size, patch_records[i].group};
    for (int i = 0; i < installed_hook_count; i++)
        for (int layer = 0; layer < installed_hooks[i].layer_count && count < capacity; layer++)
            out[count++] = (LiveWrite){installed_hooks[i].target, installed_hooks[i].size, installed_hooks[i].layers[layer].group};
    ReleaseSRWLockExclusive(&patch_lock);
    return count;
}

static void overlay(BYTE *out, const BYTE *at, size_t size, const BYTE *written_at, const BYTE *replaced, size_t written_size) {
    const BYTE *from = written_at > at ? written_at : at, *to = written_at + written_size < at + size ? written_at + written_size : at + size;
    if (from < to) memcpy(out + (from - at), replaced + (from - written_at), to - from);
}

BOOL engine_hash_matches(const BYTE *engine, DWORD rva, DWORD size, uint64_t hash, BOOL jump_allowed) {
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(engine + ((const IMAGE_DOS_HEADER *)engine)->e_lfanew);
    ULONG_PTR base = preferred_base((HMODULE)engine);
    // Three bytes each side hold every relocated dword that reaches into the range.
    DWORD start = rva >= 3 ? rva - 3 : 0, end = rva + size + 3;
    if (!base || rva + size > nt->OptionalHeader.SizeOfImage) return FALSE;
    if (end > nt->OptionalHeader.SizeOfImage) end = nt->OptionalHeader.SizeOfImage;
    BYTE *bytes = malloc(end - start);
    if (!bytes) return FALSE;
    const BYTE *at = engine + start;
    AcquireSRWLockExclusive(&patch_lock);
    memcpy(bytes, at, end - start);
    for (int i = 0; i < patch_record_count; i++)
        overlay(bytes, at, end - start, patch_records[i].target, patch_records[i].replaced, patch_records[i].size);
    for (int i = 0; i < installed_hook_count; i++)
        overlay(bytes, at, end - start, installed_hooks[i].target, installed_hooks[i].trampoline, installed_hooks[i].size);
    ReleaseSRWLockExclusive(&patch_lock);
    relocate(engine, start, bytes, end - start, base - (ULONG_PTR)engine, FALSE);
    uint64_t value = 0xcbf29ce484222325ull;
    for (DWORD i = rva - start; i < rva - start + size; i++) value = (value ^ bytes[i]) * 0x100000001b3ull;
    free(bytes);
    return value == hash || (jump_allowed && engine[rva] == 0xe9);
}
