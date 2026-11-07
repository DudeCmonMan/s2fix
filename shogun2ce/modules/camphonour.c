#include "../s2module.h"
#include "../checks/camphonour.h"
#include <intrin.h>
#include <stdint.h>
#include <string.h>
#include <xmmintrin.h>

// Campaign map: every settlement's map marker and label work out the settlement's projected public order, and with it
// the daimyo's honour, once per province. One of the 22 honour factors is an effect value the engine reads from a list
// it builds for that one read: a copy of the faction's whole effect list, merge-summed with the leader's. The merge
// stores leader + faction for a key both have and keeps the rest in order, so for sorted lists the value read is the two
// lists' own values for that key added; two lookups replace the copy and the merge. Only inside the map markers' update
// and the label layout.
#define MARKERS_RVA 0xb4f9e0
#define LABEL_LAYOUT_RVA 0x1699070
#define FACTORS_RVA 0x757f70
#define LEADER_RVA 0x760bf0
#define VALUE_BY_ID_RVA 0xe9d180
#define FACTOR_TABLE_RVA 0x105c080
#define FACTOR_ROWS_RVA 0x11f8e20

#define HOLDER_FACTORS 0xcc8
#define HOLDER_TABLES 0x110
#define TABLES_DATABASE 0x18
#define DATABASE_FACTORS 0x20
#define FACTOR_BYTES 22
#define LEADER_FACTION 0x3c0
#define LEADER_LIST 0x7b0
#define FACTION_LIST 0x838
#define EFFECT_FACTOR 0xb
#define ROW_MAX 0x48
#define ROW_MIN 0x4c
#define HONOUR_EFFECT 0xd2
#define REPORT_MS 10000

typedef uintptr_t (*MarkersFn)(BYTE *self, uintptr_t first, uintptr_t second);
typedef void (*LayoutFn)(BYTE *labels, BYTE *label, float fade);
typedef BYTE *(*FactorsFn)(BYTE *holder, BYTE *factors);
typedef BYTE *(*LeaderFn)(BYTE *holder);
typedef float (*ValueFn)(BYTE *list, uint32_t id);
typedef BYTE *(*TableFn)(BYTE *database);

// Plain counters: they are only written on the main thread.
typedef struct {
    LONG64 filled, differed;
} Stats;

__declspec(dllexport) volatile LONG honour_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats honour_stats;

static const BYTE markers_prologue[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x44, 0x88, 0x40, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54};
static const BYTE layout_prologue[] = {0x48, 0x8b, 0xc4, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8d, 0xa8, 0x68, 0xfe, 0xff, 0xff};
static const BYTE factors_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x8b, 0x81, 0xd8, 0x0c, 0x00, 0x00};
static const S2Api *api;
static MarkersFn original_markers;
static LayoutFn original_layout;
static FactorsFn original_factors;
static LeaderFn leader_of;
static ValueFn value_by_id;
static TableFn factor_table, factor_rows;
static int display_depth;

static BOOL on_main(void) {
    return __readgsdword(0x48) == *api->main_thread;
}

static uintptr_t markers_hook(BYTE *self, uintptr_t first, uintptr_t second) {
    BOOL main = on_main();
    display_depth += main;
    uintptr_t result = original_markers(self, first, second);
    display_depth -= main;
    return result;
}

static void layout_hook(BYTE *labels, BYTE *label, float fade) {
    BOOL main = on_main();
    display_depth += main;
    original_layout(labels, label, fade);
    display_depth -= main;
}

// The engine's steps, with the merged list's one value read from its two sources, then its inlined store: the value
// truncated, its low byte clamped to the factor's row. The leader is asked for twice, as the engine does; without a
// faction behind the leader the engine would read through a null pointer, so that is left to it.
static BYTE *factors_hook(BYTE *holder, BYTE *factors) {
    if (!on_main() || !display_depth || !*api->fix_enabled || !honour_enabled) return original_factors(holder, factors);
    BYTE *leader = leader_of(holder) ? leader_of(holder) : NULL;
    BYTE **faction_handle = leader ? *(BYTE ***)(leader + LEADER_FACTION) : NULL;
    BYTE *faction = faction_handle ? *faction_handle : NULL;
    if (leader && !faction) return original_factors(holder, factors);
    memcpy(factors, holder + HOLDER_FACTORS, FACTOR_BYTES);
    if (leader) {
        float value = value_by_id(leader + LEADER_LIST, HONOUR_EFFECT) + value_by_id(faction + FACTION_LIST, HONOUR_EFFECT);
        int8_t whole = (int8_t)_mm_cvtt_ss2si(_mm_set_ss(value));
        BYTE *database = *(BYTE **)(*(BYTE **)(holder + HOLDER_TABLES) + TABLES_DATABASE);
        const int8_t *row = *(const int8_t **)(factor_rows(factor_table(*(BYTE **)(database + DATABASE_FACTORS))) + EFFECT_FACTOR * sizeof(void *));
        if (row) factors[EFFECT_FACTOR] = (BYTE)(whole < row[ROW_MIN] ? row[ROW_MIN] : row[ROW_MAX] < whole ? row[ROW_MAX] : whole);
    }
    honour_stats.filled++;
    if (verify) {
        BYTE engine_factors[FACTOR_BYTES];
        original_factors(holder, engine_factors);
        if (memcmp(engine_factors, factors, FACTOR_BYTES)) honour_stats.differed++;
    }
    return factors;
}

static DWORD WINAPI report_loop(LPVOID unused) {
    (void)unused;
    Stats last = {0};
    for (;; Sleep(REPORT_MS)) {
        Stats now = honour_stats;
        LONG64 filled = now.filled - last.filled;
        if (verify && filled)
            api->log_line("camphonour: %lld honour factor fills a second from the two lists; %lld differed from the engine's", filled * 1000 / REPORT_MS,
                          now.differed - last.differed);
        last = now;
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *error = S2_CHECK_ENGINE(host, camphonour_checks);
    if (error) return error;
    api = host;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    leader_of = (LeaderFn)(void *)(api->engine + LEADER_RVA);
    value_by_id = (ValueFn)(void *)(api->engine + VALUE_BY_ID_RVA);
    factor_table = (TableFn)(void *)(api->engine + FACTOR_TABLE_RVA);
    factor_rows = (TableFn)(void *)(api->engine + FACTOR_ROWS_RVA);
    error = api->hook(api->engine + FACTORS_RVA, factors_prologue, sizeof factors_prologue, (const void *)factors_hook, (void **)&original_factors);
    if (!error)
        error = api->hook(api->engine + MARKERS_RVA, markers_prologue, sizeof markers_prologue, (const void *)markers_hook, (void **)&original_markers);
    if (!error)
        error = api->hook(api->engine + LABEL_LAYOUT_RVA, layout_prologue, sizeof layout_prologue, (const void *)layout_hook, (void **)&original_layout);
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("camphonour", &honour_enabled, NULL);
    api->register_switch("camphonourverify", &verify, NULL);
    return NULL;
}
