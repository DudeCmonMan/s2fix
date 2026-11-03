#include "../s2module.h"

// Miles' filter counts its samples down before testing them, so a block with no samples during a filter fade runs on
// through memory until it faults. There is nothing to filter in such a block, so that one call is skipped.
#define MILES_TIME_STAMP 0x4bf2900c
#define FILTER_CALL_RVA 0x19972
#define FILTER_RVA 0x16bf0
#define FADE_WORD 7
#define FADE_SAMPLES 0xffff

typedef int(__attribute__((regparm(3))) *FilterFn)(void *source, DWORD samples, DWORD unused, void *destination, const DWORD *state);

__declspec(dllexport) FilterFn filter;
__declspec(dllexport) volatile LONG blocks_skipped;

static const S2Api *api;
static const BYTE call_original[5] = {0xe8, 0x79, 0xd2, 0xff, 0xff};

__declspec(dllexport) int __attribute__((regparm(3))) filter_guard(void *source, DWORD samples, DWORD unused, void *destination, const DWORD *state) {
    if (samples || !(state[FADE_WORD] & FADE_SAMPLES)) return filter(source, samples, unused, destination, state);
    if (InterlockedIncrement(&blocks_skipped) == 1) api->log_line("soundfilter: skipped a sound block with no samples, where Miles would have crashed");
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    BYTE *miles = (BYTE *)GetModuleHandleW(L"mss32.dll");
    if (!miles) return "mss32.dll is not loaded";
    const IMAGE_NT_HEADERS *headers = (const IMAGE_NT_HEADERS *)(miles + ((const IMAGE_DOS_HEADER *)miles)->e_lfanew);
    if (headers->FileHeader.TimeDateStamp != MILES_TIME_STAMP) return "mss32.dll is not the build the fix was made for";
    filter = (FilterFn)(void *)(miles + FILTER_RVA);
    BYTE *call = miles + FILTER_CALL_RVA;
    BYTE call_replacement[5] = {0xe8};
    *(LONG *)(call_replacement + 1) = (LONG)((BYTE *)filter_guard - (call + 5));
    return api->patch(call, call_original, call_replacement, sizeof call_replacement);
}
