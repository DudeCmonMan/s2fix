#include "s2module.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// Checks modules/lookupcache.dll against a stand-in engine whose import slots hold the real functions, with a scratch
// data\ folder beside this program (the module takes the data folder beside the executable): a missing data file is
// answered from memory after Windows said so once, with the same error; files that exist, paths outside the data
// folder, wildcard searches and the switch turned off always go to Windows; a write open in the data folder drops what
// was remembered. A mod's folder counts as the data folder does: one whose pack is open when the module loads, and one
// whose pack the engine opens later.
#define ENGINE_BYTES 0x2200000

static BYTE *engine;
static volatile LONG fixes_on = 1;
static int failures, checks;

static void log_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}
static const char *patch(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size) {
    if (memcmp(target, expected, size)) return "slot differs";
    memcpy(target, replacement, size);
    return NULL;
}
static void register_switch(const char *name, volatile LONG *flag, void (*report)(BOOL)) {
    (void)name, (void)flag, (void)report;
}

static void expect(const char *what, BOOL ok) {
    checks++;
    failures += !ok;
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
}

int main(void) {
    engine = VirtualAlloc(NULL, ENGINE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    static const struct {
        DWORD rva;
        const char *name;
    } slots[] = {{0x215a6f0, "GetFileAttributesW"}, {0x215a328, "GetFileAttributesExW"}, {0x215a4c8, "FindFirstFileW"}, {0x215a530, "CreateFileW"}};
    for (size_t i = 0; i < sizeof slots / sizeof *slots; i++) *(FARPROC *)(engine + slots[i].rva) = GetProcAddress(kernel32, slots[i].name);
    wchar_t folder[MAX_PATH], data[MAX_PATH], missing[MAX_PATH], missing_folder[MAX_PATH], present[MAX_PATH], outside[MAX_PATH], pattern[MAX_PATH];
    GetModuleFileNameW(NULL, folder, MAX_PATH);
    *wcsrchr(folder, L'\\') = 0;
    swprintf(data, MAX_PATH, L"%ls\\data", folder);
    swprintf(missing, MAX_PATH, L"%ls\\data\\ui\\missing.dds", folder);
    swprintf(missing_folder, MAX_PATH, L"%ls\\data\\no_such_folder\\x.txt", folder);
    swprintf(present, MAX_PATH, L"%ls\\data\\present.txt", folder);
    swprintf(outside, MAX_PATH, L"%ls\\lookupcache_outside.txt", folder);
    swprintf(pattern, MAX_PATH, L"%ls\\data\\*.pack", folder);
    BOOL made_data = CreateDirectoryW(data, NULL);
    wchar_t ui[MAX_PATH];
    swprintf(ui, MAX_PATH, L"%ls\\data\\ui", folder);
    CreateDirectoryW(ui, NULL);
    CloseHandle(CreateFileW(present, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    wchar_t mod[MAX_PATH], mod_pack[MAX_PATH], mod_missing[MAX_PATH], late[MAX_PATH], late_pack[MAX_PATH], late_missing[MAX_PATH];
    swprintf(mod, MAX_PATH, L"%ls\\lookupcache_mod", folder);
    swprintf(mod_pack, MAX_PATH, L"%ls\\lookupcache_mod\\mod.pack", folder);
    swprintf(mod_missing, MAX_PATH, L"%ls\\lookupcache_mod\\rigidmodels\\icon.rigid_model_v2", folder);
    swprintf(late, MAX_PATH, L"%ls\\lookupcache_late", folder);
    swprintf(late_pack, MAX_PATH, L"%ls\\lookupcache_late\\late.pack", folder);
    swprintf(late_missing, MAX_PATH, L"%ls\\lookupcache_late\\icon.dds", folder);
    CreateDirectoryW(mod, NULL);
    CreateDirectoryW(late, NULL);
    CloseHandle(CreateFileW(late_pack, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    // Held open while the module loads, as the engine holds its packs.
    HANDLE mod_handle = CreateFileW(mod_pack, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);

    S2Api api = {.engine = engine, .fix_enabled = &fixes_on, .log_line = log_line, .patch = patch, .register_switch = register_switch};
    HMODULE module = LoadLibraryW(L"modules\\lookupcache.dll");
    S2ModuleMain module_main = module ? (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main") : NULL;
    const char *error = module_main ? module_main(&api) : "modules\\lookupcache.dll not found";
    expect("installs", !error);
    if (error) return printf("%s\n", error), 1;
    volatile LONG *answered = (volatile LONG *)(void *)GetProcAddress(module, "answered");
    volatile LONG *enabled = (volatile LONG *)(void *)GetProcAddress(module, "cache_enabled");
    typedef DWORD(WINAPI * AttributesFn)(LPCWSTR);
    typedef BOOL(WINAPI * AttributesExFn)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    typedef HANDLE(WINAPI * FindFn)(LPCWSTR, LPWIN32_FIND_DATAW);
    typedef HANDLE(WINAPI * OpenFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    AttributesFn attributes = *(AttributesFn *)(engine + 0x215a6f0);
    AttributesExFn attributes_ex = *(AttributesExFn *)(engine + 0x215a328);
    FindFn find = *(FindFn *)(engine + 0x215a4c8);
    OpenFn open = *(OpenFn *)(engine + 0x215a530);

    LONG before = *answered;
    SetLastError(0);
    BOOL first = attributes(missing) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
    expect("a missing data file asks Windows the first time", first && *answered == before);
    SetLastError(0);
    BOOL again = attributes(missing) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
    expect("the same check again is answered from memory, with Windows' error", again && *answered == before + 1);
    WIN32_FILE_ATTRIBUTE_DATA info;
    WIN32_FIND_DATAW found;
    SetLastError(0);
    BOOL other_calls = !attributes_ex(missing, GetFileExInfoStandard, &info) && GetLastError() == ERROR_FILE_NOT_FOUND &&
                       find(missing, &found) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND &&
                       open(missing, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE &&
                       GetLastError() == ERROR_FILE_NOT_FOUND;
    expect("attributes, an exact search and a read open of it are answered from memory too", other_calls && *answered == before + 4);
    wchar_t mixed[MAX_PATH];
    swprintf(mixed, MAX_PATH, L"%ls\\DATA\\UI\\..\\ui\\Missing.DDS", folder);
    attributes(mixed);
    expect("the same file written another way is the same check", *answered == before + 5);
    attributes(missing_folder);
    SetLastError(0);
    BOOL folder_error = attributes(missing_folder) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_PATH_NOT_FOUND;
    expect("a missing folder keeps its own error", folder_error && *answered == before + 6);

    LONG at = *answered;
    BOOL exists = attributes(present) != INVALID_FILE_ATTRIBUTES && attributes(present) != INVALID_FILE_ATTRIBUTES;
    HANDLE opened = open(present, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    expect("a file that exists is always found on disk", exists && opened != INVALID_HANDLE_VALUE && *answered == at);
    CloseHandle(opened);
    attributes(outside), attributes(outside);
    expect("a path outside the data folder always asks Windows", *answered == at);
    HANDLE search = find(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    find(pattern, &found);
    expect("a wildcard search always asks Windows", *answered == at);
    *enabled = 0;
    attributes(missing);
    expect("with the switch off every check asks Windows", *answered == at);
    *enabled = 1;

    at = *answered;
    attributes(mod_missing);
    SetLastError(0);
    BOOL mod_again = attributes(mod_missing) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_PATH_NOT_FOUND;
    expect("a missing file in the folder of a pack open as the module loaded is answered from memory", mod_again && *answered == at + 1);
    attributes(late_missing), attributes(late_missing);
    expect("a mod's folder is not known before its pack is opened", *answered == at + 1);
    CloseHandle(open(late_pack, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL));
    attributes(late_missing), attributes(late_missing);
    expect("once the engine opens a mod's pack, missing files in its folder are answered from memory", *answered == at + 2);

    HANDLE made = CreateFileW(missing, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CloseHandle(made);
    expect("a file added behind the engine's back stays missing for the session", attributes(missing) == INVALID_FILE_ATTRIBUTES);
    HANDLE written = open(present, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CloseHandle(written);
    expect("after the engine opens a data file for writing, checks ask Windows again", attributes(missing) != INVALID_FILE_ATTRIBUTES);

    DeleteFileW(missing);
    DeleteFileW(present);
    CloseHandle(mod_handle);
    DeleteFileW(mod_pack);
    DeleteFileW(late_pack);
    RemoveDirectoryW(mod);
    RemoveDirectoryW(late);
    RemoveDirectoryW(ui);
    if (made_data) RemoveDirectoryW(data);
    printf("test_lookupcache: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
