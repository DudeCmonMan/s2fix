#include "../s2module.h"
#include "../checks/textequal.h"
#include <stdint.h>

// The engine compares a name with literal text by copying the text into a new string from its locked pool, constantly
// in an end turn; this compares in place with the same sign-extending widening: the same answer, without the copy.
#define EQUALS_RVA 0x19b6d0
#define STRING_LENGTH 0x0
#define STRING_CHARACTERS 0x10
#define REPORT_MS 10000

typedef uint8_t (*EqualsFn)(const BYTE *string, const char *text);

// Plain counters: another thread's comparison can lose a count.
typedef struct {
    LONG64 comparisons, mismatches;
} Stats;

__declspec(dllexport) volatile LONG equal_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats equal_stats;

static const BYTE equals_prologue[] = {0x40, 0x53, 0x56, 0x48, 0x83, 0xec, 0x48, 0x48, 0x89, 0x6c, 0x24, 0x68, 0x48, 0xc7, 0xc3, 0xff, 0xff, 0xff, 0xff};
static const S2Api *api;
static EqualsFn original_equals;

static uint8_t equals_hook(const BYTE *string, const char *text) {
    if (!*api->fix_enabled || !equal_enabled) return original_equals(string, text);
    equal_stats.comparisons++;
    size_t length = *(const size_t *)(string + STRING_LENGTH);
    const uint16_t *characters = *(const uint16_t *const *)(string + STRING_CHARACTERS);
    size_t i = 0;
    while (text[i] && i < length && characters[i] == (uint16_t)(int16_t)(int8_t)text[i]) i++;
    uint8_t equal = !text[i] && i == length;
    if (verify && original_equals(string, text) != equal) equal_stats.mismatches++;
    return equal;
}

static DWORD WINAPI report_loop(LPVOID unused) {
    (void)unused;
    Stats last = {0};
    for (;; Sleep(REPORT_MS)) {
        Stats now = equal_stats;
        LONG64 comparisons = now.comparisons - last.comparisons;
        if (verify && comparisons)
            api->log_line("textequal: %lld comparisons a second without a copy", comparisons * 1000 / REPORT_MS);
        if (verify && comparisons) api->log_line("textequal: %lld differed from the engine's comparison", now.mismatches - last.mismatches);
        last = now;
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, textequal_checks);
    if (mismatch) return mismatch;
    api = host;
    const char *error = api->hook(api->engine + EQUALS_RVA, equals_prologue, sizeof equals_prologue, (const void *)equals_hook, (void **)&original_equals);
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("textequal", &equal_enabled, NULL);
    api->register_switch("textequalverify", &verify, NULL);
    return NULL;
}
