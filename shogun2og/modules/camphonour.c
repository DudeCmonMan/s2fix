#include "../s2module.h"
#include "../checks/camphonour.h"
#include <intrin.h>
#include <stdint.h>
#include <string.h>

// Campaign map: every settlement's map marker and label work out the settlement's projected public order, and with it
// the daimyo's honour, once per province. One of the 22 honour factors is an effect value the engine reads from a list
// it builds for that one read: a copy of the faction's whole effect list, merge-summed with the leader's. The merge
// stores leader + faction for a key both have and keeps the rest in order, so for sorted lists the value read is the two
// lists' own values for that key added; two lookups replace the copy and the merge. Only on those two display paths.
#define STATUS_RVA 0x8b3920
#define UNREST_TEST_RVA 0x8b9cd0
#define FACTORS_RVA 0x5c62c0
#define LEADER_RVA 0x5cc3b0
#define HANDLE_RVA 0x3e5960
#define VALUE_BY_ID_RVA 0xbcf300
#define FTOL_RVA 0x14fcc30
#define SET_FACTOR_RVA 0x5f7220

#define HOLDER_FACTORS 0xa34
#define FACTOR_BYTES 22
#define LEADER_FACTION 0x254
#define LEADER_LIST 0x498
#define FACTION_LIST 0x6c4
#define EFFECT_FACTOR 0xb
#define HONOUR_EFFECT 0xd2
#define REPORT_MS 10000

typedef uintptr_t(__attribute__((thiscall)) *StatusFn)(BYTE *self, BYTE *settlement);
typedef uintptr_t(__attribute__((thiscall)) *UnrestFn)(BYTE *settlement);
typedef BYTE *(__attribute__((thiscall)) *FactorsFn)(BYTE *holder, BYTE *factors);
typedef BYTE *(__attribute__((thiscall)) *LeaderFn)(BYTE *holder);
typedef BYTE *(__attribute__((thiscall)) *HandleFn)(BYTE *handle);
typedef float(__attribute__((thiscall)) *ValueFn)(BYTE *list, uint32_t id);
typedef void(__cdecl *SetFactorFn)(BYTE *factors, BYTE *holder, uint32_t factor, uint32_t value);

// Plain counters: they are only written on the main thread.
typedef struct {
    LONG64 filled, differed;
} Stats;

__declspec(dllexport) volatile LONG honour_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats honour_stats;
const BYTE *camphonour_engine_ftol;

// The engine's float-to-integer conversion, given the value on the x87 stack as the engine's code leaves it there.
uint32_t camphonour_ftol(float value);
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _camphonour_ftol\n"
        "_camphonour_ftol:\n"
        "  fld dword ptr [esp + 4]\n"
        "  jmp dword ptr [_camphonour_engine_ftol]\n"
        ".att_syntax\n");

static const BYTE status_prologue[] = {0x83, 0xec, 0x5c, 0x56, 0x8b, 0x74, 0x24, 0x64};
static const BYTE unrest_prologue[] = {0x56, 0x8b, 0xf1, 0x57, 0x8d, 0x7e, 0x24};
static const BYTE factors_prologue[] = {0x83, 0xec, 0x0c, 0x56, 0x8b, 0x74, 0x24, 0x14};
static const S2Api *api;
static StatusFn original_status;
static UnrestFn original_unrest;
static FactorsFn original_factors;
static LeaderFn leader_of;
static HandleFn handle_target;
static ValueFn value_by_id;
static SetFactorFn set_factor;
static int display_depth;

static BOOL on_main(void) {
    return __readfsdword(0x24) == *api->main_thread;
}

static uintptr_t __attribute__((thiscall)) status_hook(BYTE *self, BYTE *settlement) {
    BOOL main = on_main();
    display_depth += main;
    uintptr_t status = original_status(self, settlement);
    display_depth -= main;
    return status;
}

static uintptr_t __attribute__((thiscall)) unrest_hook(BYTE *settlement) {
    BOOL main = on_main();
    display_depth += main;
    uintptr_t answer = original_unrest(settlement);
    display_depth -= main;
    return answer;
}

// The engine's steps, with the merged list's one value read from its two sources. The leader is asked for twice, as the
// engine does; without a faction behind the leader the engine would read through a null pointer, so that is left to it.
static BYTE *__attribute__((thiscall)) factors_hook(BYTE *holder, BYTE *factors) {
    if (!on_main() || !display_depth || !*api->fix_enabled || !honour_enabled) return original_factors(holder, factors);
    BYTE *leader = leader_of(holder) ? leader_of(holder) : NULL;
    BYTE *faction = leader ? handle_target(leader + LEADER_FACTION) : NULL;
    if (leader && !faction) return original_factors(holder, factors);
    memcpy(factors, holder + HOLDER_FACTORS, FACTOR_BYTES);
    if (leader) {
        float value = value_by_id(leader + LEADER_LIST, HONOUR_EFFECT) + value_by_id(faction + FACTION_LIST, HONOUR_EFFECT);
        set_factor(factors, holder, EFFECT_FACTOR, (BYTE)camphonour_ftol(value));
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
    handle_target = (HandleFn)(void *)(api->engine + HANDLE_RVA);
    value_by_id = (ValueFn)(void *)(api->engine + VALUE_BY_ID_RVA);
    set_factor = (SetFactorFn)(void *)(api->engine + SET_FACTOR_RVA);
    camphonour_engine_ftol = api->engine + FTOL_RVA;
    error = api->hook(api->engine + FACTORS_RVA, factors_prologue, sizeof factors_prologue, (const void *)factors_hook, (void **)&original_factors);
    if (!error)
        error = api->hook(api->engine + STATUS_RVA, status_prologue, sizeof status_prologue, (const void *)status_hook, (void **)&original_status);
    if (!error)
        error = api->hook(api->engine + UNREST_TEST_RVA, unrest_prologue, sizeof unrest_prologue, (const void *)unrest_hook, (void **)&original_unrest);
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("camphonour", &honour_enabled, NULL);
    api->register_switch("camphonourverify", &verify, NULL);
    return NULL;
}
