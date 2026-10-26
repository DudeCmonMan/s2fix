#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wunused-function"
#include "renderitem.h"

// Checks the render-item sorts (radix, the counting sort over distinct keys, the tail merge) against a stable insertion
// sort on the same key words: the lists must come out byte for byte the same.
static void fill(uint8_t *items, size_t count, int meshes, int groups, int textures, unsigned seed) {
    srand(seed);
    static uint64_t mesh_values[8192];
    static uint32_t group_values[8192];
    for (int m = 0; m < 8192; m++) mesh_values[m] = 0x20000000u + (uint64_t)(rand() * 4096 + rand()) * 16 + ((uint64_t)(rand() & 7) << 40);
    for (int g = 0; g < 8192; g++) group_values[g] = 0x30000000u + (uint32_t)(rand() * 4096 + rand()) * 8;
    for (size_t i = 0; i < count; i++) {
        uint8_t *item = items + i * RENDER_ITEM_SIZE;
        for (int b = 0; b < RENDER_ITEM_SIZE; b++) item[b] = (uint8_t)rand();
        uint64_t mesh = mesh_values[rand() % meshes];
        uint32_t group = group_values[rand() % groups], texture = textures == 1 ? 0xffffffffu : (uint32_t)(rand() % textures);
        uint32_t pose = rand() % 4, block = rand() % 3;
        float value = rand() % 7 ? (float)(rand() % 5) - 2.0f : -0.0f;
        memcpy(item + ITEM_MESH_OFFSET, &mesh, 8);
        memcpy(item + ITEM_GROUP_OFFSET, &group, 4);
        memcpy(item + ITEM_TEXTURE_OFFSET, &texture, 4);
        memcpy(item + ITEM_POSE_OFFSET, &pose, 4);
        memcpy(item + ITEM_PARAM_BLOCK_OFFSET, &block, 4);
        memcpy(item + ITEM_PARAM_VALUE_OFFSET, &value, 4);
    }
}

static void reference_sort(ListKind list, int tie_grouping, uint8_t *items, size_t count) {
    uint8_t held[RENDER_ITEM_SIZE];
    for (size_t i = 1; i < count; i++) {
        uint64_t key[MAX_KEY_WORDS], other[MAX_KEY_WORDS];
        int words = item_key(list, tie_grouping, items + i * RENDER_ITEM_SIZE, key);
        memcpy(held, items + i * RENDER_ITEM_SIZE, RENDER_ITEM_SIZE);
        size_t j = i;
        for (; j > 0; j--) {
            item_key(list, tie_grouping, items + (j - 1) * RENDER_ITEM_SIZE, other);
            if (!words_less(key, other, words)) break;
            memcpy(items + j * RENDER_ITEM_SIZE, items + (j - 1) * RENDER_ITEM_SIZE, RENDER_ITEM_SIZE);
        }
        memcpy(items + j * RENDER_ITEM_SIZE, held, RENDER_ITEM_SIZE);
    }
}

int main(void) {
    struct {
        ListKind list;
        int tie;
        size_t count;
        int meshes, groups, textures;
        const char *name;
    } cases[] = {
        {LIST_A, 0, 3000, 53, 1, 1, "list A, few keys"},       {LIST_A, 0, 3000, 300, 1200, 4, "list A, many keys"},
        {LIST_B, 0, 3000, 14, 2, 1, "list B, few keys"},       {LIST_C, 1, 2000, 20, 3, 3, "list C with ties"},
        {LIST_C, 0, 2000, 200, 30, 3, "list C, many keys"},    {LIST_A, 0, 6000, 6000, 3, 1, "list A, too many keys for the counting sort"},
    };
    int failures = 0;
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        size_t bytes = cases[c].count * RENDER_ITEM_SIZE;
        uint8_t *fresh = malloc(bytes), *expected = malloc(bytes), *got = malloc(bytes);
        fill(fresh, cases[c].count, cases[c].meshes, cases[c].groups, cases[c].textures, 1234 + (unsigned)c);
        memcpy(expected, fresh, bytes);
        reference_sort(cases[c].list, cases[c].tie, expected, cases[c].count);
        for (int few_keys = 0; few_keys < 2; few_keys++) {
            memcpy(got, fresh, bytes);
            sort_render_items(cases[c].list, cases[c].tie, got, got + bytes, 0, few_keys, NULL);
            int ok = !memcmp(got, expected, bytes);
            failures += !ok;
            printf("%s, %s: %s\n", cases[c].name, few_keys ? "counting sort" : "radix", ok ? "ok" : "DIFFERS");
        }
        // A sorted list with a short unsorted tail, as the UI's 3D model leaves list A.
        size_t tail = 24;
        memcpy(got, expected, bytes - tail * RENDER_ITEM_SIZE);
        memcpy(got + bytes - tail * RENDER_ITEM_SIZE, fresh, tail * RENDER_ITEM_SIZE);
        memcpy(expected, got, bytes);
        reference_sort(cases[c].list, cases[c].tie, expected, cases[c].count);
        int merged = 0;
        sort_render_items(cases[c].list, cases[c].tie, got, got + bytes, 1, 1, &merged);
        int ok = !memcmp(got, expected, bytes) && merged;
        failures += !ok;
        printf("%s, tail merge: %s\n", cases[c].name, ok ? "ok" : "DIFFERS");
        free(fresh);
        free(expected);
        free(got);
    }
    printf("%d of %d checks failed\n", failures, (int)(sizeof cases / sizeof *cases) * 3);
    return failures != 0;
}
