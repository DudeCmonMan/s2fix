#include "../s2module.h"
#include "../checks/packindex.h"

// Each pack the engine opens gets a table of its files (0x181859f90), filled from the pack's index: a lookup, then an
// insert (0x1818f67a0), per file. The table starts at 8 slots and probes linearly, two string compares per slot, and
// grows (2n+1) only once a full scan finds no free slot, so the scans run longer and longer as it fills, and a big
// pack's table can end almost full. Here a pack's first insert sizes the still-empty table to twice its file count, so
// it never grows and stays at most half full. Lookups find the same entries at any size, and the engine's only walks
// of the table (releasing the pack, listing its files at a reset) don't depend on which slot an entry is in. The insert
// serves other tables too; only the pack open's call is sized.
#define INSERT_RVA 0x18f67a0
#define PACK_OPEN_RETURN_RVA 0x185a515
#define ALLOCATOR_RVA 0x2ead968
#define RELEASER_RVA 0x2ead970
#define ALIGNED_MALLOC_RVA 0x1da7eb0
#define ALIGNED_FREE_RVA 0x1da7e80
#define PACK_TABLE 0x10
#define PACK_FILE_COUNT 0x18
#define ENGINE_START_SLOTS 8

typedef struct {
    void **slots;
    size_t capacity;
    void *empty;
} Table;

typedef void *(*InsertFn)(Table *table, void *const *key, void *const *value);
typedef void *(*AllocFn)(size_t size, size_t alignment);
typedef void (*FreeFn)(void *memory);

// 0 leaves the engine's tables at 8 slots; read at each pack's first insert.
__declspec(dllexport) volatile LONG sizing_enabled = 1;
__declspec(dllexport) volatile LONG tables_sized;

static const S2Api *api;
static InsertFn original_insert;
static const BYTE *pack_open_return;
static AllocFn *allocator;
static FreeFn *releaser;
static AllocFn aligned_malloc;
static FreeFn aligned_free;
static const BYTE insert_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10,
                                       0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20};

static BOOL empty(const Table *table) {
    for (size_t i = 0; i < table->capacity; i++)
        if (table->slots[2 * i] != table->empty) return FALSE;
    return TRUE;
}

// The engine's own allocation and release for these tables.
static void resize(Table *table, size_t capacity) {
    AllocFn allocate = *allocator ? *allocator : aligned_malloc;
    void **slots = allocate(capacity * 16, 8);
    for (size_t i = 0; i < capacity; i++) slots[2 * i] = table->empty;
    FreeFn release = *releaser ? *releaser : aligned_free;
    release(table->slots);
    table->slots = slots;
    table->capacity = capacity;
}

static void *insert_hook(Table *table, void *const *key, void *const *value) {
    if (__builtin_return_address(0) == pack_open_return && table->capacity == ENGINE_START_SLOTS && sizing_enabled && *api->fix_enabled) {
        DWORD files = *(DWORD *)(*(BYTE **)((BYTE *)table - PACK_TABLE) + PACK_FILE_COUNT);
        if (files > ENGINE_START_SLOTS && empty(table)) {
            resize(table, (size_t)files * 2 + 1);
            InterlockedIncrement(&tables_sized);
            api->log_line("packindex: a pack of %lu files gets a table of %llu slots", files, (unsigned long long)table->capacity);
        }
    }
    return original_insert(table, key, value);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    const char *error = S2_CHECK_ENGINE(api, packindex_checks);
    if (error) return error;
    pack_open_return = api->engine + PACK_OPEN_RETURN_RVA;
    allocator = (AllocFn *)(api->engine + ALLOCATOR_RVA);
    releaser = (FreeFn *)(api->engine + RELEASER_RVA);
    aligned_malloc = (AllocFn)(void *)(api->engine + ALIGNED_MALLOC_RVA);
    aligned_free = (FreeFn)(void *)(api->engine + ALIGNED_FREE_RVA);
    error = api->hook(api->engine + INSERT_RVA, insert_prologue, sizeof insert_prologue, (const void *)insert_hook, (void **)&original_insert);
    if (error) return error;
    api->register_switch("packindex", &sizing_enabled, NULL);
    return NULL;
}
