#pragma once
#include "radixsort.h"

// The Variant node's 312-byte render items as the three list sorts compare them (insertion sorts 0x114a9380, 0x114a8f60,
// 0x114a97c0). The engine's copies skip only the padding at +0x121..+0x123 and +0x135..+0x137, so whole-item moves are
// equivalent to its field-by-field ones.
#define RENDER_ITEM_SIZE 0x138
#define ITEM_POSE_OFFSET 0x00
#define ITEM_MESH_OFFSET 0xe8
#define ITEM_GROUP_OFFSET 0x118
#define ITEM_TEXTURE_OFFSET 0x124
#define ITEM_PARAM_BLOCK_OFFSET 0x12c
#define ITEM_PARAM_VALUE_OFFSET 0x130
#define MAX_KEY_WORDS 4
#define PREFETCH_DISTANCE 16
#define CACHE_LINE 64
#define TAIL_MAX 64

// Cycles this thread spent moving items in sort_render_items; a caller reads its delta.
static __thread uint64_t permute_cycles;

// List A (prepare): mesh, texture index, group. List B (record): mesh, group. List C (record): mesh, pose, param block,
// param value, group; with tie grouping, the texture index last.
typedef enum { LIST_A, LIST_B, LIST_C, LISTS } ListKind;

static uint32_t item_u32(const uint8_t *item, size_t offset) {
    uint32_t value;
    memcpy(&value, item + offset, sizeof value);
    return value;
}

static uint64_t item_u64(const uint8_t *item, size_t offset) {
    uint64_t value;
    memcpy(&value, item + offset, sizeof value);
    return value;
}

// The engine compares param values as floats, so +0 and -0 are equal; NaNs order arbitrarily for the engine and last here.
static uint32_t ordered_float(uint32_t bits) {
    if (bits == 0x80000000u) bits = 0;
    return bits & 0x80000000u ? ~bits : bits | 0x80000000u;
}

// Key words, most significant first.
static int item_key(ListKind list, int tie_grouping, const uint8_t *item, uint64_t *words) {
    words[0] = item_u64(item, ITEM_MESH_OFFSET);
    switch (list) {
    case LIST_A:
        words[1] = (uint64_t)item_u32(item, ITEM_TEXTURE_OFFSET) << 32 | item_u32(item, ITEM_GROUP_OFFSET);
        return 2;
    case LIST_B:
        words[1] = item_u32(item, ITEM_GROUP_OFFSET);
        return 2;
    default:
        words[1] = (uint64_t)item_u32(item, ITEM_POSE_OFFSET) << 32 | item_u32(item, ITEM_PARAM_BLOCK_OFFSET);
        words[2] = (uint64_t)ordered_float(item_u32(item, ITEM_PARAM_VALUE_OFFSET)) << 32 | item_u32(item, ITEM_GROUP_OFFSET);
        if (!tie_grouping) return 3;
        words[3] = item_u32(item, ITEM_TEXTURE_OFFSET);
        return 4;
    }
}

static int words_less(const uint64_t *a, const uint64_t *b, int count) {
    for (int i = 0; i < count; i++)
        if (a[i] != b[i]) return a[i] < b[i];
    return 0;
}

static void prefetch_item(const uint8_t *item) {
    for (size_t line = 0; line < RENDER_ITEM_SIZE; line += CACHE_LINE) __builtin_prefetch(item + line);
    __builtin_prefetch(item + RENDER_ITEM_SIZE - 1);
}

// Follows each permutation cycle so items already in place are never touched and the rest move exactly once; upcoming
// sources are prefetched along the cycle.
static size_t permute_items(uint8_t *first, KeyedIndex *order, size_t count) {
    uint8_t held[RENDER_ITEM_SIZE];
    size_t moved = 0;
    for (size_t start = 0; start < count; start++) {
        if (order[start].index == start) continue;
        memcpy(held, first + start * RENDER_ITEM_SIZE, RENDER_ITEM_SIZE);
        size_t hole = start, ahead = start;
        for (int step = 0; step < PREFETCH_DISTANCE && (ahead = order[ahead].index) != start; step++) prefetch_item(first + ahead * RENDER_ITEM_SIZE);
        for (size_t source = order[hole].index; source != start; source = order[hole].index) {
            if (ahead != start && (ahead = order[ahead].index) != start) prefetch_item(first + ahead * RENDER_ITEM_SIZE);
            memcpy(first + hole * RENDER_ITEM_SIZE, first + source * RENDER_ITEM_SIZE, RENDER_ITEM_SIZE);
            order[hole].index = hole;
            hole = source;
            moved++;
        }
        memcpy(first + hole * RENDER_ITEM_SIZE, held, RENDER_ITEM_SIZE);
        order[hole].index = hole;
        moved++;
    }
    return moved;
}

// A sorted list with a few items appended (the UI's 3D model adds ~8 to list A's ~8,400 and prepares the node again): the
// tail is sorted on its own and merged in from the back, the prefix moving in blocks, so only the items after the first
// insertion move, each once. On ties the prefix's items stay first, which is the stable radix sort's order.
static long long merge_tail(ListKind list, int tie_grouping, uint8_t *first, size_t prefix, size_t count) {
    static __thread uint8_t held[TAIL_MAX][RENDER_ITEM_SIZE];
    uint64_t keys[TAIL_MAX][MAX_KEY_WORDS], key[MAX_KEY_WORDS];
    size_t tail = count - prefix;
    int words = 0;
    for (size_t t = 0; t < tail; t++) {
        const uint8_t *item = first + (prefix + t) * RENDER_ITEM_SIZE;
        words = item_key(list, tie_grouping, item, key);
        size_t j = t;
        for (; j > 0 && words_less(key, keys[j - 1], words); j--) {
            memcpy(keys[j], keys[j - 1], sizeof keys[j]);
            memcpy(held[j], held[j - 1], RENDER_ITEM_SIZE);
        }
        memcpy(keys[j], key, sizeof key);
        memcpy(held[j], item, RENDER_ITEM_SIZE);
    }
    size_t end = count, unplaced = prefix;
    for (size_t t = tail; t-- > 0;) {
        size_t low = 0, high = unplaced;
        while (low < high) {
            size_t middle = (low + high) / 2;
            item_key(list, tie_grouping, first + middle * RENDER_ITEM_SIZE, key);
            if (words_less(keys[t], key, words)) high = middle;
            else low = middle + 1;
        }
        size_t block = unplaced - low;
        if (block) memmove(first + (end - block) * RENDER_ITEM_SIZE, first + low * RENDER_ITEM_SIZE, block * RENDER_ITEM_SIZE);
        end -= block + 1;
        unplaced = low;
        memcpy(first + end * RENDER_ITEM_SIZE, held[t], RENDER_ITEM_SIZE);
    }
    return (long long)(count - end);
}

// List A holds ~7,300 items under ~50 distinct keys, so a counting sort over the distinct keys finds the order in one
// pass over the items where the radix takes one pass per key word plus its digit passes. Same stable order as the radix.
#define FEW_KEYS_MAX 1024
#define FEW_KEYS_SLOTS 4096
// After a list kind has more distinct keys than that, its next sorts skip the attempt.
#define FEW_KEYS_BACKOFF 16

typedef struct {
    uint64_t words[MAX_KEY_WORDS];
} FullKey;

typedef struct {
    FullKey keys[FEW_KEYS_MAX];
    uint32_t rank[FEW_KEYS_MAX], position[FEW_KEYS_MAX], by_rank[FEW_KEYS_MAX];
    uint16_t slots[FEW_KEYS_SLOTS];
    uint16_t *ids;
    size_t id_capacity;
    int words;
    int backoff[LISTS];
} FewKeys;

static __thread FewKeys few;

static uint32_t key_slot(const uint64_t *words, int count) {
    uint64_t hash = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < count; i++) hash = (hash ^ words[i]) * 0xff51afd7ed558ccdull;
    return (uint32_t)(hash >> 40) & (FEW_KEYS_SLOTS - 1);
}

static int few_key_less(const void *a, const void *b) {
    const uint64_t *x = few.keys[*(const uint32_t *)a].words, *y = few.keys[*(const uint32_t *)b].words;
    return words_less(x, y, few.words) ? -1 : words_less(y, x, few.words) ? 1 : 0;
}

// Fills order (index = source of each destination) and returns 1, or 0 when there are too many distinct keys or no room.
static int few_keys_order(ListKind list, int tie_grouping, const uint8_t *first, size_t count, KeyedIndex *order) {
    if (count > few.id_capacity) {
        uint16_t *ids = realloc(few.ids, count * 2 * sizeof *ids);
        if (!ids) return 0;
        few.ids = ids;
        few.id_capacity = count * 2;
    }
    memset(few.slots, 0, sizeof few.slots);
    uint32_t distinct = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t key[MAX_KEY_WORDS] = {0};
        few.words = item_key(list, tie_grouping, first + i * RENDER_ITEM_SIZE, key);
        uint32_t slot = key_slot(key, few.words);
        for (;;) {
            uint32_t id = few.slots[slot];
            if (!id) {
                if (distinct == FEW_KEYS_MAX) return 0;
                memcpy(few.keys[distinct].words, key, sizeof key);
                few.position[distinct] = 0;
                few.slots[slot] = (uint16_t)++distinct;
                id = distinct;
            } else if (memcmp(few.keys[id - 1].words, key, sizeof key)) {
                slot = (slot + 1) & (FEW_KEYS_SLOTS - 1);
                continue;
            }
            few.ids[i] = (uint16_t)(id - 1);
            few.position[id - 1]++;
            break;
        }
    }
    for (uint32_t id = 0; id < distinct; id++) few.by_rank[id] = id;
    qsort(few.by_rank, distinct, sizeof *few.by_rank, few_key_less);
    uint32_t offset = 0;
    for (uint32_t rank = 0; rank < distinct; rank++) {
        uint32_t id = few.by_rank[rank], size = few.position[id];
        few.position[id] = offset;
        offset += size;
    }
    for (size_t i = 0; i < count; i++) order[few.position[few.ids[i]]++].index = (uint32_t)i;
    return 1;
}

// Stable LSD radix over the key words, least significant word first, or merge_tail when only a short tail is out of
// order (tail_merge, and *merged set), or the counting sort over distinct keys (few_keys) when there are few. Returns the
// number of items moved, 0 when the list was already in order (then nothing is touched), or -1 when the buffers cannot
// be allocated.
static long long sort_render_items(ListKind list, int tie_grouping, uint8_t *first, uint8_t *last, int tail_merge, int few_keys, int *merged) {
    size_t count = (last - first) / RENDER_ITEM_SIZE;
    if (count < 2) return 0;
    uint64_t previous[MAX_KEY_WORDS], current[MAX_KEY_WORDS];
    int words = item_key(list, tie_grouping, first, previous);
    size_t i = 1;
    for (; i < count; i++) {
        item_key(list, tie_grouping, first + i * RENDER_ITEM_SIZE, current);
        if (words_less(current, previous, words)) break;
        memcpy(previous, current, sizeof previous);
    }
    if (i == count) return 0;
    if (tail_merge && count - i <= TAIL_MAX && (count - i) * 8 <= count) {
        if (merged) *merged = 1;
        return merge_tail(list, tie_grouping, first, i, count);
    }
    if (!reserve(count)) return -1;
    KeyedIndex *sorted = buffers.keys, *scratch = buffers.scratch;
    int counted = 0;
    if (few_keys && few.backoff[list]) few.backoff[list]--;
    else if (few_keys && !(counted = few_keys_order(list, tie_grouping, first, count, sorted))) few.backoff[list] = FEW_KEYS_BACKOFF;
    if (!counted) {
        for (size_t j = 0; j < count; j++) sorted[j].index = (uint32_t)j;
        for (int word = words - 1; word >= 0; word--) {
            for (size_t j = 0; j < count; j++) {
                uint64_t key[MAX_KEY_WORDS];
                item_key(list, tie_grouping, first + (size_t)sorted[j].index * RENDER_ITEM_SIZE, key);
                sorted[j].key = key[word];
            }
            KeyedIndex *result = radix_sort(sorted, scratch, count);
            scratch = result == sorted ? scratch : sorted;
            sorted = result;
        }
    }
    uint64_t started = __builtin_ia32_rdtsc();
    long long moved = (long long)permute_items(first, sorted, count);
    permute_cycles += __builtin_ia32_rdtsc() - started;
    return moved;
}
