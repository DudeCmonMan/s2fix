#define _WIN32_WINNT 0x0A00
#include "../s2module.h"
#include "../checks/keysplit.h"
#include <stdint.h>
#include <stdlib.h>
#include <xmmintrin.h>

// The Variant node's compute_keys (0x114e7d90) with its per-item work on helper threads while main fills the instance
// textures (0x114e78e0), which both of compute_keys' callers (the engine's prepare 0x114c4780, the overhaul's timed
// prepare) do right after it; their own fill call is then skipped. The poses are flushed and both lists merged first, as
// compute_keys does; the per-item lookup (0x114c74f0) only reads the node's pose table and its textures' row counts, and
// the fill reads only the pose list and the textures' sizes, never the item fields the keys write. compute_keys returns
// once every key is written. The helpers finish inside main's fill. keyverify recomputes the keys with the engine's own
// function and counts items that differ.
#define COMPUTE_KEYS_RVA 0x14e7d90
#define FILL_RVA 0x14e78e0
#define KEY_LOOKUP_RVA 0x14c74f0
#define POSE_FLUSH_RVA 0x14e6870
#define LIST_MERGE_RVA 0x14e74d0
#define RENDER_ITEM_SIZE 0x138
#define ITEM_POSE 0x128
#define ITEM_TEXTURE 0x124
#define ITEM_ROW 0x84
#define POSE_LIST 0x17c
#define POSE_DIRTY 0x1a0
#define LIST_A_BODY 0x90
#define LIST_A_COUNT 0x94
#define LIST_A_ITEMS 0x98
#define LIST_A_MERGE 0xb4
#define LIST_B_BODY 0x118
#define LIST_B_COUNT 0x11c
#define LIST_B_ITEMS 0x120
#define LIST_B_MERGE 0x13c
#define HELPERS 3
// The job comes once a frame and main's fill covers the wake-up, so the helpers barely spin.
#define SPIN_ROUNDS 100

typedef void(__attribute__((thiscall)) *NodeFn)(BYTE *node);
typedef void(__attribute__((thiscall)) *KeyLookupFn)(BYTE *node, uint32_t pose, uint32_t *texture, uint32_t *row);

typedef struct {
    uint32_t a_first, a_end, b_first, b_end;
} Share;

typedef struct {
    LONG64 calls, split, fills_skipped, verified, differed, wait_ticks;
} Stats;

__declspec(dllexport) volatile LONG split_enabled = 1, verify = 0;
__declspec(dllexport) volatile Stats key_stats;

static const BYTE keys_prologue[] = {0x56, 0x8b, 0xf1, 0x80, 0xbe, 0xa0, 0x01, 0x00, 0x00, 0x00};
static const BYTE fill_prologue[] = {0x56, 0x8b, 0xf1, 0x80, 0xbe, 0xc9, 0x01, 0x00, 0x00, 0x00};
static const S2Api *api;
static NodeFn original_keys;
static NodeFn original_fill;
static Share shares[HELPERS];
static BYTE *job_node;
static unsigned int job_csr;
static volatile LONG generation, remaining;
static BYTE *filled_node;
static BOOL filling;
static uint32_t *verify_copy;
static size_t verify_capacity;

static LONGLONG now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void key_items(BYTE *node, BYTE *items, uint32_t first, uint32_t end) {
    KeyLookupFn lookup = (KeyLookupFn)(api->engine + KEY_LOOKUP_RVA);
    for (uint32_t i = first; i < end; i++) {
        BYTE *item = items + (size_t)i * RENDER_ITEM_SIZE;
        uint32_t texture, row;
        lookup(node, *(uint32_t *)(item + ITEM_POSE), &texture, &row);
        *(uint32_t *)(item + ITEM_TEXTURE) = texture;
        *(float *)(item + ITEM_ROW) = (float)(double)row;
    }
}

static DWORD WINAPI helper(LPVOID parameter) {
    int index = (int)(intptr_t)parameter;
    LONG seen = 0;
    for (;;) {
        for (int spin = 0; generation == seen && spin < SPIN_ROUNDS; spin++) _mm_pause();
        while (generation == seen) WaitOnAddress(&generation, &seen, sizeof seen, INFINITE);
        seen = generation;
        _mm_setcsr(job_csr);
        BYTE *node = job_node;
        const Share *share = &shares[index];
        key_items(node, *(BYTE **)(node + LIST_A_ITEMS), share->a_first, share->a_end);
        key_items(node, *(BYTE **)(node + LIST_B_ITEMS), share->b_first, share->b_end);
        InterlockedDecrement(&remaining);
    }
    return 0;
}

static void check_keys(BYTE *node) {
    uint32_t a = *(uint32_t *)(node + LIST_A_COUNT), b = *(uint32_t *)(node + LIST_B_COUNT);
    size_t needed = 2 * ((size_t)a + b);
    if (needed > verify_capacity) {
        free(verify_copy);
        verify_copy = malloc(needed * 2 * sizeof *verify_copy);
        verify_capacity = verify_copy ? needed * 2 : 0;
        if (!verify_copy) return;
    }
    BYTE *lists[2] = {*(BYTE **)(node + LIST_A_ITEMS), *(BYTE **)(node + LIST_B_ITEMS)};
    uint32_t counts[2] = {a, b}, *at = verify_copy;
    for (int l = 0; l < 2; l++)
        for (uint32_t i = 0; i < counts[l]; i++, at += 2) {
            memcpy(at, lists[l] + (size_t)i * RENDER_ITEM_SIZE + ITEM_TEXTURE, 4);
            memcpy(at + 1, lists[l] + (size_t)i * RENDER_ITEM_SIZE + ITEM_ROW, 4);
        }
    original_keys(node);
    LONG64 differed = 0;
    at = verify_copy;
    for (int l = 0; l < 2; l++)
        for (uint32_t i = 0; i < counts[l]; i++, at += 2)
            differed += memcmp(at, lists[l] + (size_t)i * RENDER_ITEM_SIZE + ITEM_TEXTURE, 4) || memcmp(at + 1, lists[l] + (size_t)i * RENDER_ITEM_SIZE + ITEM_ROW, 4);
    key_stats.verified++;
    key_stats.differed += differed;
}

static void __attribute__((thiscall)) keys_hook(BYTE *node) {
    key_stats.calls++;
    filled_node = NULL;
    if (!*api->fix_enabled || !split_enabled || GetCurrentThreadId() != *api->main_thread) {
        original_keys(node);
        return;
    }
    if (node[POSE_DIRTY]) ((NodeFn)(api->engine + POSE_FLUSH_RVA))(node + POSE_LIST);
    if (node[LIST_A_MERGE]) ((NodeFn)(api->engine + LIST_MERGE_RVA))(node + LIST_A_BODY);
    if (node[LIST_B_MERGE]) ((NodeFn)(api->engine + LIST_MERGE_RVA))(node + LIST_B_BODY);
    uint32_t a = *(uint32_t *)(node + LIST_A_COUNT), b = *(uint32_t *)(node + LIST_B_COUNT);
    for (int k = 0; k < HELPERS; k++)
        shares[k] = (Share){(uint32_t)((uint64_t)a * k / HELPERS), (uint32_t)((uint64_t)a * (k + 1) / HELPERS), (uint32_t)((uint64_t)b * k / HELPERS),
                            (uint32_t)((uint64_t)b * (k + 1) / HELPERS)};
    job_node = node;
    job_csr = _mm_getcsr();
    remaining = HELPERS;
    InterlockedIncrement(&generation);
    WakeByAddressAll((void *)&generation);
    filling = TRUE;
    ((NodeFn)(api->engine + FILL_RVA))(node);
    filling = FALSE;
    LONGLONG started = now();
    while (remaining) _mm_pause();
    key_stats.wait_ticks += now() - started;
    filled_node = node;
    key_stats.split++;
    if (verify) check_keys(node);
}

static void __attribute__((thiscall)) fill_hook(BYTE *node) {
    // Only the fill right after compute_keys is the one main already did.
    BOOL done = !filling && node == filled_node && GetCurrentThreadId() == *api->main_thread;
    if (!filling) filled_node = NULL;
    if (done) {
        key_stats.fills_skipped++;
        return;
    }
    original_fill(node);
}

static void report(BOOL start) {
    static Stats before;
    if (start) {
        before = key_stats;
        return;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    LONG64 split = key_stats.split - before.split;
    api->log_line("  keysplit: %lld compute_keys calls, %lld split, %lld second fills skipped, main waited %.1f us each after its fill; verify: %lld "
                  "checked, %lld items differ",
                  key_stats.calls - before.calls, split, key_stats.fills_skipped - before.fills_skipped,
                  split ? 1e6 * (key_stats.wait_ticks - before.wait_ticks) / frequency.QuadPart / split : 0.0, key_stats.verified - before.verified,
                  key_stats.differed - before.differed);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    const char *mismatch = S2_CHECK_ENGINE(host, keysplit_checks);
    if (mismatch) return mismatch;
    api = host;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    for (int k = 0; k < HELPERS; k++) {
        HANDLE thread = CreateThread(NULL, 0, helper, (LPVOID)(intptr_t)k, 0, NULL);
        if (!thread) return "CreateThread failed";
        CloseHandle(thread);
    }
    // The fill first: compute_keys hooked without it would fill twice.
    const char *error = api->hook(api->engine + FILL_RVA, fill_prologue, sizeof fill_prologue, (const void *)fill_hook, (void **)&original_fill);
    if (!error) error = api->hook(api->engine + COMPUTE_KEYS_RVA, keys_prologue, sizeof keys_prologue, (const void *)keys_hook, (void **)&original_keys);
    if (error) return error;
    api->register_switch("keysplit", &split_enabled, report);
    api->register_switch("keyverify", &verify, report);
    return NULL;
}
