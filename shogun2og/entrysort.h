#pragma once
#include "radixsort.h"

// Objects as the engine's per-entry std::sort sees them: a model pointer at +0x14 (32-bit key at +8, position at +0xd8)
// and a 32-bit id at +0x1d0, ordered by key, then id, then squared distance to the camera.
#define ENTRY_MODEL_OFFSET 0x14
#define ENTRY_ID_OFFSET 0x1d0
#define MODEL_KEY_OFFSET 0x8
#define MODEL_POSITION_OFFSET 0xd8

static uint32_t model_key(const uint8_t *object) {
    const uint8_t *model;
    uint32_t key;
    memcpy(&model, object + ENTRY_MODEL_OFFSET, sizeof model);
    memcpy(&key, model + MODEL_KEY_OFFSET, sizeof key);
    return key;
}

// The comparator's operations: three differences, three squares, (x + y) + z, all in single precision. Built with SSE
// float math and no contraction, equal inputs give bit-identical distances.
static float entry_distance(const uint8_t *object, const float *camera) {
    const uint8_t *model;
    float position[3];
    memcpy(&model, object + ENTRY_MODEL_OFFSET, sizeof model);
    memcpy(position, model + MODEL_POSITION_OFFSET, sizeof position);
    float dx = position[0] - camera[0], dy = position[1] - camera[1], dz = position[2] - camera[2];
    dx *= dx;
    dy *= dy;
    dz *= dz;
    float distance = dx + dy;
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
    for (size_t i = 0; i < count; i++) buffers.keys[i] = (KeyedIndex){id_and_distance_key(first[i], camera), (uint32_t)i};
    KeyedIndex *by_id_and_distance = radix_sort(buffers.keys, buffers.scratch, count);
    KeyedIndex *spare = by_id_and_distance == buffers.keys ? buffers.scratch : buffers.keys;
    for (size_t i = 0; i < count; i++) spare[i] = (KeyedIndex){model_key(first[by_id_and_distance[i].index]), by_id_and_distance[i].index};
    KeyedIndex *sorted = radix_sort(spare, by_id_and_distance, count);
    for (size_t i = 0; i < count; i++) buffers.items[i] = first[i];
    for (size_t i = 0; i < count; i++) first[i] = buffers.items[sorted[i].index];
    return 1;
}
