#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Render items: contiguous 176-byte records ordered by the unsigned 64-bit key at +0xa8.
#define RENDER_ITEM_SIZE 0xb0
#define RENDER_ITEM_KEY_OFFSET 0xa8
// Objects: 32-byte records in a tbb::concurrent_vector ordered by the float at +0x18, largest first.
#define OBJECT_SIZE 0x20
#define OBJECT_VALUE_OFFSET 0x18
#define VECTOR_SEGMENTS_OFFSET 0x10
#define PREFETCH_DISTANCE 16
#define CACHE_LINE 64

typedef struct {
    uint64_t key, index;
} KeyedIndex;

typedef struct {
    KeyedIndex *keys, *scratch;
    uint8_t **items;
    size_t capacity;
} SortBuffers;

static __thread SortBuffers buffers;

static int reserve(size_t count) {
    if (count <= buffers.capacity) return 1;
    size_t capacity = count * 2;
    KeyedIndex *keys = realloc(buffers.keys, capacity * sizeof *keys);
    if (keys) buffers.keys = keys;
    KeyedIndex *scratch = realloc(buffers.scratch, capacity * sizeof *scratch);
    if (scratch) buffers.scratch = scratch;
    uint8_t **items = realloc(buffers.items, capacity * sizeof *items);
    if (items) buffers.items = items;
    if (!keys || !scratch || !items) return 0;
    buffers.capacity = capacity;
    return 1;
}

static KeyedIndex *radix_sort(KeyedIndex *keys, KeyedIndex *scratch, size_t count) {
    uint32_t histogram[8][256];
    memset(histogram, 0, sizeof histogram);
    for (size_t i = 0; i < count; i++)
        for (int digit = 0; digit < 8; digit++) histogram[digit][(keys[i].key >> (digit * 8)) & 0xff]++;
    for (int digit = 0; digit < 8; digit++) {
        uint32_t *buckets = histogram[digit];
        if (buckets[(keys[0].key >> (digit * 8)) & 0xff] == count) continue;
        uint32_t offset = 0;
        for (int bucket = 0; bucket < 256; bucket++) {
            uint32_t size = buckets[bucket];
            buckets[bucket] = offset;
            offset += size;
        }
        for (size_t i = 0; i < count; i++) scratch[buckets[(keys[i].key >> (digit * 8)) & 0xff]++] = keys[i];
        KeyedIndex *sorted = scratch;
        scratch = keys;
        keys = sorted;
    }
    return keys;
}

static void prefetch_item(const uint8_t *item, size_t size) {
    for (size_t line = 0; line < size; line += CACHE_LINE) __builtin_prefetch(item + line);
    __builtin_prefetch(item + size - 1);
}

// Follows each permutation cycle so items already in place are never touched and the rest move exactly once; each move
// writes the slot just read, which is still cached. The cycle is walked ahead through the index array so upcoming
// sources are prefetched instead of every move waiting on the previous one's cache miss.
static size_t permute_items(uint8_t **items, KeyedIndex *order, size_t count, size_t size) {
    uint8_t held[RENDER_ITEM_SIZE];
    size_t moved = 0;
    for (size_t start = 0; start < count; start++) {
        if (order[start].index == start) continue;
        memcpy(held, items[start], size);
        size_t hole = start, ahead = start;
        for (int step = 0; step < PREFETCH_DISTANCE && (ahead = order[ahead].index) != start; step++) prefetch_item(items[ahead], size);
        for (size_t source = order[hole].index; source != start; source = order[hole].index) {
            if (ahead != start && (ahead = order[ahead].index) != start) prefetch_item(items[ahead], size);
            memcpy(items[hole], items[source], size);
            order[hole].index = hole;
            hole = source;
            moved++;
        }
        memcpy(items[hole], held, size);
        order[hole].index = hole;
        moved++;
    }
    return moved;
}

static uint64_t render_item_key(const uint8_t *item) {
    uint64_t key;
    memcpy(&key, item + RENDER_ITEM_KEY_OFFSET, sizeof key);
    return key;
}

static size_t count_render_item_descents(const uint8_t *first, size_t count) {
    size_t descents = 0;
    for (size_t i = 1; i < count; i++)
        descents += render_item_key(first + i * RENDER_ITEM_SIZE) < render_item_key(first + (i - 1) * RENDER_ITEM_SIZE);
    return descents;
}

// List A holds ~20,000 items under few distinct keys, so a counting sort over the distinct keys finds the order in one
// pass where the radix takes a histogram pass and a scatter per varying key byte. Same stable order as the radix.
#define FEW_KEYS_MAX 1024
#define FEW_KEYS_SLOTS 4096
// After a list has more distinct keys than that, its next sorts skip the attempt.
#define FEW_KEYS_BACKOFF 16
#define FEW_KEYS_LISTS 4

typedef struct {
    uint64_t keys[FEW_KEYS_MAX];
    uint32_t position[FEW_KEYS_MAX], by_rank[FEW_KEYS_MAX];
    uint16_t slots[FEW_KEYS_SLOTS];
    uint16_t *ids;
    size_t id_capacity;
    int backoff[FEW_KEYS_LISTS];
} FewKeys;

static __thread FewKeys few;

static uint32_t key_slot(uint64_t key) {
    return (uint32_t)((key ^ 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull >> 40) & (FEW_KEYS_SLOTS - 1);
}

static int few_key_less(const void *a, const void *b) {
    uint64_t x = few.keys[*(const uint32_t *)a], y = few.keys[*(const uint32_t *)b];
    return x < y ? -1 : x > y;
}

// Fills order (index = source of each destination) and returns 1, or 0 when there are too many distinct keys or no room.
static int few_keys_order(const uint8_t *first, size_t count, KeyedIndex *order) {
    if (count > few.id_capacity) {
        uint16_t *ids = realloc(few.ids, count * 2 * sizeof *ids);
        if (!ids) return 0;
        few.ids = ids;
        few.id_capacity = count * 2;
    }
    memset(few.slots, 0, sizeof few.slots);
    uint32_t distinct = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t key = render_item_key(first + i * RENDER_ITEM_SIZE);
        for (uint32_t slot = key_slot(key);; slot = (slot + 1) & (FEW_KEYS_SLOTS - 1)) {
            uint32_t id = few.slots[slot];
            if (!id) {
                if (distinct == FEW_KEYS_MAX) return 0;
                few.keys[distinct] = key;
                few.position[distinct] = 0;
                few.slots[slot] = (uint16_t)++distinct;
                id = distinct;
            } else if (few.keys[id - 1] != key) {
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
    for (size_t i = 0; i < count; i++) order[few.position[few.ids[i]]++].index = i;
    return 1;
}

// Items are moved whole: the game's copy only skips padding and unused bitfield bits, which nothing reads. With few_keys
// the counting sort is tried first, per list (below FEW_KEYS_LISTS). Returns the number of items moved, or -1 when the
// buffers cannot be allocated.
static long long sort_render_items(uint8_t *first, uint8_t *last, int few_keys, int list) {
    size_t count = (last - first) / RENDER_ITEM_SIZE;
    if (count < 2) return 0;
    if (!reserve(count)) return -1;
    for (size_t i = 0; i < count; i++) buffers.items[i] = first + i * RENDER_ITEM_SIZE;
    if (few_keys && few.backoff[list]) few.backoff[list]--;
    else if (few_keys && few_keys_order(first, count, buffers.keys)) return permute_items(buffers.items, buffers.keys, count, RENDER_ITEM_SIZE);
    else if (few_keys) few.backoff[list] = FEW_KEYS_BACKOFF;
    for (size_t i = 0; i < count; i++) buffers.keys[i] = (KeyedIndex){render_item_key(buffers.items[i]), i};
    return permute_items(buffers.items, radix_sort(buffers.keys, buffers.scratch, count), count, RENDER_ITEM_SIZE);
}

// oneTBB stores each segment pointer pre-offset by its first index, so an element is segments[bsr(i | 1)] + i * size.
static uint8_t *vector_object(const uint8_t *vector, uint64_t index) {
    const uintptr_t *segments = *(const uintptr_t *const *)(vector + VECTOR_SEGMENTS_OFFSET);
    return (uint8_t *)(segments[63 - __builtin_clzll(index | 1)] + index * OBJECT_SIZE);
}

static float object_value(const uint8_t *object) {
    float value;
    memcpy(&value, object + OBJECT_VALUE_OFFSET, sizeof value);
    return value;
}

// Maps the float to an unsigned key whose ascending order is the game's descending float order.
static uint64_t object_key(const uint8_t *object) {
    uint32_t bits;
    memcpy(&bits, object + OBJECT_VALUE_OFFSET, sizeof bits);
    uint32_t ascending = bits & 0x80000000u ? ~bits : bits | 0x80000000u;
    return (uint32_t)~ascending;
}

static size_t count_object_descents(const uint8_t *vector, uint64_t begin, uint64_t end) {
    size_t descents = 0;
    for (uint64_t i = begin + 1; i < end; i++) descents += object_value(vector_object(vector, i)) > object_value(vector_object(vector, i - 1));
    return descents;
}

// Objects own a heap array; moving their bytes transfers it without the game's allocate-copy-free per move.
static long long sort_vector_objects(const uint8_t *vector, uint64_t begin, uint64_t end) {
    size_t count = end - begin;
    if (count < 2) return 0;
    if (!reserve(count)) return -1;
    for (size_t i = 0; i < count; i++) {
        buffers.items[i] = vector_object(vector, begin + i);
        buffers.keys[i] = (KeyedIndex){object_key(buffers.items[i]), i};
    }
    return permute_items(buffers.items, radix_sort(buffers.keys, buffers.scratch, count), count, OBJECT_SIZE);
}
