#include "../s2module.h"
#include <stdio.h>
#include <tlhelp32.h>

// The game's OpenSSL (0.9.8k, libeay32.dll) seeds its random pool from Windows' random generator and from the state
// of the system, which includes up to 80 entries of every heap of the process, read through toolhelp. Windows copies
// the list of every block of every heap for each entry it reads, so the cost grows with what the game has loaded (more
// with mods), and the game's start-up seeds twice (curl's set-up seeds once without marking the pool seeded, so its
// first connection seeds again while the game waits). OpenSSL looks the toolhelp functions up by name on each seeding;
// given a Heap32First that finds no entries, it keeps every other source and skips the heap entries. Its random
// numbers serve only curl's network encryption.
#define GET_PROC_ADDRESS_IAT_RVA 0xa20b8
#define VERSION "OpenSSL 0.9.8k "

// 0 lets OpenSSL read the heap entries; exported, and a switch, like the other fixes.
__declspec(dllexport) volatile LONG seed_enabled = 1;
__declspec(dllexport) volatile LONG seedings_eased;

static const S2Api *api;
static FARPROC(WINAPI *real_GetProcAddress)(HMODULE, LPCSTR);

static BOOL WINAPI no_heap_entries(HEAPENTRY32 *entry, DWORD process, ULONG_PTR heap) {
    (void)entry, (void)process, (void)heap;
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

static FARPROC WINAPI hook_GetProcAddress(HMODULE module, LPCSTR name) {
    if (seed_enabled && *api->fix_enabled && !IS_INTRESOURCE(name) && !strcmp(name, "Heap32First")) {
        api->log_line("randseed: OpenSSL seeds without the heap entries (%ld)", InterlockedIncrement(&seedings_eased));
        return (FARPROC)(void *)no_heap_entries;
    }
    return real_GetProcAddress(module, name);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    BYTE *openssl = (BYTE *)GetModuleHandleW(L"libeay32.dll");
    if (!openssl) return "libeay32.dll is not loaded";
    const char *(__cdecl *version)(int) = (const char *(__cdecl *)(int))(void *)GetProcAddress((HMODULE)openssl, "SSLeay_version");
    if (!version || strncmp(version(0), VERSION, strlen(VERSION))) return "libeay32.dll is not the OpenSSL 0.9.8k the fix was made for";
    BYTE *slot = openssl + GET_PROC_ADDRESS_IAT_RVA;
    real_GetProcAddress = (FARPROC(WINAPI *)(HMODULE, LPCSTR))(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress");
    if (*(void **)slot != (void *)real_GetProcAddress) return "libeay32's import slot does not hold GetProcAddress";
    void *replacement = (void *)hook_GetProcAddress;
    const char *error = api->patch(slot, (const BYTE *)&real_GetProcAddress, (const BYTE *)&replacement, sizeof replacement);
    if (error) return error;
    api->register_switch("randseed", &seed_enabled, NULL);
    api->log_line("randseed: OpenSSL will seed without reading the heaps' entries");
    return NULL;
}
