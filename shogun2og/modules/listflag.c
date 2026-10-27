#include "../s2module.h"
#include "../checks/listflag.h"
#include <stdint.h>
#include <string.h>

// The engine's parallel list appends (13 instantiations, among them the render-item list and the 48-byte instance list)
// add the element to the calling thread's sublist and then store 1 into the list's "needs merge" flag at + 0x24 on every
// call. When TBB workers append to one list from a parallel loop, that unconditional store keeps the list header's cache
// line moving between cores. Each store and the instruction after it (mov byte [ebp+0x24],1; lea ecx,[edi+1]) become a
// call to a stub that stores the flag only when it is not already 1, then does the lea; the flag ends up 1 either way and
// is cleared only by the single-threaded merge. Every site reloads eax and sets the flags right after, so the stub may
// use both. The calls stay installed: a revert could return a thread from the stub into the restored instruction, so
// the switch is read by the stub instead.

// 0 makes the stub store unconditionally, as the engine does; exported, and a switch, so it can be A/B tested.
__declspec(dllexport) volatile LONG conditional_store_enabled = 1;
volatile LONG *listflag_fix_enabled;

void listflag_stub(void);
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _listflag_stub\n"
        "_listflag_stub:\n"
        "  mov eax, dword ptr [_listflag_fix_enabled]\n"
        "  cmp dword ptr [eax], 0\n"
        "  je 2f\n"
        "  cmp dword ptr [_conditional_store_enabled], 0\n"
        "  je 2f\n"
        "  cmp byte ptr [ebp + 0x24], 1\n"
        "  je 1f\n"
        "2:\n"
        "  mov byte ptr [ebp + 0x24], 1\n"
        "1:\n"
        "  lea ecx, [edi + 1]\n"
        "  ret\n"
        ".att_syntax\n");

static const DWORD store_rvas[] = {0x14034fb, 0x148a3ef, 0x148a5af, 0x14d9a9f, 0x14d9c5f, 0x14d9e1f, 0x14d9fdf,
                                   0x14da19f, 0x14da35f, 0x14da51f, 0x14da6df, 0x14da89f, 0x14ec79f};
static const BYTE store_bytes[] = {0xc6, 0x45, 0x24, 0x01, 0x8d, 0x4f, 0x01};
#define SITES (sizeof store_rvas / sizeof *store_rvas)

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, listflag_checks);
    if (mismatch) return mismatch;
    listflag_fix_enabled = api->fix_enabled;
    // Every site is checked before any is patched, so a mismatched build is left untouched.
    for (size_t i = 0; i < SITES; i++)
        if (memcmp(api->engine + store_rvas[i], store_bytes, sizeof store_bytes)) return "a list append differs from the analysed build";
    for (size_t i = 0; i < SITES; i++) {
        BYTE *site = api->engine + store_rvas[i], call[sizeof store_bytes];
        // call rel32 to the stub, computed for the site's address, then two nops.
        LONG offset = (LONG)((BYTE *)(void *)listflag_stub - (site + 5));
        call[0] = 0xe8;
        memcpy(call + 1, &offset, sizeof offset);
        call[5] = call[6] = 0x90;
        const char *error = api->patch(site, store_bytes, call, sizeof call);
        if (error) return error;
    }
    api->register_switch("listflag", &conditional_store_enabled, NULL);
    return NULL;
}
