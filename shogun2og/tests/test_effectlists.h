#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The engine's effect lists for the tests: {capacity, count, data} of 20-byte entries, ordered by the first key, the
// fourth (signed), then the second and third (unsigned); the copy, the merge-sum and the value lookup as the engine's
// code does them.
typedef struct {
    int32_t first;
    uint32_t second, third;
    int32_t fourth;
    float value;
} Effect;

typedef struct {
    uint32_t capacity, count;
    Effect *data;
} EffectList;

static long effect_blocks;

static Effect *effects_alloc(uint32_t count) {
    if (!count) return NULL;
    effect_blocks++;
    return malloc(count * sizeof(Effect));
}

static void effects_free(EffectList *list) {
    if (list->data) effect_blocks--, free(list->data);
    list->data = NULL;
    list->capacity = list->count = 0;
}

static int effect_less(const Effect *a, const Effect *b) {
    if (a->first != b->first) return a->first < b->first;
    if (a->fourth != b->fourth) return a->fourth < b->fourth;
    if (a->second != b->second) return a->second < b->second;
    return a->third < b->third;
}

static int effect_same_key(const Effect *a, const Effect *b) {
    return a->first == b->first && a->second == b->second && a->third == b->third && a->fourth == b->fourth;
}

static void effects_copy(EffectList *out, const EffectList *list) {
    out->capacity = out->count = list->count;
    out->data = effects_alloc(list->count);
    if (list->count) memcpy(out->data, list->data, list->count * sizeof(Effect));
}

// Equal keys store source + destination and advance both; the rest is copied in order, then whatever remains.
static void effects_merge_sum(EffectList *dest, const EffectList *src) {
    if (!src->count) return;
    if (!dest->count) {
        effects_free(dest);
        effects_copy(dest, src);
        return;
    }
    EffectList old = *dest;
    uint32_t capacity = old.count + src->count;
    Effect *out = effects_alloc(capacity);
    uint32_t i = 0, j = 0, n = 0;
    while (i < old.count && j < src->count) {
        if (effect_less(&old.data[i], &src->data[j]))
            out[n++] = old.data[i++];
        else if (effect_same_key(&old.data[i], &src->data[j])) {
            old.data[i].value = src->data[j].value + old.data[i].value;
            out[n++] = old.data[i++];
            j++;
        } else
            out[n++] = src->data[j++];
    }
    while (i < old.count) out[n++] = old.data[i++];
    while (j < src->count) out[n++] = src->data[j++];
    effects_free(&old);
    dest->data = out;
    dest->count = n;
    dest->capacity = capacity;
}

// A lower bound, then the key's equality; 0 when absent.
static float effects_value(const EffectList *list, const Effect *key) {
    uint32_t first = 0, length = list->count;
    while (length) {
        uint32_t half = length / 2;
        if (effect_less(&list->data[first + half], key))
            first += half + 1, length -= half + 1;
        else
            length = half;
    }
    return first < list->count && effect_same_key(&list->data[first], key) ? list->data[first].value : 0.0f;
}
