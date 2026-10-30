#pragma once
#include "itemsort.h"

// The engine's per-entry std::sort orders by model key, then id, then squared distance to the camera.
#define ENTRY_MODEL_OFFSET 0x28
#define ENTRY_ID_OFFSET 0x27c
#define MODEL_KEY_OFFSET 0x8
#define MODEL_POSITION_OFFSET 0xf8

static uint64_t model_key(const uint8_t *object) {
    const uint8_t *model;
    uint64_t key;
    memcpy(&model, object + ENTRY_MODEL_OFFSET, sizeof model);
    memcpy(&key, model + MODEL_KEY_OFFSET, sizeof key);
    return key;
}

// Same operations in the same order as the comparator, so equal inputs give bit-identical distances.
static float entry_distance(const uint8_t *object, const float *camera) {
    const uint8_t *model;
    float position[3];
    memcpy(&model, object + ENTRY_MODEL_OFFSET, sizeof model);
    memcpy(position, model + MODEL_POSITION_OFFSET, sizeof position);
    float dy = position[1] - camera[1], dx = position[0] - camera[0], dz = position[2] - camera[2];
    dy *= dy;
    dx *= dx;
    dz *= dz;
    float distance = dy + dx;
    distance += dz;
    return distance;
}

// Squared distances are never negative, so their bit patterns order like the floats.
static uint64_t id_and_distance_key(const uint8_t *object, const float *camera) {
    uint32_t id, bits;
    float distance = entry_distance(object, camera);
    memcpy(&id, object + ENTRY_ID_OFFSET, sizeof id);
    memcpy(&bits, &distance, sizeof bits);
    return (uint64_t)id << 32 | bits;
}

// Two stable radix passes: by id and distance, then by model key. Returns 0 when the buffers cannot be allocated.
static int sort_entries(uint8_t **first, size_t count, const float *camera) {
    if (count < 2) return 1;
    if (!reserve(count)) return 0;
    for (size_t i = 0; i < count; i++) buffers.keys[i] = (KeyedIndex){id_and_distance_key(first[i], camera), i};
    KeyedIndex *by_id_and_distance = radix_sort(buffers.keys, buffers.scratch, count);
    KeyedIndex *spare = by_id_and_distance == buffers.keys ? buffers.scratch : buffers.keys;
    for (size_t i = 0; i < count; i++) spare[i] = (KeyedIndex){model_key(first[by_id_and_distance[i].index]), by_id_and_distance[i].index};
    KeyedIndex *sorted = radix_sort(spare, by_id_and_distance, count);
    for (size_t i = 0; i < count; i++) buffers.items[i] = first[i];
    for (size_t i = 0; i < count; i++) first[i] = buffers.items[sorted[i].index];
    return 1;
}
