#include "../s2module.h"
#include "../checks/listflag.h"

// The render-item list's parallel append (sub_181b3d750, parallel flag set) adds the item to the calling thread's sublist
// and then stores 1 into the list's "needs merge" flag at + 0x30 on every call. TBB workers append to the same list from
// one parallel loop, so that unconditional store kept the list header's cache line moving between cores, and every
// append's read of the header at + 0x14 waited for it. This runs the same fast path but only stores the flag when it is
// clear; the flag ends up set either way and is only cleared by the single-threaded merge. The resize path and
// non-parallel appends still run the engine's code.
#define LIST_APPEND_RVA 0x1b3d750
#define THREAD_REGISTRY_RVA 0x190e2b0
#define SUBLIST_APPEND_RVA 0x1b3f140

typedef DWORD (*ListAppendFn)(BYTE *list, const BYTE *item, BYTE parallel);
typedef BYTE *(*ThreadRegistryFn)(void);
typedef DWORD (*ThreadQueryFn)(BYTE *registry);
typedef void (*SublistAppendFn)(BYTE *sublist, const BYTE *item);

// 0 runs the engine's append for every call; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG enabled = 1;

static const BYTE list_append_prologue[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x41, 0x57, 0x48, 0x83, 0xec, 0x58, 0x48, 0x8b, 0xea};
static ListAppendFn original_list_append;
static ThreadRegistryFn thread_registry;
static SublistAppendFn sublist_append;
static volatile LONG *fix_enabled;

static DWORD list_append_hook(BYTE *list, const BYTE *item, BYTE parallel) {
    if (!parallel || !enabled || !*fix_enabled) return original_list_append(list, item, parallel);
    BYTE *registry = thread_registry();
    if ((*(ThreadQueryFn **)registry)[0](registry) != *(DWORD *)(list + 0x14)) return original_list_append(list, item, parallel);
    registry = thread_registry();
    DWORD thread_index = (*(ThreadQueryFn **)registry)[1](registry);
    BYTE *sublist = *(BYTE **)(list + 0x18) + (size_t)thread_index * 16;
    sublist_append(sublist, item);
    if (!list[0x30]) list[0x30] = 1;
    return (thread_index + 1) << 24 | (*(DWORD *)(sublist + 4) - 1);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, listflag_checks);
    if (mismatch) return mismatch;
    fix_enabled = api->fix_enabled;
    thread_registry = (ThreadRegistryFn)(api->engine + THREAD_REGISTRY_RVA);
    sublist_append = (SublistAppendFn)(api->engine + SUBLIST_APPEND_RVA);
    return api->hook(api->engine + LIST_APPEND_RVA, list_append_prologue, sizeof list_append_prologue, (const void *)list_append_hook,
                     (void **)&original_list_append);
}
