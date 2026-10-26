#include "itemsort.h"
#include "entrysort.h"
#include <math.h>
#include <stdio.h>

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint64_t next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static uint64_t make_key(int pattern, size_t i) {
    uint64_t r = next_random();
    switch (pattern) {
    case 0: return r;
    case 1: return r % 7;
    case 2: return 0x8000000000000000ull | (r & 0xff000000ffull);
    case 3: return i;
    case 4: return ~(uint64_t)i;
    case 5: return r & 1 ? UINT64_MAX : 0;
    default: return r % 100 ? i : r % 1000;
    }
}

static float make_value(int pattern, size_t i) {
    uint64_t r = next_random();
    switch (pattern) {
    case 0: return (float)(r % 100000) / 7.0f - 5000.0f;
    case 1: return (float)(r % 5);
    case 2: return r & 1 ? 0.0f : -0.0f;
    case 3: return -(float)i;
    case 4: return (float)i;
    case 5: return r % 3 ? (float)(r % 9) : -1e30f;
    default: return r % 100 ? -(float)i : (float)(r % 1000);
    }
}

static void fill_random(uint8_t *bytes, size_t size) {
    for (size_t b = 0; b < size; b++) bytes[b] = (uint8_t)next_random();
}

static int check_render_items(size_t count, int pattern) {
    uint8_t *items = malloc(count * RENDER_ITEM_SIZE + 1), *original = malloc(count * RENDER_ITEM_SIZE + 1);
    for (size_t i = 0; i < count; i++) {
        uint8_t *item = items + i * RENDER_ITEM_SIZE;
        fill_random(item, RENDER_ITEM_SIZE);
        memcpy(item, &i, sizeof i);
        uint64_t key = make_key(pattern, i);
        memcpy(item + RENDER_ITEM_KEY_OFFSET, &key, sizeof key);
    }
    memcpy(original, items, count * RENDER_ITEM_SIZE);
    size_t descents = 0;
    for (size_t i = 1; i < count; i++) descents += render_item_key(items + i * RENDER_ITEM_SIZE) < render_item_key(items + (i - 1) * RENDER_ITEM_SIZE);
    if (count_render_item_descents(items, count) != descents) return printf("render count=%zu pattern=%d: descents miscounted\n", count, pattern), 0;
    long long moved = sort_render_items(items, items + count * RENDER_ITEM_SIZE);
    if (moved < 0 || (size_t)moved > count) return printf("render count=%zu pattern=%d: moved=%lld\n", count, pattern, moved), 0;
    if (!descents && moved) return printf("render count=%zu pattern=%d: sorted input moved %lld items\n", count, pattern, moved), 0;
    uint8_t *seen = calloc(count + 1, 1);
    for (size_t i = 0; i < count; i++) {
        uint8_t *item = items + i * RENDER_ITEM_SIZE;
        uint64_t id;
        memcpy(&id, item, sizeof id);
        if (id >= count || seen[id]++) return printf("render count=%zu pattern=%d: item %zu duplicated or invalid\n", count, pattern, i), 0;
        if (memcmp(item, original + id * RENDER_ITEM_SIZE, RENDER_ITEM_SIZE)) return printf("render count=%zu pattern=%d: item %zu changed\n", count, pattern, i), 0;
        if (i) {
            uint64_t previous_id, key = render_item_key(item), previous_key = render_item_key(item - RENDER_ITEM_SIZE);
            memcpy(&previous_id, item - RENDER_ITEM_SIZE, sizeof previous_id);
            if (previous_key > key) return printf("render count=%zu pattern=%d: out of order at %zu\n", count, pattern, i), 0;
            if (previous_key == key && previous_id > id) return printf("render count=%zu pattern=%d: equal keys reordered at %zu\n", count, pattern, i), 0;
        }
    }
    free(seen);
    free(items);
    free(original);
    return 1;
}

// Mirrors oneTBB's layout: segment k holds indices [2^k, 2^(k+1)) (segment 0 holds [0, 2)), each allocated separately,
// and the table stores each segment's address minus its first index times the element size.
typedef struct {
    uint8_t header[VECTOR_SEGMENTS_OFFSET];
    uintptr_t *segments;
} FakeVector;

static int check_objects(size_t size, size_t begin, int pattern) {
    uintptr_t table[64] = {0};
    uint8_t *blocks[64] = {0};
    FakeVector vector = {.segments = table};
    for (int k = 0; (k ? (size_t)1 << k : 0) < size; k++) {
        size_t low = k ? (size_t)1 << k : 0, high = (size_t)1 << (k + 1);
        blocks[k] = malloc((high - low) * OBJECT_SIZE);
        table[k] = (uintptr_t)blocks[k] - low * OBJECT_SIZE;
    }
    uint8_t *original = malloc(size * OBJECT_SIZE + 1);
    for (size_t i = 0; i < size; i++) {
        uint8_t *object = vector_object((uint8_t *)&vector, i);
        fill_random(object, OBJECT_SIZE);
        memcpy(object, &i, sizeof i);
        float value = make_value(pattern, i);
        memcpy(object + OBJECT_VALUE_OFFSET, &value, sizeof value);
        memcpy(original + i * OBJECT_SIZE, object, OBJECT_SIZE);
    }
    size_t descents = 0;
    for (size_t i = begin + 1; i < size; i++) descents += object_value(vector_object((uint8_t *)&vector, i)) > object_value(vector_object((uint8_t *)&vector, i - 1));
    if (count_object_descents((uint8_t *)&vector, begin, size) != descents) return printf("object size=%zu pattern=%d: descents miscounted\n", size, pattern), 0;
    long long moved = sort_vector_objects((uint8_t *)&vector, begin, size);
    if (moved < 0) return printf("object size=%zu pattern=%d: allocation failed\n", size, pattern), 0;
    uint8_t *seen = calloc(size + 1, 1);
    for (size_t i = 0; i < size; i++) {
        uint8_t *object = vector_object((uint8_t *)&vector, i);
        uint64_t id;
        memcpy(&id, object, sizeof id);
        if (id >= size || seen[id]++) return printf("object size=%zu pattern=%d: object %zu duplicated or invalid\n", size, pattern, i), 0;
        if (memcmp(object, original + id * OBJECT_SIZE, OBJECT_SIZE)) return printf("object size=%zu pattern=%d: object %zu changed\n", size, pattern, i), 0;
        if (i < begin && id != i) return printf("object size=%zu pattern=%d: object %zu before the range moved\n", size, pattern, i), 0;
        if (i > begin) {
            uint8_t *previous = vector_object((uint8_t *)&vector, i - 1);
            uint64_t previous_id;
            memcpy(&previous_id, previous, sizeof previous_id);
            float value = object_value(object), previous_value = object_value(previous);
            if (value > previous_value) return printf("object size=%zu pattern=%d: out of order at %zu\n", size, pattern, i), 0;
            if (value == previous_value && signbit(value) == signbit(previous_value) && previous_id > id)
                return printf("object size=%zu pattern=%d: equal values reordered at %zu\n", size, pattern, i), 0;
        }
    }
    free(seen);
    free(original);
    for (int k = 0; k < 64; k++) free(blocks[k]);
    return 1;
}

#define ENTRY_OBJECT_SIZE 0x280
#define MODEL_SIZE 0x108

// The engine comparator, written from its disassembly: key, then id (both unsigned), then nearer first.
static int engine_less(const uint8_t *a, const uint8_t *b, const float *camera) {
    uint64_t key_a = model_key(a), key_b = model_key(b);
    if (key_a != key_b) return key_a < key_b;
    uint32_t id_a, id_b;
    memcpy(&id_a, a + ENTRY_ID_OFFSET, sizeof id_a);
    memcpy(&id_b, b + ENTRY_ID_OFFSET, sizeof id_b);
    if (id_a != id_b) return id_a < id_b;
    return entry_distance(b, camera) > entry_distance(a, camera);
}

static int check_entries(size_t count, int pattern) {
    size_t model_count = count / (pattern % 3 + 1) + 1;
    uint8_t *objects = calloc(count + 1, ENTRY_OBJECT_SIZE), *models = calloc(model_count, MODEL_SIZE), **order = malloc((count + 1) * sizeof *order);
    float camera[3] = {(float)(next_random() % 1000) - 500.0f, 20.0f, (float)(next_random() % 1000) - 500.0f};
    for (size_t m = 0; m < model_count; m++) {
        uint8_t *model = models + m * MODEL_SIZE;
        uint64_t key = pattern < 3 ? next_random() % 5 : next_random();
        float position[3] = {(float)(next_random() % 2000) / 3.0f - 300.0f, (float)(next_random() % 50), (float)(next_random() % 2000) / 3.0f - 300.0f};
        memcpy(model + MODEL_KEY_OFFSET, &key, sizeof key);
        memcpy(model + MODEL_POSITION_OFFSET, position, sizeof position);
    }
    for (size_t i = 0; i < count; i++) {
        uint8_t *object = objects + i * ENTRY_OBJECT_SIZE, *model = models + (next_random() % model_count) * MODEL_SIZE;
        uint32_t id = pattern % 2 ? (uint32_t)(next_random() % 3) : (uint32_t)next_random();
        memcpy(object + ENTRY_MODEL_OFFSET, &model, sizeof model);
        memcpy(object + ENTRY_ID_OFFSET, &id, sizeof id);
        order[i] = object;
    }
    if (!sort_entries(order, count, camera)) return printf("entries count=%zu pattern=%d: allocation failed\n", count, pattern), 0;
    uint8_t *seen = calloc(count + 1, 1);
    for (size_t i = 0; i < count; i++) {
        size_t index = (size_t)(order[i] - objects) / ENTRY_OBJECT_SIZE;
        if (index >= count || seen[index]++) return printf("entries count=%zu pattern=%d: object %zu duplicated or invalid\n", count, pattern, i), 0;
        if (i && engine_less(order[i], order[i - 1], camera)) return printf("entries count=%zu pattern=%d: out of order at %zu\n", count, pattern, i), 0;
    }
    free(seen);
    free(order);
    free(models);
    free(objects);
    return 1;
}

int main(void) {
    static const size_t counts[] = {0, 1, 2, 3, 32, 33, 41, 255, 256, 1000, 20000, 100000};
    int failures = 0, runs = 0;
    for (size_t c = 0; c < sizeof counts / sizeof *counts; c++)
        for (int pattern = 0; pattern < 7; pattern++) {
            failures += !check_render_items(counts[c], pattern);
            failures += !check_objects(counts[c], 0, pattern);
            failures += !check_objects(counts[c], counts[c] / 3, pattern);
            failures += !check_entries(counts[c], pattern);
            runs += 4;
        }
    printf("%d/%d sort checks passed\n", runs - failures, runs);
    return failures != 0;
}
