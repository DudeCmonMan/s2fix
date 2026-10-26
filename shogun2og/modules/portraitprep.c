#include "../s2module.h"
#include <stdint.h>
#include <string.h>

// The UI's 3D models (the general's portrait, the advisor) are drawn through the battle's Variant node: 0x110ddc00
// adds the model's pose and items, calls the node's prepare-now (vtbl+0x84, 0x114c47f0), draws the model into its
// rectangle and clears the node. Prepare-now runs the whole prepare again: keys for every item of lists A and B, the
// instance textures, and list A's sort, over the ~10,000 battle items it already prepared this frame, to add ~8. The
// model's draws carry only its own instances (10 draws, 10 instances, measured 2026-10-25), and nothing draws the
// battle's items after it, so this prepares only what was added since the frame's prepare: keys for the new items,
// the instance-texture rows of the new poses, and the new items sorted among themselves after the battle's. The model's
// view then records the same sequence, since its walk records none of the battle's items. The engine's fill would
// rewrite every row of every instance texture (WRITE_DISCARD) for the model's one pose: 62% of the model's time
// (measured 2026-10-25). Only the new rows are written, into the textures and rows the engine's fill puts them in; the
// discarded rows are read by nothing after this.
#define PREPARE_RVA 0x14c4780
#define PREPARE_NOW_RVA 0x14c47f0
#define UI_MODEL_RVA 0x10ddc00
#define KEY_LOOKUP_RVA 0x14c74f0
#define FILL_TEXTURES_RVA 0x14e78e0
#define TEXTURE_WRAPPER_RVA 0x13e3500
#define RENDER_DEVICE_RVA 0x1c9a93c
#define GET_RENDERER_SLOT 0x84
#define LIST_MERGE_RVA 0x14e74d0
#define POSE_FLUSH_RVA 0x14e6870
#define SORT_A_RVA 0x14aba00
#define FRAME_STAMP_RVA 0x17cc340
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
#define NODE_TEXTURE_COUNT 0x1c0
#define NODE_TEXTURES 0x1c4
#define NODE_ROWS_DIRTY 0x1c9
#define POSE_COUNT 0x180
#define POSE_ROWS 0x184
#define BONE_BYTES 48
#define MAP_WRITE_DISCARD 4
#define NODE_INSTANCING 0x1c8
#define NODE_INSTANCING_OFF 0x1ca

typedef void(__attribute__((thiscall)) *PrepareFn)(BYTE *prepare_interface, BYTE *renderer);
typedef uint32_t(__attribute__((thiscall)) *UiModelFn)(BYTE *self, void *a, void *b, void *c, void *d, void *e);
typedef void(__attribute__((thiscall)) *NodeFn)(BYTE *node);
typedef void(__attribute__((thiscall)) *ListFn)(BYTE *list);
typedef void(__attribute__((thiscall)) *KeyLookupFn)(BYTE *node, uint32_t pose, uint32_t *texture, uint32_t *row);
typedef void (*SortFn)(BYTE *first, BYTE *last, uint32_t count, BYTE *context);
typedef uint32_t(__attribute__((thiscall)) *RowsFn)(BYTE *texture);
typedef BYTE *(__attribute__((thiscall)) *WrapperFn)(BYTE *texture);
typedef BYTE *(__attribute__((thiscall)) *GetRendererFn)(BYTE *device);
typedef BYTE(__attribute__((thiscall)) *MapFn)(BYTE *wrapper, BYTE *renderer, uint32_t subresource, uint32_t type, BYTE **data, uint32_t *pitch, uint32_t a,
                                             uint32_t b);
typedef void(__attribute__((thiscall)) *UnmapFn)(BYTE *wrapper, BYTE *renderer, uint32_t subresource);

typedef struct {
    LONG64 calls, shortcuts, fallbacks, items_keyed, rows_written, full_fills;
} Stats;

// 0 lets prepare-now run the whole prepare, as the engine does; exported, and a switch.
__declspec(dllexport) volatile LONG portrait_prepare = 1;
// 0 lets the shortened prepare run the engine's whole texture fill; exported, and a switch.
__declspec(dllexport) volatile LONG portrait_fill = 1;
__declspec(dllexport) volatile Stats portrait_stats;

static const BYTE prepare_prologue[] = {0x51, 0x56, 0x57, 0x8b, 0xf9};
static const BYTE prepare_now_prologue[] = {0x8b, 0x41, 0x04, 0x56, 0x57};
static const BYTE ui_model_prologue[] = {0x81, 0xec, 0xb4, 0x00, 0x00, 0x00};
static const S2Api *api;
static PrepareFn original_prepare;
static NodeFn original_prepare_now;
static UiModelFn original_ui_model;
// Set while a UI model draws on main; other hooks on prepare-now hide the call site's return address.
static int in_ui_model;
static volatile uint64_t *frame_stamp;
// The last whole prepare on main: its node, frame, and the lists' lengths when it returned.
static BYTE *prepared_node;
static uint64_t prepared_stamp;
static uint32_t prepared_a, prepared_b, prepared_rows;

static void __attribute__((thiscall)) prepare_hook(BYTE *prepare_interface, BYTE *renderer) {
    original_prepare(prepare_interface, renderer);
    if (GetCurrentThreadId() != *api->main_thread) return;
    BYTE *node = prepare_interface - 4;
    prepared_node = node;
    prepared_stamp = *frame_stamp;
    prepared_a = *(uint32_t *)(node + LIST_A_COUNT);
    prepared_b = *(uint32_t *)(node + LIST_B_COUNT);
    prepared_rows = *(uint32_t *)(node + POSE_COUNT);
}

static BOOL instancing_active(const BYTE *node) {
    return node[NODE_INSTANCING] && !node[NODE_INSTANCING_OFF] && *(const uint32_t *)(node + NODE_TEXTURE_COUNT);
}

// As compute_keys_list (0x114e7cf0) keys each item: the texture index and row of its pose, the row as a float.
static void key_items(BYTE *node, BYTE *items, uint32_t first, uint32_t end) {
    KeyLookupFn lookup = (KeyLookupFn)(api->engine + KEY_LOOKUP_RVA);
    for (uint32_t i = first; i < end; i++) {
        BYTE *item = items + (size_t)i * RENDER_ITEM_SIZE;
        uint32_t texture, row;
        lookup(node, *(uint32_t *)(item + ITEM_POSE), &texture, &row);
        *(uint32_t *)(item + ITEM_TEXTURE) = texture;
        *(float *)(item + ITEM_ROW) = (float)(double)row;
    }
    portrait_stats.items_keyed += end - first;
}

static uint32_t texture_rows(BYTE *texture) {
    return ((RowsFn)(*(void ***)texture)[0x64 / 4])(texture);
}

// As 0x114c5470 fills an instance texture holding rows [start, start + its rows), for rows [first, end) only: mapped
// with WRITE_DISCARD, each row's bones (48 bytes each, clipped to the row pitch) at its offset from start.
static void fill_rows(BYTE *node, BYTE *texture, uint32_t start, uint32_t first, uint32_t end) {
    BYTE *engine = api->engine, *device = *(BYTE **)(engine + RENDER_DEVICE_RVA);
    BYTE *renderer = ((GetRendererFn)(*(void ***)device)[GET_RENDERER_SLOT / 4])(device);
    BYTE *wrapper = ((WrapperFn)(engine + TEXTURE_WRAPPER_RVA))(texture);
    BYTE *data = NULL;
    uint32_t pitch = 0;
    if (!((MapFn)(*(void ***)wrapper)[0x20 / 4])(wrapper, renderer, 0, MAP_WRITE_DISCARD, &data, &pitch, 0, 0)) return;
    BYTE **rows = *(BYTE ***)(node + POSE_ROWS);
    for (uint32_t row = first; row < end; row++) {
        uint32_t bytes = *(uint32_t *)(rows[row] + 4) * BONE_BYTES;
        memcpy(data + (size_t)(row - start) * pitch, *(BYTE **)(rows[row] + 8), bytes < pitch ? bytes : pitch);
    }
    ((UnmapFn)(*(void ***)wrapper)[0x24 / 4])(wrapper, renderer, 0);
    portrait_stats.rows_written += end - first;
}

// The engine's fill (0x114e78e0) with only rows [first, count) written. Fewer rows than the last texture holds go into
// the first texture that holds more, from row 0; otherwise the textures fill from the last down, each from the rows
// the later ones took.
static void fill_new_rows(BYTE *node, uint32_t first) {
    uint32_t count = *(uint32_t *)(node + POSE_COUNT), textures = *(uint32_t *)(node + NODE_TEXTURE_COUNT);
    BYTE **texture = *(BYTE ***)(node + NODE_TEXTURES);
    if (count < texture_rows(texture[textures - 1])) {
        for (uint32_t i = 0; i < textures; i++)
            if (count < texture_rows(texture[i])) {
                fill_rows(node, texture[i], 0, first, count);
                break;
            }
    } else {
        uint32_t start = 0;
        for (uint32_t i = textures; i-- > 0 && start <= count;) {
            uint32_t rows = texture_rows(texture[i]), end = start + rows < count ? start + rows : count;
            if (first < end) fill_rows(node, texture[i], start, first > start ? first : start, end);
            start += rows;
        }
    }
    node[NODE_ROWS_DIRTY] = 0;
}

static void __attribute__((thiscall)) prepare_now_hook(BYTE *node) {
    BOOL portrait = in_ui_model && GetCurrentThreadId() == *api->main_thread;
    if (!portrait) {
        original_prepare_now(node);
        return;
    }
    portrait_stats.calls++;
    if (!*api->fix_enabled || !portrait_prepare || node != prepared_node || *frame_stamp != prepared_stamp || !instancing_active(node)) {
        portrait_stats.fallbacks++;
        original_prepare_now(node);
        return;
    }
    BYTE *engine = api->engine;
    if (node[POSE_DIRTY]) ((ListFn)(engine + POSE_FLUSH_RVA))(node + POSE_LIST);
    if (node[LIST_A_MERGE]) ((ListFn)(engine + LIST_MERGE_RVA))(node + LIST_A_BODY);
    if (node[LIST_B_MERGE]) ((ListFn)(engine + LIST_MERGE_RVA))(node + LIST_B_BODY);
    uint32_t count_a = *(uint32_t *)(node + LIST_A_COUNT), count_b = *(uint32_t *)(node + LIST_B_COUNT);
    // A list that shrank was rebuilt since the frame's prepare; the engine's whole prepare handles it.
    if (count_a < prepared_a || count_b < prepared_b) {
        portrait_stats.fallbacks++;
        original_prepare_now(node);
        return;
    }
    BYTE *items_a = *(BYTE **)(node + LIST_A_ITEMS);
    key_items(node, items_a, prepared_a, count_a);
    key_items(node, *(BYTE **)(node + LIST_B_ITEMS), prepared_b, count_b);
    if (portrait_fill && node[NODE_ROWS_DIRTY] && *(uint32_t *)(node + POSE_COUNT) >= prepared_rows) {
        fill_new_rows(node, prepared_rows);
    } else {
        ((NodeFn)(engine + FILL_TEXTURES_RVA))(node);
        portrait_stats.full_fills++;
    }
    if (count_a - prepared_a > 1) {
        BYTE *tail = items_a + (size_t)prepared_a * RENDER_ITEM_SIZE;
        ((SortFn)(engine + SORT_A_RVA))(tail, items_a + (size_t)count_a * RENDER_ITEM_SIZE, count_a - prepared_a, node + 4);
    }
    portrait_stats.shortcuts++;
}

static uint32_t __attribute__((thiscall)) ui_model_hook(BYTE *self, void *a, void *b, void *c, void *d, void *e) {
    BOOL main = GetCurrentThreadId() == *api->main_thread;
    in_ui_model += main;
    uint32_t result = original_ui_model(self, a, b, c, d, e);
    in_ui_model -= main;
    return result;
}

static void report(BOOL start) {
    static Stats before;
    if (start) {
        before = portrait_stats;
        return;
    }
    LONG64 shortcuts = portrait_stats.shortcuts - before.shortcuts;
    api->log_line("  portraitprep: %lld UI model prepares, %lld shortened (%.1f items keyed and %.1f texture rows written each, %lld whole fills), %lld "
                  "whole",
                  portrait_stats.calls - before.calls, shortcuts, shortcuts ? (double)(portrait_stats.items_keyed - before.items_keyed) / shortcuts : 0.0,
                  shortcuts ? (double)(portrait_stats.rows_written - before.rows_written) / shortcuts : 0.0, portrait_stats.full_fills - before.full_fills,
                  portrait_stats.fallbacks - before.fallbacks);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    frame_stamp = (volatile uint64_t *)(api->engine + FRAME_STAMP_RVA);
    // 0x110ddc00 calls prepare-now through the node's vtbl+0x84 at 0x110ddfdc.
    static const BYTE call_site[] = {0xff, 0x90, 0x84, 0x00, 0x00, 0x00};
    if (memcmp(api->engine + 0x10ddfdc, call_site, sizeof call_site)) return "the UI model's prepare call differs from the analysed build";
    const char *error = api->hook(api->engine + UI_MODEL_RVA, ui_model_prologue, sizeof ui_model_prologue, (const void *)ui_model_hook, (void **)&original_ui_model);
    if (!error)
        error = api->hook(api->engine + PREPARE_RVA, prepare_prologue, sizeof prepare_prologue, (const void *)prepare_hook, (void **)&original_prepare);
    if (!error)
        error = api->hook(api->engine + PREPARE_NOW_RVA, prepare_now_prologue, sizeof prepare_now_prologue, (const void *)prepare_now_hook,
                          (void **)&original_prepare_now);
    if (error) return error;
    api->register_switch("portraitprep", &portrait_prepare, report);
    api->register_switch("portraitfill", &portrait_fill, report);
    return NULL;
}
