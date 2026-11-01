#include "../s2module.h"
#include "../checks/campeffects.h"
#include <emmintrin.h>
#include <intrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Campaign map: every frame, each building slot of the player's settlements on screen asks whether it can build
// something now, and each ask rebuilds its region's whole effect list, merging and summing up to seven stored lists.
// The sum depends only on those lists' contents, so it is kept per region and reused while every list is byte for byte
// what it was when the engine built it. The candidate check that reads it (treasury, requirements) still runs each time.
#define ACTION_CHECK_RVA 0x98ef30
#define EFFECT_SUM_RVA 0xa94a20
#define CANDIDATES_RVA 0x9e65b0
#define BRANCH_VALUE_RVA 0xb4d3d0
#define TIER_LIST_RVA 0x76f2f0
#define FREE_EFFECTS_RVA 0x60bcf0
#define ENGINE_FREE_RVA 0x19fe10

#define CONTEXT_OBJECT 0x8
#define OBJECT_EFFECT_OWNER_SLOT 0x80
#define OWNER_LISTS_A 0x1e0
#define OWNER_LISTS_B 0x238
#define OWNER_LISTS_C 0x3c0
#define OWNER_SKIPS_TABLE_LIST 0x124
#define OWNER_HOLDER 0x2e0
#define HOLDER_FACTION 0x68
#define HOLDER_TABLE_INDEX 0x20
#define HOLDER_TABLE 0x48
#define HOLDER_LINK 0x8
#define HOLDER_BRANCH 0x80
#define LINK_LINKED 0x28
#define FACTION_LIST 0x838
#define FACTION_BRANCH_VALUE 0x878
#define LINKED_TIER 0x3f0
#define LINKED_LIST 0x7b0

#define ENTRY_BYTES 32
#define TABLE_LIST_BYTES 16
#define CANDIDATE_BYTES 32
#define CANDIDATE_DIFFERS 0x1c
#define MAX_SOURCES 7
#define SLOTS 128
// Rounding, flush-to-zero and denormals-are-zero: the sums' float additions depend on them.
#define MXCSR_ROUNDING 0xe040
#define REPORT_MS 10000

typedef BYTE *(*OwnerFn)(BYTE *object);
typedef BYTE *(*SumFn)(BYTE *owner, BYTE *out);
typedef void (*CandidatesFn)(BYTE *context, BYTE *out, const BYTE *effects, BYTE available_only, BYTE second);
typedef uintptr_t (*BranchValueFn)(BYTE *object);
typedef const BYTE *(*TierListFn)(BYTE *tier);
typedef void (*FreeEffectsFn)(BYTE *list);
typedef void (*EngineFreeFn)(void *block);
typedef uintptr_t (*CheckFn)(BYTE *context);

typedef struct {
    const BYTE *lists[MAX_SOURCES];
    uint32_t count, shape;
} Sources;

typedef struct {
    BYTE *owner;
    uint32_t shape, mxcsr, counts[MAX_SOURCES];
    BYTE *snapshot;
    size_t snapshot_capacity;
    // The engine's list {capacity, count, data}, from its own allocator.
    BYTE sum[16];
    uint64_t used;
    BOOL filled;
} Entry;

// Plain counters: they are only written on the main thread.
typedef struct {
    LONG64 checks, hits, rebuilds, mismatches, answer_mismatches;
} Stats;

__declspec(dllexport) volatile LONG effects_enabled = 1;
__declspec(dllexport) volatile LONG verify;
__declspec(dllexport) volatile Stats effects_stats;

static const BYTE check_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0xf9, 0x33, 0xdb};
static const BYTE empty_list[16];
static const S2Api *api;
static CheckFn original_check;
static SumFn effect_sum;
static CandidatesFn list_candidates;
static BranchValueFn branch_value;
static TierListFn tier_list;
static FreeEffectsFn free_effects;
static EngineFreeFn engine_free;
static Entry entries[SLOTS];
static uint64_t use_clock;

static uint32_t count_of(const BYTE *list) {
    return *(const uint32_t *)(list + 4);
}

static const BYTE *data_of(const BYTE *list) {
    return *(BYTE *const *)(list + 8);
}

static BYTE *handle_target(const BYTE *handle) {
    BYTE **target = *(BYTE ***)handle;
    return target ? *target : NULL;
}

// The lists the engine's sum merges, in its order, and which of its three shapes it takes. FALSE where the engine
// would read through a null pointer: those asks are left to it.
static BOOL gather(BYTE *owner, Sources *sources) {
    sources->count = 0;
    sources->shape = 0;
    sources->lists[sources->count++] = owner + OWNER_LISTS_A;
    sources->lists[sources->count++] = owner + OWNER_LISTS_B;
    sources->lists[sources->count++] = owner + OWNER_LISTS_C;
    BYTE *holder = handle_target(owner + OWNER_HOLDER);
    if (!holder) return TRUE;
    BYTE *faction = handle_target(holder + HOLDER_FACTION);
    BYTE *link = *(BYTE **)(holder + HOLDER_LINK);
    if (!faction || !link) return FALSE;
    sources->shape = 1;
    sources->lists[sources->count++] = faction + FACTION_LIST;
    sources->lists[sources->count++] = owner[OWNER_SKIPS_TABLE_LIST]
                                           ? empty_list
                                           : *(BYTE **)(holder + HOLDER_TABLE) + (size_t)*(uint32_t *)(holder + HOLDER_TABLE_INDEX) * TABLE_LIST_BYTES;
    BYTE *linked = handle_target(link + LINK_LINKED);
    if (!linked) return TRUE;
    BYTE *branch_handle = handle_target(holder + HOLDER_BRANCH);
    BYTE *branch_object = branch_handle ? *(BYTE **)branch_handle : NULL;
    if (!branch_object) return FALSE;
    if (*(uintptr_t *)(faction + FACTION_BRANCH_VALUE) == branch_value(branch_object)) return TRUE;
    BYTE *tier = handle_target(linked + LINKED_TIER);
    if (!tier) return FALSE;
    sources->shape = 2;
    sources->lists[sources->count++] = tier_list(tier);
    sources->lists[sources->count++] = linked + LINKED_LIST;
    return TRUE;
}

static BOOL equal_bytes(const BYTE *a, const BYTE *b, size_t size) {
    size_t i = 0;
    for (; i + 64 <= size; i += 64) {
        __m128i differ = _mm_or_si128(
            _mm_or_si128(_mm_xor_si128(_mm_loadu_si128((const __m128i *)(a + i)), _mm_loadu_si128((const __m128i *)(b + i))),
                         _mm_xor_si128(_mm_loadu_si128((const __m128i *)(a + i + 16)), _mm_loadu_si128((const __m128i *)(b + i + 16)))),
            _mm_or_si128(_mm_xor_si128(_mm_loadu_si128((const __m128i *)(a + i + 32)), _mm_loadu_si128((const __m128i *)(b + i + 32))),
                         _mm_xor_si128(_mm_loadu_si128((const __m128i *)(a + i + 48)), _mm_loadu_si128((const __m128i *)(b + i + 48)))));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(differ, _mm_setzero_si128())) != 0xffff) return FALSE;
    }
    return !memcmp(a + i, b + i, size - i);
}

static BOOL unchanged(const Entry *entry, const Sources *sources, uint32_t mxcsr) {
    if (!entry->filled || entry->shape != sources->shape || entry->mxcsr != mxcsr) return FALSE;
    for (uint32_t i = 0; i < sources->count; i++)
        if (count_of(sources->lists[i]) != entry->counts[i]) return FALSE;
    const BYTE *saved = entry->snapshot;
    for (uint32_t i = 0; i < sources->count; i++) {
        size_t size = (size_t)entry->counts[i] * ENTRY_BYTES;
        if (size && !equal_bytes(saved, data_of(sources->lists[i]), size)) return FALSE;
        saved += size;
    }
    return TRUE;
}

static void release(Entry *entry) {
    if (entry->filled) free_effects(entry->sum);
    memset(entry->sum, 0, sizeof entry->sum);
    entry->filled = FALSE;
}

static Entry *entry_for(BYTE *owner) {
    uint32_t start = (uint32_t)(((uintptr_t)owner >> 3) * 2654435761u) % SLOTS;
    Entry *oldest = &entries[start];
    for (uint32_t probe = 0; probe < SLOTS; probe++) {
        Entry *entry = &entries[(start + probe) % SLOTS];
        if (entry->owner == owner || !entry->owner) return entry;
        if (entry->used < oldest->used) oldest = entry;
    }
    release(oldest);
    return oldest;
}

static BOOL rebuild(Entry *entry, BYTE *owner, const Sources *sources, uint32_t mxcsr) {
    size_t total = 0;
    for (uint32_t i = 0; i < sources->count; i++) total += (size_t)count_of(sources->lists[i]) * ENTRY_BYTES;
    if (total > entry->snapshot_capacity) {
        BYTE *grown = realloc(entry->snapshot, total);
        if (!grown) return FALSE;
        entry->snapshot = grown;
        entry->snapshot_capacity = total;
    }
    release(entry);
    BYTE *saved = entry->snapshot;
    for (uint32_t i = 0; i < sources->count; i++) {
        entry->counts[i] = count_of(sources->lists[i]);
        size_t size = (size_t)entry->counts[i] * ENTRY_BYTES;
        if (size) memcpy(saved, data_of(sources->lists[i]), size);
        saved += size;
    }
    effect_sum(owner, entry->sum);
    entry->owner = owner;
    entry->shape = sources->shape;
    entry->mxcsr = mxcsr;
    entry->filled = TRUE;
    effects_stats.rebuilds++;
    return TRUE;
}

static void compare_with_engine(BYTE *owner, const Entry *entry) {
    BYTE fresh[16] = {0};
    effect_sum(owner, fresh);
    if (memcmp(fresh, entry->sum, 8) || !equal_bytes(data_of(fresh), data_of(entry->sum), (size_t)count_of(fresh) * ENTRY_BYTES))
        effects_stats.mismatches++;
    free_effects(fresh);
}

static const BYTE *cached_effects(BYTE *owner) {
    Sources sources;
    if (!gather(owner, &sources)) return NULL;
    uint32_t mxcsr = _mm_getcsr() & MXCSR_ROUNDING;
    Entry *entry = entry_for(owner);
    entry->used = ++use_clock;
    if (entry->owner == owner && unchanged(entry, &sources, mxcsr)) {
        effects_stats.hits++;
        if (verify) compare_with_engine(owner, entry);
        return entry->sum;
    }
    return rebuild(entry, owner, &sources, mxcsr) ? entry->sum : NULL;
}

// The engine's check, with the region's sum from the cache: list the candidates the slot can take now, then answer
// whether any matches the slot's current building (or any at all for an empty slot).
static uintptr_t check_hook(BYTE *context) {
    if (!*api->fix_enabled || !effects_enabled || __readgsdword(0x48) != *api->main_thread) return original_check(context);
    effects_stats.checks++;
    BYTE *object = *(BYTE **)(context + CONTEXT_OBJECT);
    OwnerFn effect_owner = *(OwnerFn *)(*(BYTE **)object + OBJECT_EFFECT_OWNER_SLOT);
    const BYTE *effects = cached_effects(effect_owner(object));
    if (!effects) return original_check(context);
    BYTE candidates[16] = {0};
    list_candidates(context, candidates, effects, 1, 1);
    uint32_t count = count_of(candidates);
    BYTE *data = *(BYTE **)(candidates + 8);
    BOOL any = FALSE;
    for (uint32_t i = 0; i < count && !any; i++) any = !data[(size_t)i * CANDIDATE_BYTES + CANDIDATE_DIFFERS];
    if (data) engine_free(data);
    if (verify && (BYTE)original_check(context) != any) effects_stats.answer_mismatches++;
    return any;
}

static DWORD WINAPI report_loop(LPVOID unused) {
    (void)unused;
    Stats last = {0};
    for (;; Sleep(REPORT_MS)) {
        Stats now = effects_stats;
        LONG64 checks = now.checks - last.checks;
        if (verify && checks)
            api->log_line("campeffects: %lld checks a second, %.1f%% of their effect lists from the cache, %lld rebuilt; %lld cached lists differed from the engine's sum, %lld answers from the engine's check",
                          checks * 1000 / REPORT_MS, 100.0 * (double)(now.hits - last.hits) / (double)checks, now.rebuilds - last.rebuilds,
                          now.mismatches - last.mismatches, now.answer_mismatches - last.answer_mismatches);
        last = now;
    }
    return 0;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, campeffects_checks);
    if (mismatch) return mismatch;
    api = host;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    effect_sum = (SumFn)(void *)(api->engine + EFFECT_SUM_RVA);
    list_candidates = (CandidatesFn)(void *)(api->engine + CANDIDATES_RVA);
    branch_value = (BranchValueFn)(void *)(api->engine + BRANCH_VALUE_RVA);
    tier_list = (TierListFn)(void *)(api->engine + TIER_LIST_RVA);
    free_effects = (FreeEffectsFn)(void *)(api->engine + FREE_EFFECTS_RVA);
    engine_free = (EngineFreeFn)(void *)(api->engine + ENGINE_FREE_RVA);
    const char *error = api->hook(api->engine + ACTION_CHECK_RVA, check_prologue, sizeof check_prologue, (const void *)check_hook, (void **)&original_check);
    if (error) return error;
    HANDLE thread = CreateThread(NULL, 0, report_loop, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    api->register_switch("campeffects", &effects_enabled, NULL);
    api->register_switch("campeffectsverify", &verify, NULL);
    return NULL;
}
