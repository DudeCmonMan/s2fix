#include "../s2module.h"
#include "../checks/powcache.h"
#include <stdint.h>

// The engine converts colours to linear with powf(channel, 2.2) per mesh and per draw; objects share few colours, so
// results are cached by input bits, a hit returning what powf computed for those same bits.
#define POWF_RVA 0x15136d8
#define GAMMA_BITS 0x400ccccd
#define SLOT_BITS 12
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

__declspec(dllexport) volatile LONG cache_enabled = 1;

static const BYTE powf_prologue[] = {0xf3, 0x0f, 0x10, 0x3d, 0xd4, 0x85, 0x77, 0x11};
// Read by the assembly stub below, so not static.
volatile LONG *powcache_fix_enabled;
volatile LONG *powcache_cache_enabled = &cache_enabled;
void *powcache_original;
// Input bits high, result bits low: one aligned 8-byte access reads a consistent pair. A zeroed slot is powf(+0, 2.2).
volatile uint64_t powcache_slots[1 << SLOT_BITS] __attribute__((aligned(64)));

// Callers expect only eax, ecx, edx, xmm0-xmm7 and the flags to change, as with powf, so the stub uses no more.
void powf_hook(void);
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _powf_hook\n"
        "_powf_hook:\n"
        "  movd eax, xmm1\n"
        "  cmp eax, " STRINGIFY(GAMMA_BITS) "\n"
        "  jne 1f\n"
        "  mov eax, dword ptr [_powcache_fix_enabled]\n"
        "  cmp dword ptr [eax], 0\n"
        "  je 1f\n"
        "  mov eax, dword ptr [_powcache_cache_enabled]\n"
        "  cmp dword ptr [eax], 0\n"
        "  je 1f\n"
        "  movd ecx, xmm0\n"
        "  imul eax, ecx, 0x9e3779b1\n"
        "  shr eax, 32 - " STRINGIFY(SLOT_BITS) "\n"
        "  lea edx, [_powcache_slots + eax * 8]\n"
        "  movq xmm2, qword ptr [edx]\n"
        "  movdqa xmm3, xmm2\n"
        "  psrlq xmm3, 32\n"
        "  movd eax, xmm3\n"
        "  cmp eax, ecx\n"
        "  jne 2f\n"
        "  movss xmm0, xmm2\n"
        "  ret\n"
        "2:\n"
        "  push ecx\n"
        "  push edx\n"
        "  call dword ptr [_powcache_original]\n"
        "  pop edx\n"
        "  pop ecx\n"
        "  movd xmm3, ecx\n"
        "  psllq xmm3, 32\n"
        "  movd eax, xmm0\n"
        "  movd xmm2, eax\n"
        "  por xmm2, xmm3\n"
        "  movq qword ptr [edx], xmm2\n"
        "  ret\n"
        "1:\n"
        "  jmp dword ptr [_powcache_original]\n"
        ".att_syntax\n");

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, powcache_checks);
    if (mismatch) return mismatch;
    powcache_fix_enabled = api->fix_enabled;
    const char *error = api->hook(api->engine + POWF_RVA, powf_prologue, sizeof powf_prologue, (const void *)powf_hook, &powcache_original);
    if (!error) api->register_switch("powcache", &cache_enabled, NULL);
    return error;
}
