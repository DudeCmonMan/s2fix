#include "../s2module.h"
#include "../checks/powcache.h"
#include <stdint.h>
#include <string.h>

// The engine converts colours to linear with powf(channel, 2.2) per mesh and per draw; objects share few colours, so
// results are cached by input bits, a hit returning what powf computed for those same bits.
#define POWF_RVA 0x4a1420
#define GAMMA_BITS 0x400ccccdu
#define SLOT_BITS 12

typedef float (*PowfFn)(float, float);

__declspec(dllexport) volatile LONG cache_enabled = 1;

static const BYTE powf_prologue[] = {0xf3, 0x0f, 0x11, 0x4c, 0x24, 0x10, 0xf3, 0x0f, 0x11, 0x44, 0x24, 0x08, 0x48, 0x83, 0xec, 0x28};
static PowfFn original_powf;
static volatile LONG *fix_enabled;
// Input bits high, result bits low: one aligned 8-byte access reads a consistent pair. A zeroed slot is powf(+0, 2.2).
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
    const char *mismatch = S2_CHECK_ENGINE(api, powcache_checks);
    if (mismatch) return mismatch;
    fix_enabled = api->fix_enabled;
    return api->hook(api->engine + POWF_RVA, powf_prologue, sizeof powf_prologue, (const void *)powf_hook, (void **)&original_powf);
}
