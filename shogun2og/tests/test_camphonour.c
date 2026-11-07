#include "s2module.h"
#include "test_effectlists.h"
#include <math.h>
#include <stdio.h>
#include <xmmintrin.h>

// camphonour.dll against a stand-in engine whose honour factor fill builds the merged list and reads it as the engine's
// code does: inside the map status and the unrest test, on the main thread, the module's two lookups give the same
// factors over random lists (duplicate keys, keys in one list or neither, signed zeros, denormals, infinities, NaNs,
// three MXCSR modes) without building a list; everywhere else, and with a switch off, the engine's
// fill runs. Verify agrees with the fill, and flags unsorted lists, where the two ways differ.
#define ENGINE_BYTES 0x1500000
#define STATUS_RVA 0x8b3920
#define UNREST_TEST_RVA 0x8b9cd0
#define FACTORS_RVA 0x5c62c0
#define LEADER_RVA 0x5cc3b0
#define HANDLE_RVA 0x3e5960
#define VALUE_BY_ID_RVA 0xbcf300
#define FTOL_RVA 0x14fcc30
#define SET_FACTOR_RVA 0x5f7220

#define HOLDER_FACTORS 0xa34
#define HOLDER_LEADER 0x10
#define FACTOR_BYTES 22
#define LEADER_FACTION 0x254
#define LEADER_LIST 0x498
#define FACTION_LIST 0x6c4
#define EFFECT_FACTOR 0xb
#define HONOUR_EFFECT 0xd2
#define TRIALS 20000
#define MAX_ENTRIES 24

typedef BYTE *(__attribute__((thiscall)) *FactorsFn)(BYTE *holder, BYTE *factors);
typedef uintptr_t(__attribute__((thiscall)) *StatusFn)(BYTE *self, BYTE *settlement);
typedef uintptr_t(__attribute__((thiscall)) *UnrestFn)(BYTE *settlement);

static BYTE *engine;
static volatile LONG fixes_on = 1, *honour_flag, *verify_flag;
static volatile DWORD main_thread;
static int failures, checks;
static long lists_built, faction_faults, factor_stores;
static S2Api api;
static BYTE holder[0xb00], leader[0x600], faction[0x700], factors_out[FACTOR_BYTES + 2];
static EffectList leader_effects, faction_effects;
static uint32_t random_state = 12345;

static const Effect honour_key = {1, 0, 0, HONOUR_EFFECT, 0};

// The engine's conversion: the x87 value truncated to 64 bits (the indefinite integer for NaN and overflow), popped.
uint64_t stand_in_ftol(void);
uint64_t call_ftol(float value);
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _stand_in_ftol\n"
        "_stand_in_ftol:\n"
        "  sub esp, 12\n"
        "  fnstcw word ptr [esp + 8]\n"
        "  mov ax, word ptr [esp + 8]\n"
        "  or ax, 0x0c00\n"
        "  mov word ptr [esp + 10], ax\n"
        "  fldcw word ptr [esp + 10]\n"
        "  fistp qword ptr [esp]\n"
        "  fldcw word ptr [esp + 8]\n"
        "  mov eax, dword ptr [esp]\n"
        "  mov edx, dword ptr [esp + 4]\n"
        "  add esp, 12\n"
        "  ret\n"
        ".globl _call_ftol\n"
        "_call_ftol:\n"
        "  fld dword ptr [esp + 4]\n"
        "  call _stand_in_ftol\n"
        "  ret\n"
        ".att_syntax\n");

static BYTE *__attribute__((thiscall)) stand_in_leader(BYTE *of) {
    return *(BYTE **)(of + HOLDER_LEADER);
}

static BYTE *__attribute__((thiscall)) stand_in_handle(BYTE *handle) {
    return *(BYTE **)handle;
}

static float __attribute__((thiscall)) stand_in_value(BYTE *list, uint32_t id) {
    Effect key = honour_key;
    key.fourth = (int32_t)id;
    return effects_value((const EffectList *)list, &key);
}

static void __cdecl stand_in_set_factor(BYTE *factors, BYTE *of, uint32_t factor, uint32_t value) {
    (void)of;
    factor_stores++;
    factors[factor] = (BYTE)value;
}

// The engine's fill: the stored factors, then the effect factor read from a list built for it.
static BYTE *__attribute__((thiscall)) stand_in_factors(BYTE *of, BYTE *factors) {
    memcpy(factors, of + HOLDER_FACTORS, FACTOR_BYTES);
    if (!stand_in_leader(of)) return factors;
    BYTE *chief = stand_in_leader(of);
    BYTE *house = stand_in_handle(chief + LEADER_FACTION);
    if (!house) {
        faction_faults++;
        return factors;
    }
    EffectList merged;
    effects_copy(&merged, (const EffectList *)(house + FACTION_LIST));
    effects_merge_sum(&merged, (const EffectList *)(chief + LEADER_LIST));
    lists_built++;
    stand_in_set_factor(factors, of, EFFECT_FACTOR, (BYTE)call_ftol(effects_value(&merged, &honour_key)));
    effects_free(&merged);
    return factors;
}

static BYTE *status_factors, *unrest_factors, other_status_factors[FACTOR_BYTES + 2];
static void (*during_status)(void);
static HANDLE other_inside, other_release;

// Another thread's map status waits inside until released, when the test asks it to.
static uintptr_t __attribute__((thiscall)) stand_in_status(BYTE *self, BYTE *settlement) {
    (void)self;
    BOOL main = GetCurrentThreadId() == main_thread;
    ((FactorsFn)(void *)(engine + FACTORS_RVA))(settlement, main ? status_factors : other_status_factors);
    if (main && during_status) during_status();
    if (!main && other_release) SetEvent(other_inside), WaitForSingleObject(other_release, INFINITE);
    return 2;
}

static uintptr_t __attribute__((thiscall)) stand_in_unrest(BYTE *settlement) {
    ((FactorsFn)(void *)(engine + FACTORS_RVA))(settlement, unrest_factors);
    return 1;
}

static void log_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

static BYTE *jump_target(BYTE *at) {
    return at + 5 + *(LONG *)(at + 1);
}

static void jump_to(BYTE *at, const void *to) {
    at[0] = 0xe9;
    *(LONG *)(at + 1) = (LONG)((const BYTE *)to - (at + 5));
}

// The engine's functions are stand-ins reached by a jump, so the "original" is where that jump went.
static const char *hook(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original) {
    (void)prologue, (void)size;
    if (target[0] != 0xe9) return "not a stand-in";
    *original = jump_target(target);
    jump_to(target, replacement);
    return NULL;
}

static void register_switch(const char *name, volatile LONG *flag, void (*report)(BOOL)) {
    (void)report;
    if (!strcmp(name, "camphonour")) honour_flag = flag;
    if (!strcmp(name, "camphonourverify")) verify_flag = flag;
}

static const char *check_engine(const S2Check *table, int count) {
    (void)table, (void)count;
    return NULL;
}

static void expect(const char *what, BOOL ok) {
    checks++;
    failures += !ok;
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
}

static uint32_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static float random_value(void) {
    static const float special[] = {0.0f, -0.0f, 1.0f, -1.0f, 2.5f, -2.5f, 3.99f, 127.5f, 255.0f, 256.0f, 257.75f, -129.0f, 1e-40f, -1e-40f,
                                    2147483648.0f, 4294967808.0f, 1e18f, -1e18f, 1e30f, INFINITY, -INFINITY, NAN};
    uint32_t pick = next_random() % 4;
    if (pick == 0) return special[next_random() % (sizeof special / sizeof *special)];
    if (pick == 1) return (float)((int)(next_random() % 41) - 20);
    return (float)((int)(next_random() % 2001) - 1000) / 8.0f;
}

// Sorted, with keys close to the honour effect's so that both lists often hold it, sometimes twice.
static void random_list(EffectList *list) {
    effects_free(list);
    uint32_t count = next_random() % (MAX_ENTRIES + 1);
    list->data = effects_alloc(count);
    list->count = list->capacity = count;
    for (uint32_t i = 0; i < count; i++) {
        Effect *e = &list->data[i];
        e->first = next_random() % 4 ? 1 : (int)(next_random() % 3);
        e->second = next_random() % 6 ? 0 : next_random() % 2;
        e->third = 0;
        e->fourth = HONOUR_EFFECT - 3 + (int)(next_random() % 7);
        e->value = random_value();
    }
    for (uint32_t i = 1; i < count; i++)
        for (uint32_t j = i; j > 0 && effect_less(&list->data[j], &list->data[j - 1]); j--) {
            Effect swap = list->data[j];
            list->data[j] = list->data[j - 1];
            list->data[j - 1] = swap;
        }
}

static void random_holder(void) {
    for (int i = 0; i < FACTOR_BYTES; i++) holder[HOLDER_FACTORS + i] = (BYTE)next_random();
    random_list(&leader_effects);
    random_list(&faction_effects);
    memcpy(leader + LEADER_LIST, &leader_effects, sizeof leader_effects);
    memcpy(faction + FACTION_LIST, &faction_effects, sizeof faction_effects);
}

static BYTE engine_factors[FACTOR_BYTES + 2];

static void engine_way(void) {
    memset(engine_factors, 0xcc, sizeof engine_factors);
    stand_in_factors(holder, engine_factors);
}

static void through_status(void) {
    memset(factors_out, 0xcc, sizeof factors_out);
    status_factors = factors_out;
    ((StatusFn)(void *)(engine + STATUS_RVA))(NULL, holder);
}

static void through_unrest(void) {
    memset(factors_out, 0xcc, sizeof factors_out);
    unrest_factors = factors_out;
    ((UnrestFn)(void *)(engine + UNREST_TEST_RVA))(holder);
}

static BOOL same_factors(void) {
    return !memcmp(factors_out, engine_factors, sizeof factors_out);
}

// Random holders through the map status: how many disagree with the engine's fill, and how many lists were built.
static int disagreements(int trials, unsigned int mxcsr, long *built) {
    unsigned int saved = _mm_getcsr();
    int wrong = 0;
    long before = lists_built;
    for (int i = 0; i < trials; i++) {
        random_holder();
        _mm_setcsr(mxcsr);
        through_status();
        long fast_built = lists_built;
        engine_way();
        _mm_setcsr(saved);
        before += lists_built - fast_built;
        wrong += !same_factors();
    }
    *built = lists_built - before;
    return wrong;
}

static DWORD WINAPI status_on_other_thread(LPVOID unused) {
    (void)unused;
    through_status();
    return 0;
}

static BYTE other_factors[FACTOR_BYTES + 2];

static DWORD WINAPI fill_on_other_thread(LPVOID unused) {
    (void)unused;
    ((FactorsFn)(void *)(engine + FACTORS_RVA))(holder, other_factors);
    return 0;
}

static void fill_elsewhere_meanwhile(void) {
    HANDLE thread = CreateThread(NULL, 0, fill_on_other_thread, NULL, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}

int main(void) {
    engine = VirtualAlloc(NULL, ENGINE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    jump_to(engine + STATUS_RVA, (const void *)stand_in_status);
    jump_to(engine + UNREST_TEST_RVA, (const void *)stand_in_unrest);
    jump_to(engine + FACTORS_RVA, (const void *)stand_in_factors);
    jump_to(engine + LEADER_RVA, (const void *)stand_in_leader);
    jump_to(engine + HANDLE_RVA, (const void *)stand_in_handle);
    jump_to(engine + VALUE_BY_ID_RVA, (const void *)stand_in_value);
    jump_to(engine + FTOL_RVA, (const void *)stand_in_ftol);
    jump_to(engine + SET_FACTOR_RVA, (const void *)stand_in_set_factor);
    main_thread = GetCurrentThreadId();
    *(BYTE **)(holder + HOLDER_LEADER) = leader;
    static BYTE *faction_handle;
    faction_handle = faction;
    memcpy(leader + LEADER_FACTION, &faction_handle, sizeof faction_handle);

    api = (S2Api){.engine = engine, .fix_enabled = &fixes_on, .log_line = log_line, .hook = hook, .register_switch = register_switch,
                  .main_thread = &main_thread, .check_engine = check_engine};
    HMODULE module = LoadLibraryW(L"modules\\camphonour.dll");
    S2ModuleMain module_main = module ? (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main") : NULL;
    volatile LONG64 *stats = module ? (volatile LONG64 *)(void *)GetProcAddress(module, "honour_stats") : NULL;
    if (!module_main || !stats) return printf("modules\\camphonour.dll not found\n"), 1;
    expect("installs", !module_main(&api) && honour_flag && verify_flag);

    long built;
    expect("the map status: the engine's factors over random lists", !disagreements(TRIALS, 0x1f80, &built));
    expect("  without building a list", !built);
    expect("  denormals as zero and flushed results", !disagreements(TRIALS, 0x1f80 | 0x8040, &built));
    expect("  rounding toward zero", !disagreements(TRIALS, 0x1f80 | 0x6000, &built));

    random_holder();
    through_unrest();
    engine_way();
    long built_before = lists_built;
    through_unrest();
    expect("the unrest test: the engine's factors without a list", same_factors() && lists_built == built_before);

    built_before = lists_built;
    memset(factors_out, 0xcc, sizeof factors_out);
    ((FactorsFn)(void *)(engine + FACTORS_RVA))(holder, factors_out);
    expect("outside the two display paths the engine's fill runs", lists_built == built_before + 1 && same_factors());

    built_before = lists_built;
    HANDLE thread = CreateThread(NULL, 0, status_on_other_thread, NULL, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    expect("another thread gets the engine's fill", lists_built == built_before + 1);
    built_before = lists_built;
    during_status = fill_elsewhere_meanwhile;
    through_status();
    during_status = NULL;
    expect("  also while the main thread is in the map status", lists_built == built_before + 1);
    built_before = lists_built;
    other_inside = CreateEventW(NULL, FALSE, FALSE, NULL);
    other_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    thread = CreateThread(NULL, 0, status_on_other_thread, NULL, 0, NULL);
    WaitForSingleObject(other_inside, INFINITE);
    memset(factors_out, 0xcc, sizeof factors_out);
    ((FactorsFn)(void *)(engine + FACTORS_RVA))(holder, factors_out);
    SetEvent(other_release);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(other_inside);
    CloseHandle(other_release);
    other_release = NULL;
    expect("main outside the map status while another thread is in one: the engine's fill", lists_built == built_before + 2);

    *honour_flag = 0;
    built_before = lists_built;
    through_status();
    expect("switch off: the engine's fill", lists_built == built_before + 1 && same_factors());
    *honour_flag = 1;
    fixes_on = 0;
    built_before = lists_built;
    through_status();
    expect("Apply Fixes off: the engine's fill", lists_built == built_before + 1 && same_factors());
    fixes_on = 1;

    *(BYTE **)(holder + HOLDER_LEADER) = NULL;
    long stores_before = factor_stores;
    through_status();
    engine_way();
    expect("no leader: the stored factors only, as the engine's", same_factors() && factor_stores == stores_before);
    *(BYTE **)(holder + HOLDER_LEADER) = leader;

    static BYTE *no_faction;
    memcpy(leader + LEADER_FACTION, &no_faction, sizeof no_faction);
    long faults_before = faction_faults;
    through_status();
    expect("a leader without a faction is left to the engine", faction_faults == faults_before + 1);
    memcpy(leader + LEADER_FACTION, &faction_handle, sizeof faction_handle);

    *verify_flag = 1;
    LONG64 differed_before = stats[1];
    expect("verify: no disagreement over random lists", !disagreements(TRIALS / 4, 0x1f80, &built) && stats[1] == differed_before);
    // An unsorted leader list: the merged list's lookup finds the honour key, a lookup of the leader's own list does not.
    random_holder();
    effects_free(&leader_effects);
    effects_free(&faction_effects);
    faction_effects.data = effects_alloc(2);
    faction_effects.count = faction_effects.capacity = 2;
    faction_effects.data[0] = honour_key, faction_effects.data[0].fourth = HONOUR_EFFECT - 1, faction_effects.data[0].value = 1.0f;
    faction_effects.data[1] = honour_key, faction_effects.data[1].value = 7.0f;
    leader_effects.data = effects_alloc(2);
    leader_effects.count = leader_effects.capacity = 2;
    leader_effects.data[0] = honour_key, leader_effects.data[0].value = 4.0f;
    leader_effects.data[1] = honour_key, leader_effects.data[1].fourth = HONOUR_EFFECT - 2, leader_effects.data[1].value = 2.0f;
    memcpy(leader + LEADER_LIST, &leader_effects, sizeof leader_effects);
    memcpy(faction + FACTION_LIST, &faction_effects, sizeof faction_effects);
    through_status();
    expect("verify flags an unsorted list, where the two ways differ", stats[1] == differed_before + 1);
    *verify_flag = 0;

    effects_free(&leader_effects);
    effects_free(&faction_effects);
    expect("every list the engine's fill built was freed", effect_blocks == 0);
    printf("%d of %d checks failed\n", failures, checks);
    return failures != 0;
}
