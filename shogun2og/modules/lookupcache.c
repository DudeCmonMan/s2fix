#include "../s2module.h"
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

// Before reading a file from its packs, the engine asks Windows whether a loose copy is in the data folder (mods can
// override pack files there): over 100,000 checks in a campaign load, nearly all for files that are not there, many
// asked again and again. Each "no such file" answer Windows gives for a path in the data folder is remembered for the
// rest of the session, and later checks of that path are answered from memory. The cache holds only what the engine has
// asked during play, never a list or a scan of the folder, so a loose file that exists is always found on disk; it keeps
// filling on every load. The game never writes into its data folder; if the engine opens a file there for writing
// anyway, the cache is emptied. A loose file added while the game runs is seen after a restart.
#define GET_FILE_ATTRIBUTES_W_IAT_RVA 0x15a1270
#define GET_FILE_ATTRIBUTES_EX_W_IAT_RVA 0x15a1058
#define FIND_FIRST_FILE_W_IAT_RVA 0x15a114c
#define CREATE_FILE_W_IAT_RVA 0x15a1144
#define KEY_CHARS 520
#define MAX_BYTES (16 << 20)
#define REPORT_SECONDS 10
#define WRITE_ACCESS (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER)

typedef struct {
    DWORD hash, length, error;
    wchar_t text[];
} Missing;

// 0 asks Windows every time; exported, and a switch, so it can be A/B tested live. The counts are for the test and log.
__declspec(dllexport) volatile LONG cache_enabled = 1;
__declspec(dllexport) volatile LONG answered, remembered;

static const S2Api *api;
static wchar_t data_folder[KEY_CHARS];
static DWORD data_length;
static SRWLOCK lock = SRWLOCK_INIT;
static HANDLE heap;
static Missing **slots;
static DWORD slot_count, used;
static SIZE_T bytes;
static volatile LONG full, emptied;

static DWORD(WINAPI *real_GetFileAttributesW)(LPCWSTR);
static BOOL(WINAPI *real_GetFileAttributesExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
static HANDLE(WINAPI *real_FindFirstFileW)(LPCWSTR, LPWIN32_FIND_DATAW);
static HANDLE(WINAPI *real_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static BOOL active(void) {
    return cache_enabled && *api->fix_enabled;
}

// The path as a key (full, lower case, backslashes) and its length; 0 when it is not in the data folder.
static DWORD make_key(LPCWSTR path, wchar_t *key) {
    if (!path || wcspbrk(path, L"*?")) return 0;
    DWORD length = GetFullPathNameW(path, KEY_CHARS, key, NULL);
    if (!length || length >= KEY_CHARS) return 0;
    for (DWORD i = 0; i < length; i++) key[i] = key[i] == L'/' ? L'\\' : (wchar_t)towlower(key[i]);
    return length > data_length && !wmemcmp(key, data_folder, data_length) ? length : 0;
}

static DWORD hash_of(const wchar_t *key, DWORD length) {
    DWORD hash = 2166136261u;
    for (DWORD i = 0; i < length; i++) hash = (hash ^ key[i]) * 16777619u;
    return hash;
}

static Missing **slot_for(Missing **table, DWORD count, DWORD hash, const wchar_t *key, DWORD length) {
    for (DWORD i = hash & (count - 1);; i = (i + 1) & (count - 1))
        if (!table[i] || (table[i]->hash == hash && table[i]->length == length && !wmemcmp(table[i]->text, key, length))) return &table[i];
}

static BOOL known_missing(const wchar_t *key, DWORD length, DWORD *error) {
    DWORD hash = hash_of(key, length);
    AcquireSRWLockShared(&lock);
    Missing *entry = slots ? *slot_for(slots, slot_count, hash, key, length) : NULL;
    if (entry) *error = entry->error;
    ReleaseSRWLockShared(&lock);
    if (entry) InterlockedIncrement(&answered);
    return entry != NULL;
}

// Only "no such file" and "no such folder" are kept; the last error is left as Windows set it.
static void remember(const wchar_t *key, DWORD length) {
    DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return;
    DWORD hash = hash_of(key, length);
    AcquireSRWLockExclusive(&lock);
    if (used * 2 >= slot_count) {
        DWORD count = slot_count ? slot_count * 2 : 4096;
        Missing **table = HeapAlloc(heap, HEAP_ZERO_MEMORY, count * sizeof *table);
        if (table) {
            for (DWORD i = 0; i < slot_count; i++)
                if (slots[i]) *slot_for(table, count, slots[i]->hash, slots[i]->text, slots[i]->length) = slots[i];
            if (slots) HeapFree(heap, 0, slots);
            slots = table, slot_count = count;
        }
    }
    Missing **slot = used * 2 < slot_count ? slot_for(slots, slot_count, hash, key, length) : NULL;
    SIZE_T size = sizeof(Missing) + length * sizeof(wchar_t);
    if (slot && !*slot && bytes + size <= MAX_BYTES && (*slot = HeapAlloc(heap, 0, size))) {
        **slot = (Missing){hash, length, error};
        wmemcpy((*slot)->text, key, length);
        used++, bytes += size;
        InterlockedIncrement(&remembered);
    } else if (slot && !*slot) {
        full = 1;
    }
    ReleaseSRWLockExclusive(&lock);
    SetLastError(error);
}

static void forget_all(void) {
    AcquireSRWLockExclusive(&lock);
    if (slots) {
        for (DWORD i = 0; i < slot_count; i++)
            if (slots[i]) HeapFree(heap, 0, slots[i]);
        memset(slots, 0, slot_count * sizeof *slots);
    }
    used = 0, bytes = 0, full = 0;
    ReleaseSRWLockExclusive(&lock);
    emptied = 1;
}

static DWORD WINAPI hook_GetFileAttributesW(LPCWSTR path) {
    wchar_t key[KEY_CHARS];
    DWORD length = active() ? make_key(path, key) : 0, error;
    if (length && known_missing(key, length, &error)) {
        SetLastError(error);
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD attributes = real_GetFileAttributesW(path);
    if (length && attributes == INVALID_FILE_ATTRIBUTES) remember(key, length);
    return attributes;
}

static BOOL WINAPI hook_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    wchar_t key[KEY_CHARS];
    DWORD length = active() ? make_key(path, key) : 0, error;
    if (length && known_missing(key, length, &error)) {
        SetLastError(error);
        return FALSE;
    }
    BOOL found = real_GetFileAttributesExW(path, level, info);
    if (length && !found) remember(key, length);
    return found;
}

static HANDLE WINAPI hook_FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    wchar_t key[KEY_CHARS];
    DWORD length = active() ? make_key(path, key) : 0, error;
    if (length && known_missing(key, length, &error)) {
        SetLastError(error);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE find = real_FindFirstFileW(path, data);
    if (length && find == INVALID_HANDLE_VALUE) remember(key, length);
    return find;
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
                                     HANDLE template_file) {
    wchar_t key[KEY_CHARS];
    DWORD length = active() ? make_key(path, key) : 0, error;
    BOOL reading = disposition == OPEN_EXISTING && !(access & WRITE_ACCESS);
    if (length && !reading) forget_all();
    if (length && reading && known_missing(key, length, &error)) {
        SetLastError(error);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE file = real_CreateFileW(path, access, share, security, disposition, flags, template_file);
    if (length && reading && file == INVALID_HANDLE_VALUE) remember(key, length);
    return file;
}

static DWORD WINAPI reporter(LPVOID unused) {
    (void)unused;
    LONG reported = 0;
    for (;;) {
        Sleep(REPORT_SECONDS * 1000);
        if (emptied) emptied = 0, api->log_line("lookupcache: the engine opened a file in the data folder for writing; the remembered checks were dropped");
        LONG now = answered;
        if (now == reported) continue;
        api->log_line("lookupcache: %ld checks for missing data files answered from memory so far, %ld files remembered%s", now, remembered,
                      full ? " (full: later misses go to Windows)" : "");
        reported = now;
    }
    return 0;
}

#define IMPORT(rva, name) {rva, #name, (void *)hook_##name, (void **)&real_##name}

static const struct {
    DWORD rva;
    const char *name;
    void *hook;
    void **real;
} imports[] = {IMPORT(GET_FILE_ATTRIBUTES_W_IAT_RVA, GetFileAttributesW), IMPORT(GET_FILE_ATTRIBUTES_EX_W_IAT_RVA, GetFileAttributesExW),
               IMPORT(FIND_FIRST_FILE_W_IAT_RVA, FindFirstFileW), IMPORT(CREATE_FILE_W_IAT_RVA, CreateFileW)};

// The data folder sits beside the game's executable.
static BOOL find_data_folder(void) {
    DWORD length = GetModuleFileNameW(NULL, data_folder, KEY_CHARS - 8);
    wchar_t *slash = length && length < KEY_CHARS - 8 ? wcsrchr(data_folder, L'\\') : NULL;
    if (!slash) return FALSE;
    wcscpy(slash, L"\\data\\");
    data_length = (DWORD)wcslen(data_folder);
    for (DWORD i = 0; i < data_length; i++) data_folder[i] = (wchar_t)towlower(data_folder[i]);
    return TRUE;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    static char error[96];
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    for (size_t i = 0; i < sizeof imports / sizeof *imports; i++)
        if (*(void **)(api->engine + imports[i].rva) != (void *)GetProcAddress(kernel32, imports[i].name)) {
            snprintf(error, sizeof error, "the import slot does not hold %s", imports[i].name);
            return error;
        }
    if (!find_data_folder()) return "the game's folder could not be found";
    if (!(heap = HeapCreate(HEAP_NO_SERIALIZE, 0, 0))) return "HeapCreate failed";
    for (size_t i = 0; i < sizeof imports / sizeof *imports; i++) {
        BYTE *slot = api->engine + imports[i].rva;
        *imports[i].real = *(void **)slot;
        const char *patch_error = api->patch(slot, (const BYTE *)imports[i].real, (const BYTE *)&imports[i].hook, sizeof imports[i].hook);
        if (patch_error) return patch_error;
    }
    HANDLE thread = CreateThread(NULL, 0, reporter, NULL, 0, NULL);
    if (!thread) return "CreateThread failed";
    CloseHandle(thread);
    api->register_switch("lookupcache", &cache_enabled, NULL);
    return NULL;
}
