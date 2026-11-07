#include "s2module.h"
#include "test_effectlists.h"
#include <stdio.h>
#include <xmmintrin.h>

// campeffects.dll's settlement projection against a stand-in engine: inside the map status and the unrest test, on the
// main thread, the projection equals the engine's, takes its region's sum from the cache while every source list is
// byte for byte the same (three lists, or five with a faction), and the engine's sum again after any list, count, shape
// or MXCSR mode changes. A projection that changes its list leaves the cache as the engine built it. Elsewhere, on
// another thread or with a switch off, the engine's projection runs; nothing leaks; verify flags a stale sum.
#define ENGINE_BYTES 0x1500000
#define CHECK_RVA 0x7818d0
#define EFFECT_OWNER_RVA 0x7f4ae0
#define EFFECT_SUM_RVA 0x85be10
#define CANDIDATES_RVA 0x7cb0b0
#define BRANCH_VALUE_RVA 0x600470
#define TIER_LIST_RVA 0x5d7510
#define FREE_EFFECTS_RVA 0x4ad200
#define ENGINE_FREE_RVA 0x0f04d0
#define STATUS_RVA 0x8b3920
#define UNREST_TEST_RVA 0x8b9cd0
#define PROJECTION_RVA 0x87cbe0
#define RECORD_COPY_RVA 0x764090
#define APPLY_RVA 0x8e4960
#define LIST_COPY_RVA 0x55fae0

#define OWNER_LISTS_A 0x144
#define OWNER_LISTS_B 0x184
#define OWNER_LISTS_C 0x29c
#define OWNER_SKIPS_TABLE_LIST 0xd4
#define OWNER_HOLDER 0x1fc
#define HOLDER_LINK 0x4
#define HOLDER_TABLE_INDEX 0x10
#define HOLDER_TABLE 0x30
#define HOLDER_FACTION 0x40
#define LINK_LINKED 0x10
#define FACTION_LIST 0x6c4
#define FACTION_BRANCH_VALUE 0x6e8

#define PROJECTION_BYTES 0x5c
#define ENTRY_BYTES 0x98
#define REGIONS 6
#define ENTRIES 3

typedef BYTE *(__attribute__((thiscall)) *ProjectionFn)(BYTE *economy, BYTE *projection);
typedef uintptr_t(__attribute__((thiscall)) *StatusFn)(BYTE *self, BYTE *settlement);
typedef uintptr_t(__attribute__((thiscall)) *UnrestFn)(BYTE *settlement);

static BYTE *engine;
static volatile LONG fixes_on = 1, *effects_flag, *verify_flag;
static volatile DWORD main_thread;
static int failures, checks;
static long sums, projection_blocks;
static float hidden_term, hidden_key_c_term;
static BOOL hidden_last_entry;
static S2Api api;
static uint32_t random_state = 777;

typedef struct {
    BYTE owner[0x700], economy[0x80], holder[0x60], link[0x20], faction[0x700];
    BYTE *holder_target, *faction_target;
    EffectList table[2];
} Region;
static Region regions[REGIONS];

static const Effect key_a = {1, 0, 0, 0xd2, 0}, key_b = {1, 0, 0, 0x13, 0}, key_c = {2, 1, 0, 7, 0};

static BYTE *__attribute__((thiscall)) stand_in_owner(BYTE *context) {
    return context;
}

static uint32_t __attribute__((thiscall)) stand_in_branch_value(BYTE *holder) {
    (void)holder;
    return 1;
}

static const BYTE *__attribute__((thiscall)) stand_in_tier_list(BYTE *tier) {
    return tier;
}

static void __attribute__((thiscall)) stand_in_free_effects(BYTE *list) {
    effects_free((EffectList *)list);
}

static void __cdecl stand_in_engine_free(void *block) {
    projection_blocks--;
    free(block);
}

static BYTE *__attribute__((thiscall)) stand_in_copy_list(BYTE *out, const BYTE *list) {
    effects_copy((EffectList *)out, (const EffectList *)list);
    return out;
}

static BYTE *handle_target(const BYTE *handle) {
    BYTE **target = *(BYTE ***)(handle + 4);
    return target ? *target : NULL;
}

// The region's sum, from its three lists and, with a holder, its faction's list and the holder's table list. A hidden
// term, nonzero only to show what a missed input does, is added to the first entry.
static BYTE *__attribute__((thiscall)) stand_in_sum(BYTE *owner, BYTE *out) {
    sums++;
    EffectList *sum = (EffectList *)out;
    effects_copy(sum, (const EffectList *)(owner + OWNER_LISTS_A));
    effects_merge_sum(sum, (const EffectList *)(owner + OWNER_LISTS_B));
    effects_merge_sum(sum, (const EffectList *)(owner + OWNER_LISTS_C));
    BYTE *holder = handle_target(owner + OWNER_HOLDER);
    if (holder) {
        EffectList faction;
        effects_copy(&faction, (const EffectList *)(handle_target(holder + HOLDER_FACTION) + FACTION_LIST));
        if (!owner[OWNER_SKIPS_TABLE_LIST])
            effects_merge_sum(&faction, (const EffectList *)(*(BYTE **)(holder + HOLDER_TABLE) + *(uint32_t *)(holder + HOLDER_TABLE_INDEX) * 12));
        effects_merge_sum(sum, &faction);
        effects_free(&faction);
    }
    if (sum->count) sum->data[0].value += hidden_term;
    for (uint32_t i = 0; i < sum->count; i++)
        if (effect_same_key(&sum->data[i], &key_c)) sum->data[i].value += hidden_key_c_term;
    if (hidden_last_entry) {
        static Effect last = {9, 0, 0, 0, 1.0f};
        EffectList extra = {1, 1, &last};
        effects_merge_sum(sum, &extra);
    }
    return out;
}

static void *projection_alloc(size_t size) {
    if (!size) return NULL;
    projection_blocks++;
    return malloc(size);
}

static void __attribute__((thiscall)) stand_in_copy_record(BYTE *projection, const BYTE *record) {
    memcpy(projection, record, PROJECTION_BYTES);
    uint32_t entries = *(const uint32_t *)(record + 0x24), pairs = *(const uint32_t *)(record + 0x48);
    BYTE *entry_data = projection_alloc(entries * ENTRY_BYTES), *pair_data = projection_alloc(pairs * 8);
    if (entries) memcpy(entry_data, *(BYTE *const *)(record + 0x28), entries * ENTRY_BYTES);
    if (pairs) memcpy(pair_data, *(BYTE *const *)(record + 0x4c), pairs * 8);
    *(BYTE **)(projection + 0x28) = entry_data;
    *(BYTE **)(projection + 0x4c) = pair_data;
}

// Figures from the list's values, per entry and overall; it also changes the list it is given.
static void __attribute__((thiscall)) stand_in_apply(BYTE *projection, BYTE *effects, uint32_t first, uint32_t second, uint32_t third) {
    EffectList *list = (EffectList *)effects;
    *(float *)(projection + 0x4) = effects_value(list, &key_a) + (float)first + (float)second + (float)third;
    *(float *)(projection + 0x8) = effects_value(list, &key_b);
    *(float *)(projection + 0xc) = list->count ? list->data[0].value : 0.0f;
    *(uint32_t *)(projection + 0x54) = list->count;
    BYTE *entries = *(BYTE **)(projection + 0x28);
    for (uint32_t i = 0; i < *(uint32_t *)(projection + 0x24); i++)
        *(float *)(entries + i * ENTRY_BYTES + 0x38) = effects_value(list, &key_c) * (float)(i + 1);
    if (list->count) list->data[list->count - 1].value += 100.0f;
}

static BYTE *__attribute__((thiscall)) stand_in_projection(BYTE *economy, BYTE *projection) {
    stand_in_copy_record(projection, economy + 0x24);
    BYTE effects[12];
    stand_in_sum(*(BYTE **)(economy + 4), effects);
    stand_in_apply(projection, effects, 0, 0xffffffffu, 0);
    effects_free((EffectList *)effects);
    return projection;
}

static void release_projection(BYTE *projection) {
    if (*(void **)(projection + 0x4c)) stand_in_engine_free(*(void **)(projection + 0x4c));
    if (*(void **)(projection + 0x28)) stand_in_engine_free(*(void **)(projection + 0x28));
}

static BYTE status_projection[PROJECTION_BYTES], other_projection[PROJECTION_BYTES], unrest_projection[PROJECTION_BYTES];
static BOOL keep_projection;
static void (*during_status)(void);
static HANDLE other_inside, other_release;

// Another thread's map status waits inside until released, when the test asks it to.
static uintptr_t __attribute__((thiscall)) stand_in_status(BYTE *self, BYTE *settlement) {
    (void)self;
    BOOL main = GetCurrentThreadId() == main_thread;
    BYTE *projection = main ? status_projection : other_projection;
    ((ProjectionFn)(void *)(engine + PROJECTION_RVA))(settlement, projection);
    if (main && during_status) during_status();
    if (!main && other_release) SetEvent(other_inside), WaitForSingleObject(other_release, INFINITE);
    if (!main || !keep_projection) release_projection(projection);
    return 0;
}

static uintptr_t __attribute__((thiscall)) stand_in_unrest(BYTE *settlement) {
    ((ProjectionFn)(void *)(engine + PROJECTION_RVA))(settlement, unrest_projection);
    if (!keep_projection) release_projection(unrest_projection);
    return 0;
}

static uintptr_t __attribute__((thiscall)) stand_in_check(BYTE *context) {
    (void)context;
    return 0;
}

static void log_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

static void jump_to(BYTE *at, const void *to) {
    at[0] = 0xe9;
    *(LONG *)(at + 1) = (LONG)((const BYTE *)to - (at + 5));
}

static const char *hook(BYTE *target, const BYTE *prologue, size_t size, const void *replacement, void **original) {
    (void)prologue, (void)size;
    if (target[0] != 0xe9) return "not a stand-in";
    *original = target + 5 + *(LONG *)(target + 1);
    jump_to(target, replacement);
    return NULL;
}

static void register_switch(const char *name, volatile LONG *flag, void (*report)(BOOL)) {
    (void)report;
    if (!strcmp(name, "campeffects")) effects_flag = flag;
    if (!strcmp(name, "campeffectsverify")) verify_flag = flag;
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

static void random_list(EffectList *list) {
    effects_free(list);
    uint32_t count = 1 + next_random() % 12;
    list->data = effects_alloc(count);
    list->count = list->capacity = count;
    static const Effect keys[] = {{1, 0, 0, 0xd2, 0}, {1, 0, 0, 0x13, 0}, {2, 1, 0, 7, 0}, {1, 0, 0, 0x20, 0}, {3, 0, 2, 1, 0}};
    for (uint32_t i = 0; i < count; i++) {
        list->data[i] = keys[next_random() % (sizeof keys / sizeof *keys)];
        list->data[i].value = (float)((int)(next_random() % 200) - 100) / 4.0f;
    }
    for (uint32_t i = 1; i < count; i++)
        for (uint32_t j = i; j > 0 && effect_less(&list->data[j], &list->data[j - 1]); j--) {
            Effect swap = list->data[j];
            list->data[j] = list->data[j - 1];
            list->data[j - 1] = swap;
        }
}

static EffectList *list_at(BYTE *at) {
    return (EffectList *)at;
}

// Regions 0-2 have their three lists only; 3-5 a holder too, with a faction and a table list.
static void set_up_region(int index) {
    Region *r = &regions[index];
    random_list(list_at(r->owner + OWNER_LISTS_A));
    random_list(list_at(r->owner + OWNER_LISTS_B));
    random_list(list_at(r->owner + OWNER_LISTS_C));
    *(BYTE **)(r->economy + 4) = r->owner;
    BYTE *record = r->economy + 0x24;
    *(uint32_t *)(record + 0x20) = *(uint32_t *)(record + 0x24) = ENTRIES;
    static BYTE entries[REGIONS][ENTRIES * ENTRY_BYTES];
    for (int i = 0; i < ENTRIES * ENTRY_BYTES; i++) entries[index][i] = (BYTE)(index * 31 + i);
    *(BYTE **)(record + 0x28) = entries[index];
    *(float *)(record + 0x34) = (float)index;
    if (index < 3) return;
    r->holder_target = r->holder;
    *(BYTE ***)(r->owner + OWNER_HOLDER + 4) = &r->holder_target;
    r->faction_target = r->faction;
    *(BYTE ***)(r->holder + HOLDER_FACTION + 4) = &r->faction_target;
    *(BYTE **)(r->holder + HOLDER_LINK) = r->link;
    random_list(list_at(r->faction + FACTION_LIST));
    random_list(&r->table[1]);
    *(BYTE **)(r->holder + HOLDER_TABLE) = (BYTE *)r->table;
    *(uint32_t *)(r->holder + HOLDER_TABLE_INDEX) = 1;
    *(uint32_t *)(r->faction + FACTION_BRANCH_VALUE) = 1;
}

static BOOL same_projection(const BYTE *a, const BYTE *b) {
    static const struct {
        BYTE offset, size;
    } fields[] = {{0x00, 0x28}, {0x2c, 0x15}, {0x44, 0x08}, {0x50, 0x0c}};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
        if (memcmp(a + fields[i].offset, b + fields[i].offset, fields[i].size)) return FALSE;
    uint32_t entries = *(const uint32_t *)(a + 0x24), pairs = *(const uint32_t *)(a + 0x48);
    return (!entries || !memcmp(*(BYTE *const *)(a + 0x28), *(BYTE *const *)(b + 0x28), entries * ENTRY_BYTES)) &&
           (!pairs || !memcmp(*(BYTE *const *)(a + 0x4c), *(BYTE *const *)(b + 0x4c), pairs * 8));
}

// The projection through the map status, compared with the engine's; how many sums it took.
static BOOL status_matches(int index, long *sums_taken) {
    BYTE engine_projection[PROJECTION_BYTES] = {0};
    long before = sums;
    keep_projection = TRUE;
    memset(status_projection, 0xcc, sizeof status_projection);
    ((StatusFn)(void *)(engine + STATUS_RVA))(NULL, regions[index].economy);
    keep_projection = FALSE;
    *sums_taken = sums - before;
    stand_in_projection(regions[index].economy, engine_projection);
    BOOL same = same_projection(status_projection, engine_projection);
    release_projection(status_projection);
    release_projection(engine_projection);
    return same;
}

static BOOL reused(int index) {
    long taken;
    return status_matches(index, &taken) && taken == 0;
}

static BOOL rebuilt(int index) {
    long taken;
    return status_matches(index, &taken) && taken == 1;
}

static DWORD WINAPI status_on_other_thread(LPVOID unused) {
    (void)unused;
    ((StatusFn)(void *)(engine + STATUS_RVA))(NULL, regions[0].economy);
    return 0;
}

static void project_elsewhere_meanwhile(void) {
    HANDLE thread = CreateThread(NULL, 0, status_on_other_thread, NULL, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}

int main(void) {
    engine = VirtualAlloc(NULL, ENGINE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    jump_to(engine + CHECK_RVA, (const void *)stand_in_check);
    jump_to(engine + EFFECT_OWNER_RVA, (const void *)stand_in_owner);
    jump_to(engine + EFFECT_SUM_RVA, (const void *)stand_in_sum);
    jump_to(engine + CANDIDATES_RVA, (const void *)stand_in_check);
    jump_to(engine + BRANCH_VALUE_RVA, (const void *)stand_in_branch_value);
    jump_to(engine + TIER_LIST_RVA, (const void *)stand_in_tier_list);
    jump_to(engine + FREE_EFFECTS_RVA, (const void *)stand_in_free_effects);
    jump_to(engine + ENGINE_FREE_RVA, (const void *)stand_in_engine_free);
    jump_to(engine + STATUS_RVA, (const void *)stand_in_status);
    jump_to(engine + UNREST_TEST_RVA, (const void *)stand_in_unrest);
    jump_to(engine + PROJECTION_RVA, (const void *)stand_in_projection);
    jump_to(engine + RECORD_COPY_RVA, (const void *)stand_in_copy_record);
    jump_to(engine + APPLY_RVA, (const void *)stand_in_apply);
    jump_to(engine + LIST_COPY_RVA, (const void *)stand_in_copy_list);
    main_thread = GetCurrentThreadId();
    for (int i = 0; i < REGIONS; i++) set_up_region(i);

    api = (S2Api){.engine = engine, .fix_enabled = &fixes_on, .log_line = log_line, .hook = hook, .register_switch = register_switch,
                  .main_thread = &main_thread, .check_engine = check_engine};
    HMODULE module = LoadLibraryW(L"modules\\campeffects.dll");
    S2ModuleMain module_main = module ? (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main") : NULL;
    volatile LONG64 *stats = module ? (volatile LONG64 *)(void *)GetProcAddress(module, "effects_stats") : NULL;
    if (!module_main || !stats) return printf("modules\\campeffects.dll not found\n"), 1;
    expect("installs", !module_main(&api) && effects_flag && verify_flag);

    BOOL all = TRUE;
    for (int i = 0; i < REGIONS; i++) all &= rebuilt(i);
    expect("the first projection of each region: the engine's, from the engine's sum", all);
    all = TRUE;
    for (int round = 0; round < 3; round++)
        for (int i = 0; i < REGIONS; i++) all &= reused(i);
    expect("then the engine's projection from the cached sum, three lists or five", all);

    long blocks = effect_blocks, arrays = projection_blocks;
    for (int i = 0; i < 50; i++) reused(i % REGIONS);
    expect("the cached projections leak nothing", effect_blocks == blocks && projection_blocks == arrays);

    list_at(regions[0].owner + OWNER_LISTS_B)->data[0].value += 0.5f;
    expect("a value changed in a region's list: the engine's sum again", rebuilt(0) && reused(0));
    EffectList *a = list_at(regions[1].owner + OWNER_LISTS_A);
    a->count--;
    expect("a list one entry shorter: the engine's sum again", rebuilt(1) && reused(1));
    a->count++;
    expect("  and back", rebuilt(1) && reused(1));
    list_at(regions[3].faction + FACTION_LIST)->data[0].fourth ^= 0x40;
    expect("a faction list's key changed: the engine's sum again", rebuilt(3) && reused(3));
    regions[4].table[1].data[0].value -= 1.0f;
    expect("the holder's table list changed: the engine's sum again", rebuilt(4) && reused(4));
    regions[5].owner[OWNER_SKIPS_TABLE_LIST] = 1;
    expect("the table list skipped: the engine's sum again", rebuilt(5) && reused(5));
    *(BYTE ***)(regions[3].owner + OWNER_HOLDER + 4) = NULL;
    expect("the holder gone (five lists to three): the engine's sum again", rebuilt(3) && reused(3));
    *(BYTE ***)(regions[3].owner + OWNER_HOLDER + 4) = &regions[3].holder_target;
    unsigned int saved = _mm_getcsr();
    _mm_setcsr(saved | 0x8040);
    BOOL flushed = rebuilt(2) && reused(2);
    _mm_setcsr(saved);
    expect("another MXCSR mode: the engine's sum again", flushed && rebuilt(2));

    long before = sums;
    keep_projection = TRUE;
    ((ProjectionFn)(void *)(engine + PROJECTION_RVA))(regions[0].economy, status_projection);
    keep_projection = FALSE;
    release_projection(status_projection);
    expect("outside the two display paths: the engine's projection", sums == before + 1);
    before = sums;
    ((UnrestFn)(void *)(engine + UNREST_TEST_RVA))(regions[0].economy);
    expect("the unrest test: from the cache", sums == before);
    before = sums;
    project_elsewhere_meanwhile();
    expect("another thread: the engine's projection", sums == before + 1);
    before = sums;
    during_status = project_elsewhere_meanwhile;
    ((StatusFn)(void *)(engine + STATUS_RVA))(NULL, regions[1].economy);
    during_status = NULL;
    expect("  also while the main thread is in the map status", sums == before + 1);
    before = sums;
    other_inside = CreateEventW(NULL, FALSE, FALSE, NULL);
    other_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE thread = CreateThread(NULL, 0, status_on_other_thread, NULL, 0, NULL);
    WaitForSingleObject(other_inside, INFINITE);
    keep_projection = TRUE;
    ((ProjectionFn)(void *)(engine + PROJECTION_RVA))(regions[1].economy, status_projection);
    keep_projection = FALSE;
    release_projection(status_projection);
    SetEvent(other_release);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(other_inside);
    CloseHandle(other_release);
    other_release = NULL;
    expect("main outside the map status while another thread is in one: the engine's projection", sums == before + 2);
    *effects_flag = 0;
    expect("switch off: the engine's projection", rebuilt(0));
    *effects_flag = 1;
    fixes_on = 0;
    expect("Apply Fixes off: the engine's projection", rebuilt(0));
    fixes_on = 1;

    *verify_flag = 1;
    LONG64 mismatches = stats[4], projection_mismatches = stats[6];
    for (int i = 0; i < REGIONS; i++) reused(i);
    expect("verify: the cached sums and the projections agree with the engine's", stats[4] == mismatches && stats[6] == projection_mismatches);
    hidden_term = 3.0f;
    reused(0);
    expect("verify flags a sum with an input the cache doesn't see, and its projection", stats[4] == mismatches + 1 && stats[6] == projection_mismatches + 1);
    hidden_term = 0;
    // A missed input that shows only in the building entries, and one that shows only in the last fields (the count).
    EffectList *first = list_at(regions[2].owner + OWNER_LISTS_A);
    effects_free(first);
    first->data = effects_alloc(2);
    first->count = first->capacity = 2;
    first->data[0] = key_b, first->data[0].value = 1.0f;
    first->data[1] = key_c, first->data[1].value = 2.0f;
    rebuilt(2);
    projection_mismatches = stats[6];
    hidden_key_c_term = 5.0f;
    reused(2);
    hidden_key_c_term = 0;
    expect("  and one that changes only the building entries", stats[6] == projection_mismatches + 1);
    projection_mismatches = stats[6];
    hidden_last_entry = TRUE;
    reused(2);
    hidden_last_entry = FALSE;
    expect("  and one that changes only the last fields", stats[6] == projection_mismatches + 1);
    *verify_flag = 0;

    printf("%d of %d checks failed\n", failures, checks);
    return failures != 0;
}
