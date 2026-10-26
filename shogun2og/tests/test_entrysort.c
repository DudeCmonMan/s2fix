#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "entrysort.h"

// Checks sort_entries against a stable sort with the engine's comparator (0x10237470): key, then id, then distance.
#define OBJECT_BYTES 0x200
#define MODEL_BYTES 0x100
#define MAX_OBJECTS 2000

static int failures, checks;

static float reference_distance(const uint8_t *object, const float *camera) {
    const uint8_t *model;
    float position[3];
    memcpy(&model, object + ENTRY_MODEL_OFFSET, sizeof model);
    memcpy(position, model + MODEL_POSITION_OFFSET, sizeof position);
    // The engine computes one side as (dy^2 + dx^2) + dz^2 and the other as (dx^2 + dy^2) + dz^2; IEEE addition commutes.
    float dx = position[0] - camera[0], dy = position[1] - camera[1], dz = position[2] - camera[2];
    return (dy * dy + dx * dx) + dz * dz;
}

static uint32_t reference_id(const uint8_t *object) {
    uint32_t id;
    memcpy(&id, object + ENTRY_ID_OFFSET, sizeof id);
    return id;
}

static int engine_less(const uint8_t *a, const uint8_t *b, const float *camera) {
    if (model_key(a) != model_key(b)) return model_key(a) < model_key(b);
    if (reference_id(a) != reference_id(b)) return reference_id(a) < reference_id(b);
    return reference_distance(a, camera) < reference_distance(b, camera);
}

static void stable_reference_sort(uint8_t **objects, size_t count, const float *camera) {
    for (size_t i = 1; i < count; i++) {
        uint8_t *held = objects[i];
        size_t j = i;
        while (j && engine_less(held, objects[j - 1], camera)) {
            objects[j] = objects[j - 1];
            j--;
        }
        objects[j] = held;
    }
}

static float random_float(float range) {
    return (float)rand() / RAND_MAX * 2 * range - range;
}

static void run_case(size_t count, int keys, int ids, int positions, float range) {
    static uint8_t object_bytes[MAX_OBJECTS][OBJECT_BYTES], model_bytes[MAX_OBJECTS][MODEL_BYTES];
    static uint8_t *ours[MAX_OBJECTS], *reference[MAX_OBJECTS];
    float camera[3] = {random_float(range), random_float(range), random_float(range)};
    for (size_t i = 0; i < count; i++) {
        uint8_t *model = model_bytes[i % positions], *object = object_bytes[i];
        uint32_t key = (uint32_t)(rand() % keys) * 0x9e3779b1u, id = (uint32_t)(rand() % ids);
        float position[3] = {random_float(range), random_float(range), random_float(range)};
        memcpy(model + MODEL_KEY_OFFSET, &key, sizeof key);
        memcpy(model + MODEL_POSITION_OFFSET, position, sizeof position);
        memcpy(object + ENTRY_MODEL_OFFSET, &model, sizeof model);
        memcpy(object + ENTRY_ID_OFFSET, &id, sizeof id);
        ours[i] = reference[i] = object;
    }
    if (!sort_entries(ours, count, camera)) {
        printf("FAIL: allocation failed for %zu objects\n", count);
        failures++;
        return;
    }
    stable_reference_sort(reference, count, camera);
    checks++;
    for (size_t i = 0; i < count; i++)
        if (ours[i] != reference[i]) {
            printf("FAIL: %zu objects, %d keys, %d ids, %d positions: first difference at %zu\n", count, keys, ids, positions, i);
            failures++;
            return;
        }
}

int main(void) {
    srand(12345);
    const size_t counts[] = {0, 1, 2, 3, 31, 32, 33, 100, 1000, MAX_OBJECTS};
    for (size_t c = 0; c < sizeof counts / sizeof *counts; c++)
        for (int round = 0; round < 8; round++) {
            size_t count = counts[c];
            run_case(count, 1 + rand() % 50, 1 + rand() % 50, 1 + (int)(count ? rand() % count : 0), round & 1 ? 1e4f : 50.0f);
            run_case(count, 1, 1, 1 + (int)(count ? rand() % count : 0), 10.0f);
            run_case(count, 1000, 1000, count ? (int)count : 1, 1e3f);
        }
    printf("%d of %d entry sort checks failed\n", failures, checks);
    return failures != 0;
}
