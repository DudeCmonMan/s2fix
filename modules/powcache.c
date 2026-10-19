#include "../s2module.h"
#include <stdint.h>
#include <string.h>

// The engine converts colours from gamma to linear space with powf(channel, 2.2) per object and per draw (sub_1819e3a20
// from the render-item task and battle logic, effect parameter writers), ~2.5% of the main thread inside the CRT's pow.
// Objects share few colours, so results are cached by input bits; a hit returns what powf computed for those same bits.
#define POWF_RVA 0x4a1420
#define GAMMA_BITS 0x400ccccdu
#define SLOT_BITS 12

typedef float (*PowfFn)(float, float);

// 0 always calls powf; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG cache_enabled = 1;

static const BYTE powf_prologue[] = {0xf3, 0x0f, 0x11, 0x4c, 0x24, 0x10, 0xf3, 0x0f, 0x11, 0x44, 0x24, 0x08, 0x48, 0x83, 0xec, 0x28};
static PowfFn original_powf;
static volatile LONG *fix_enabled;
// Input bits in the high half, result bits in the low half: one aligned 64-bit access is always a consistent pair across
// the worker threads. A zeroed slot reads as powf(+0, 2.2) = +0, which is correct.
static volatile uint64_t slots[1 << SLOT_BITS];

static float powf_hook(float base, float exponent) {
    uint32_t base_bits, exponent_bits, result_bits;
    memcpy(&base_bits, &base, 4);
    memcpy(&exponent_bits, &exponent, 4);
    if (exponent_bits != GAMMA_BITS || !*fix_enabled || !cache_enabled) return original_powf(base, exponent);
    volatile uint64_t *slot = &slots[(base_bits * 2654435761u) >> (32 - SLOT_BITS)];
    uint64_t entry = *slot;
    if ((uint32_t)(entry >> 32) == base_bits) {
        float cached;
        result_bits = (uint32_t)entry;
        memcpy(&cached, &result_bits, 4);
        return cached;
    }
    float result = original_powf(base, exponent);
    memcpy(&result_bits, &result, 4);
    *slot = (uint64_t)base_bits << 32 | result_bits;
    return result;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    fix_enabled = api->fix_enabled;
    return api->hook(api->engine + POWF_RVA, powf_prologue, sizeof powf_prologue, (const void *)powf_hook, (void **)&original_powf);
}
