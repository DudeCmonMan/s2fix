#pragma once
#include <windows.h>

#define SORT_CALLERS 4

typedef struct {
    volatile LONG64 calls, items, ticks, max_items, presorted, descents, moved;
} SortStats;

extern volatile LONG fixes_enabled;
// 1 tries the counting sort over distinct keys before the radix (a switch).
extern volatile LONG few_keys_enabled;
// The flag whose value picks the stats row a sort is counted in: Apply Fixes, or the switch an A/B capture flips.
extern volatile LONG *volatile sort_mode_flag;
extern volatile LONG sortfix_allocation_failures;
extern SortStats sort_stats[2][SORT_CALLERS];
extern const char *const sort_caller_names[SORT_CALLERS];

const char *sortfix_install(void);
const char *patch_code(BYTE *target, const BYTE *patch, size_t size);
const char *patch_checked(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size);
const char *hook_function(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original);
void sortfix_reset_stats(void);
