#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include "sortfix.h"
#include "itemsort.h"
#include <tlhelp32.h>

// std::sort over the renderer's 176-byte items (unsigned key at +0xa8).
#define RENDER_SORT_RVA 0x1afee20
#define RENDER_LIST_RETURN_RVA 0x1b20704
#define OTHER_LIST_RETURN_RVA 0x1b211de
// std::sort over a concurrent_vector of 32-byte objects whose comparator takes both objects by value.
#define OBJECT_SORT_RVA 0x1aff420
#define OBJECT_LIST_RETURN_RVA 0x1b0ebc0
#define UNKNOWN_CALLER (SORT_CALLERS - 1)
#define JUMP_SIZE 14
#define PATCH_SIZE 18
#define PROLOGUE_CHECK_SIZE 30
#define MAX_PATCH_THREADS 2048
#define MAX_HOOKS 64
#define MAX_HOOK_PATCH 64
#define PATCH_ATTEMPTS 100

static const BYTE render_sort_prologue[PROLOGUE_CHECK_SIZE] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41,
                                                               0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xc9, 0x48, 0x81, 0xec, 0xf0, 0x00, 0x00, 0x00};
static const BYTE object_sort_prologue[PROLOGUE_CHECK_SIZE] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08,
                                                               0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24};

typedef void (*RenderSortFn)(uint8_t *first, uint8_t *last, int64_t ideal, uint64_t predicate);

typedef struct {
    const uint8_t *vector;
    uint64_t index;
    uint8_t *item;
} VectorIterator;

typedef void (*ObjectSortFn)(VectorIterator *first, VectorIterator *last, int64_t ideal, uint64_t predicate);

typedef struct {
    BYTE *target;
    void *trampoline;
} InstalledHook;

volatile LONG fixes_enabled;
volatile LONG sortfix_allocation_failures;
SortStats sort_stats[2][SORT_CALLERS];
const char *const sort_caller_names[SORT_CALLERS] = {"render list", "other list", "object list", "unknown caller"};

static RenderSortFn original_render_sort;
static ObjectSortFn original_object_sort;
static uintptr_t caller_returns[UNKNOWN_CALLER];
static InstalledHook installed_hooks[MAX_HOOKS];
static int installed_hook_count;
// Both sorts recurse into themselves through the hooked entry, so only the outermost call is replaced and timed.
static __thread int sort_depth;

static int caller_of(void *return_address) {
    for (int caller = 0; caller < UNKNOWN_CALLER; caller++)
        if ((uintptr_t)return_address == caller_returns[caller]) return caller;
    return UNKNOWN_CALLER;
}

static void record(int enabled, int caller, LONG64 items, LONG64 ticks, LONG64 descents, LONG64 moved) {
    SortStats *stats = &sort_stats[enabled][caller];
    InterlockedIncrement64(&stats->calls);
    InterlockedAdd64(&stats->items, items);
    InterlockedAdd64(&stats->ticks, ticks);
    InterlockedAdd64(&stats->descents, descents);
    InterlockedAdd64(&stats->moved, moved);
    if (!descents) InterlockedIncrement64(&stats->presorted);
    for (LONG64 max = stats->max_items; items > max; max = stats->max_items)
        if (InterlockedCompareExchange64(&stats->max_items, items, max) == max) break;
}

static LONG64 now_ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// The disorder scan is part of the fix's own work, but only measurement for the original, so it is timed only when enabled.
static void render_sort_hook(uint8_t *first, uint8_t *last, int64_t ideal, uint64_t predicate) {
    if (sort_depth) {
        original_render_sort(first, last, ideal, predicate);
        return;
    }
    int caller = caller_of(__builtin_return_address(0));
    int enabled = fixes_enabled != 0;
    size_t count = (last - first) / RENDER_ITEM_SIZE, descents = 0;
    long long moved = 0;
    if (!enabled) descents = count_render_item_descents(first, count);
    LONG64 started = now_ticks();
    sort_depth = 1;
    if (!enabled) {
        original_render_sort(first, last, ideal, predicate);
    } else if ((descents = count_render_item_descents(first, count)) && (moved = sort_render_items(first, last)) < 0) {
        InterlockedIncrement(&sortfix_allocation_failures);
        moved = 0;
        original_render_sort(first, last, ideal, predicate);
    }
    sort_depth = 0;
    record(enabled, caller, count, now_ticks() - started, descents, moved);
}

static void object_sort_hook(VectorIterator *first, VectorIterator *last, int64_t ideal, uint64_t predicate) {
    if (sort_depth) {
        original_object_sort(first, last, ideal, predicate);
        return;
    }
    int caller = caller_of(__builtin_return_address(0));
    int enabled = fixes_enabled != 0;
    size_t count = last->index - first->index, descents = 0;
    long long moved = 0;
    if (!enabled) descents = count_object_descents(first->vector, first->index, last->index);
    LONG64 started = now_ticks();
    sort_depth = 1;
    if (!enabled) {
        original_object_sort(first, last, ideal, predicate);
    } else if ((descents = count_object_descents(first->vector, first->index, last->index)) &&
               (moved = sort_vector_objects(first->vector, first->index, last->index)) < 0) {
        InterlockedIncrement(&sortfix_allocation_failures);
        moved = 0;
        original_object_sort(first, last, ideal, predicate);
    }
    sort_depth = 0;
    record(enabled, caller, count, now_ticks() - started, descents, moved);
}

static void write_jump(BYTE *at, const void *destination) {
    at[0] = 0xff;
    at[1] = 0x25;
    memset(at + 2, 0, 4);
    memcpy(at + 6, &destination, sizeof destination);
}

// Other threads are suspended so none can execute a half-written prologue.
const char *patch_code(BYTE *target, const BYTE *patch, size_t size) {
    static HANDLE handles[MAX_PATCH_THREADS];
    int count = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return "thread snapshot failed";
    THREADENTRY32 entry = {.dwSize = sizeof entry};
    DWORD process_id = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL more = Thread32First(snapshot, &entry); more && count < MAX_PATCH_THREADS; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == self) continue;
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (handle) handles[count++] = handle;
    }
    CloseHandle(snapshot);
    const char *result = "a thread kept executing the patch site";
    for (int attempt = 0; attempt < PATCH_ATTEMPTS && result; attempt++) {
        for (int i = 0; i < count; i++) SuspendThread(handles[i]);
        BOOL busy = FALSE;
        for (int i = 0; i < count && !busy; i++) {
            CONTEXT context = {.ContextFlags = CONTEXT_CONTROL};
            busy = GetThreadContext(handles[i], &context) && context.Rip >= (DWORD64)target && context.Rip < (DWORD64)(target + size);
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
    return result;
}

// A target hooked before keeps its trampoline and only has its jump redirected, so a reloaded module replaces the earlier hook.
const char *hook_function(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original) {
    BYTE patch[MAX_HOOK_PATCH];
    write_jump(patch, replacement);
    for (int i = 0; i < installed_hook_count; i++)
        if (installed_hooks[i].target == target) {
            *original = installed_hooks[i].trampoline;
            return patch_code(target, patch, JUMP_SIZE);
        }
    if (installed_hook_count == MAX_HOOKS) return "all hook slots used";
    if (size < JUMP_SIZE || size > MAX_HOOK_PATCH) return "prologue size must fit the jump and the patch buffer";
    if (memcmp(target, prologue, size)) return "engine code differs from the analysed build";
    BYTE *trampoline = VirtualAlloc(NULL, size + JUMP_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return "VirtualAlloc for a trampoline failed";
    memcpy(trampoline, target, size);
    write_jump(trampoline + size, target + size);
    FlushInstructionCache(GetCurrentProcess(), trampoline, size + JUMP_SIZE);
    *original = trampoline;
    memset(patch + JUMP_SIZE, 0xcc, size - JUMP_SIZE);
    const char *error = patch_code(target, patch, size);
    if (!error) installed_hooks[installed_hook_count++] = (InstalledHook){target, trampoline};
    return error;
}

const char *sortfix_install(void) {
    BYTE *engine = (BYTE *)GetModuleHandleW(L"empire.retail.x64.dll");
    if (!engine) return "empire.retail.x64.dll not loaded";
    // Both prologues are checked before either is patched, so a mismatched build is left untouched.
    if (memcmp(engine + RENDER_SORT_RVA, render_sort_prologue, PROLOGUE_CHECK_SIZE) ||
        memcmp(engine + OBJECT_SORT_RVA, object_sort_prologue, PROLOGUE_CHECK_SIZE))
        return "engine code differs from the analysed build";
    caller_returns[0] = (uintptr_t)engine + RENDER_LIST_RETURN_RVA;
    caller_returns[1] = (uintptr_t)engine + OTHER_LIST_RETURN_RVA;
    caller_returns[2] = (uintptr_t)engine + OBJECT_LIST_RETURN_RVA;
    const char *error =
        hook_function(engine + RENDER_SORT_RVA, render_sort_prologue, PATCH_SIZE, (const void *)render_sort_hook, (void **)&original_render_sort);
    if (error) return error;
    return hook_function(engine + OBJECT_SORT_RVA, object_sort_prologue, PATCH_SIZE, (const void *)object_sort_hook, (void **)&original_object_sort);
}

void sortfix_reset_stats(void) {
    memset(sort_stats, 0, sizeof sort_stats);
    sortfix_allocation_failures = 0;
}
