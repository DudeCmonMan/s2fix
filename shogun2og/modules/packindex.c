#include "../s2module.h"
#include "../checks/packindex.h"

// A pack's file table starts at 8 slots, probes linearly and grows only when full, so big packs' inserts scan ever
// longer; a pack's first insert sizes it to twice its file count. No engine walk depends on an entry's slot.
#define PACK_INSERT_CALL_RVA 0x126ec00
#define INSERT_RVA 0x12e3db0
#define ALLOCATOR_RVA 0x17cc4a4
#define RELEASER_RVA 0x17cc4a8
#define ALIGNED_MALLOC_RVA 0x14feda9
#define ALIGNED_FREE_RVA 0x14fed8f
#define PACK_TABLE 8
#define PACK_FILE_COUNT 0x14
#define ENGINE_START_SLOTS 8

typedef struct {
    DWORD *slots;
    DWORD capacity;
    DWORD empty;
} Table;

typedef void *(__attribute__((thiscall)) *InsertFn)(Table *table, const DWORD *key, const DWORD *value);
typedef void *(__cdecl *AllocFn)(size_t size, size_t alignment);
typedef void(__cdecl *FreeFn)(void *memory);

__declspec(dllexport) volatile LONG sizing_enabled = 1;
__declspec(dllexport) volatile LONG tables_sized;

static const S2Api *api;
static InsertFn engine_insert;
static AllocFn *allocator;
static FreeFn *releaser;
static AllocFn aligned_malloc;
static FreeFn aligned_free;
static const BYTE insert_call_original[5] = {0xe8, 0xab, 0x51, 0x07, 0x00};
static BYTE insert_call_replacement[5];

static BOOL empty(const Table *table) {
    for (DWORD i = 0; i < table->capacity; i++)
        if (table->slots[2 * i] != table->empty) return FALSE;
    return TRUE;
}

// The engine's own allocator and release, since the engine frees the table later.
static void resize(Table *table, DWORD capacity) {
    AllocFn allocate = *allocator ? *allocator : aligned_malloc;
    DWORD *slots = allocate((size_t)capacity * 8, 4);
    for (DWORD i = 0; i < capacity; i++) slots[2 * i] = table->empty;
    FreeFn release = *releaser ? *releaser : aligned_free;
    release(table->slots);
    table->slots = slots;
    table->capacity = capacity;
}

static void *__attribute__((thiscall)) insert_hook(Table *table, const DWORD *key, const DWORD *value) {
    if (table->capacity == ENGINE_START_SLOTS && sizing_enabled && *api->fix_enabled) {
        DWORD files = *(DWORD *)(*(BYTE **)((BYTE *)table - PACK_TABLE) + PACK_FILE_COUNT);
        if (files > ENGINE_START_SLOTS && empty(table)) {
            resize(table, files * 2 + 1);
            InterlockedIncrement(&tables_sized);
            api->log_line("packindex: a pack of %lu files gets a table of %lu slots", files, table->capacity);
        }
    }
    return engine_insert(table, key, value);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    const char *error = S2_CHECK_ENGINE(api, packindex_checks);
    if (error) return error;
    engine_insert = (InsertFn)(void *)(api->engine + INSERT_RVA);
    allocator = (AllocFn *)(api->engine + ALLOCATOR_RVA);
    releaser = (FreeFn *)(api->engine + RELEASER_RVA);
    aligned_malloc = (AllocFn)(void *)(api->engine + ALIGNED_MALLOC_RVA);
    aligned_free = (FreeFn)(void *)(api->engine + ALIGNED_FREE_RVA);
    BYTE *call = api->engine + PACK_INSERT_CALL_RVA;
    insert_call_replacement[0] = 0xe8;
    *(LONG *)(insert_call_replacement + 1) = (LONG)((BYTE *)insert_hook - (call + 5));
    error = api->patch(call, insert_call_original, insert_call_replacement, sizeof insert_call_original);
    if (error) return error;
    api->register_switch("packindex", &sizing_enabled, NULL);
    return NULL;
}
