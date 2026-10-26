#include "../s2module.h"
#include <stdint.h>
#include <string.h>

// The execute step's geometry path (0x11424680, from 0x11422160) uploads each imposter mesh's quads into the geometry
// ring once per pass that draws it: the main pass and every shadow cascade, with the same quads each time. A repeat
// whose bytes are still in the ring (same holder, no wrap, rebuild or new frame since) reuses that upload's offset
// instead of allocating, mapping and copying again, so the GPU reads the same bytes from the same buffer. Only the
// allocating branch for a single-stream imposter mesh (writer 0x1146c800) is replaced; anything else runs the engine's
// path. The ring allocator (0x113fcdc0) is hooked to see every wrap, rebuild and frame change.
#define GEOMETRY_RVA 0x1424680
#define WRITER_RVA 0x146c800
#define ALLOCATOR_RVA 0x13fcdc0
#define G_ENGINE_RVA 0x1c9a920
#define RING_STAMP_OFFSET 0x69ccc
#define QUAD_BYTES 256
#define SLOT_BITS 10
#define MAX_HOLDERS 16

typedef struct {
    LONG64 uploads, reused, reused_bytes, verify_mismatches, foreign_allocations;
} Stats;

typedef struct {
    BYTE *mesh, *source, *holder;
    uint32_t start, count, bytes, offset, stamp, epoch, foreign, hashed;
    uint64_t hash;
} Upload;

typedef struct {
    BYTE *holder;
    uint32_t epoch;
} Holder;

typedef BYTE(__attribute__((thiscall)) *GeometryFn)(void *self, void *renderer, BYTE *generator, BYTE *streams, uint32_t fill);
typedef void *(__attribute__((thiscall)) *AllocatorFn)(BYTE *holder, void *result, void *renderer, uint32_t bytes, uint32_t size, uint32_t *start,
                                                       uint32_t stride, uint32_t map);
typedef uint32_t(__attribute__((thiscall)) *CountFn)(BYTE *generator);
typedef BYTE *(__attribute__((thiscall)) *StreamWriterFn)(BYTE *generator, uint32_t stream);
typedef BYTE(__attribute__((thiscall)) *FlagFn)(BYTE *generator);
typedef void(__attribute__((thiscall)) *SourceFn)(BYTE *mesh, uint32_t unused, BYTE **source);

__declspec(dllexport) volatile Stats stats;
// 0 uploads every time; exported, and a switch, so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG reuse_enabled = 1;
// 1 hashes the source on every reuse and counts mismatches with the reused upload; a switch, off by default.
__declspec(dllexport) volatile LONG verify;

static const BYTE geometry_prologue[] = {0x51, 0x53, 0x55, 0x56, 0x8b, 0x74, 0x24, 0x18};
static const BYTE allocator_prologue[] = {0x51, 0x53, 0x55, 0x56, 0x8b, 0xf1, 0x57};
static const S2Api *api;
static GeometryFn original_geometry;
static AllocatorFn original_allocator;
static Upload uploads[1 << SLOT_BITS];
static Holder holders[MAX_HOLDERS];
static int holder_count;
// The tables belong to the thread that runs the geometry path, main (execute). An allocation on any other thread
// counts as foreign and retires every recorded upload, since its holder's epoch could not be tracked safely.
static volatile DWORD main_thread;
static volatile LONG foreign_allocations;

static void *method(void *object, int offset) {
    return (*(void ***)object)[offset / 4];
}

// Generation of a ring: bumped whenever an allocation restarts it (wrap, new frame) or rebuilds its buffer.
static Holder *holder_of(BYTE *holder) {
    for (int i = 0; i < holder_count; i++)
        if (holders[i].holder == holder) return &holders[i];
    // Sharing the last entry only bumps generations more often, which costs reuse, never correctness.
    if (holder_count == MAX_HOLDERS) return &holders[MAX_HOLDERS - 1];
    holders[holder_count] = (Holder){holder, 1};
    return &holders[holder_count++];
}

static void *__attribute__((thiscall)) allocator_hook(BYTE *holder, void *result, void *renderer, uint32_t bytes, uint32_t size, uint32_t *start,
                                                      uint32_t stride, uint32_t map) {
    if (GetCurrentThreadId() != main_thread) {
        if (main_thread) InterlockedIncrement(&foreign_allocations);
        return original_allocator(holder, result, renderer, bytes, size, start, stride, map);
    }
    uint32_t position = *(uint32_t *)(holder + 0x1c), stamp = *(uint32_t *)(holder + 0x5c), capacity = *(uint32_t *)(holder + 0x14);
    BYTE *buffer = *(BYTE **)(holder + 0x10);
    // The allocator rebuilds the buffer, possibly at the same address, when it has none or a resize is pending.
    BOOL rebuilds = !capacity || (capacity != *(uint32_t *)(holder + 0x18) && *(holder + 0x54));
    void *returned = original_allocator(holder, result, renderer, bytes, size, start, stride, map);
    // A restart always places the allocation at 0; a position compare alone misses one larger than the space it skipped.
    if (rebuilds || *start == 0 || *(uint32_t *)(holder + 0x1c) <= position || *(uint32_t *)(holder + 0x5c) != stamp || *(BYTE **)(holder + 0x10) != buffer)
        holder_of(holder)->epoch++;
    return returned;
}

static uint64_t hash_bytes(const BYTE *data, uint32_t bytes) {
    uint64_t hash = 0x9e3779b97f4a7c15ull;
    for (uint32_t i = 0; i + 8 <= bytes; i += 8) {
        uint64_t word;
        memcpy(&word, data + i, sizeof word);
        hash = (hash ^ word) * 0x100000001b3ull;
    }
    return hash;
}

// The engine's steps for one imposter stream on the allocating branch (0x114245a0), with the repeat taken from the table.
static BYTE reuse_or_upload(void *self, void *renderer, BYTE *generator, BYTE *streams, uint32_t fill) {
    if ((BYTE)fill != 1) return original_geometry(self, renderer, generator, streams, fill);
    uint32_t vertices = ((CountFn)method(generator, 0x38))(generator);
    if (vertices > ((CountFn)method(generator, 0x30))(generator) || ((CountFn)method(generator, 0x40))(generator) != 1)
        return original_geometry(self, renderer, generator, streams, fill);
    BYTE *writer = ((StreamWriterFn)method(generator, 0x44))(generator, 0);
    if (method(writer, 0) != api->engine + WRITER_RVA || ((FlagFn)method(generator, 0x80))(generator) != 1 || ((FlagFn)method(generator, 0x94))(generator))
        return original_geometry(self, renderer, generator, streams, fill);
    BYTE *stream = *(BYTE **)streams, *holder = *(BYTE **)stream, *mesh = *(BYTE **)(writer + 4);
    uint32_t bytes = vertices * *(uint32_t *)(stream + 0x8), start = *(uint32_t *)(mesh + 0xa0), count = *(uint32_t *)(mesh + 0xa4);
    BYTE *engine_object = *(BYTE **)(api->engine + G_ENGINE_RVA);
    if (!bytes || !*(BYTE **)(holder + 0x10) || !engine_object) return original_geometry(self, renderer, generator, streams, fill);
    BYTE *source;
    ((SourceFn)method(mesh, 0xc8))(mesh, 0, &source);
    source += start * QUAD_BYTES;
    uint32_t stamp = *(uint32_t *)(engine_object + RING_STAMP_OFFSET), foreign = (uint32_t)foreign_allocations;
    Upload *slot = &uploads[(((uint32_t)(uintptr_t)mesh >> 4) ^ start * 0x85ebca6bu) * 0x9e3779b1u >> (32 - SLOT_BITS)];
    stats.uploads++;
    if (slot->stamp == stamp && slot->mesh == mesh && slot->start == start && slot->count == count && slot->source == source && slot->holder == holder &&
        slot->bytes == bytes && slot->foreign == foreign && slot->epoch == holder_of(holder)->epoch) {
        if (verify && slot->hashed && hash_bytes(source, count * QUAD_BYTES) != slot->hash) stats.verify_mismatches++;
        *(uint32_t *)(stream + 0xc) = bytes;
        *(uint32_t *)(stream + 0x4) = slot->offset;
        *(uint32_t *)(streams + 0x18) = vertices;
        stats.reused++;
        stats.reused_bytes += bytes;
        return 1;
    }
    BYTE result = original_geometry(self, renderer, generator, streams, fill);
    if (result && *(uint32_t *)(stream + 0xc) == bytes)
        *slot = (Upload){mesh,    source, holder, start, count, bytes, *(uint32_t *)(stream + 0x4), stamp, holder_of(holder)->epoch, foreign,
                         verify != 0, verify ? hash_bytes(source, count * QUAD_BYTES) : 0};
    return result;
}

static BYTE __attribute__((thiscall)) geometry_hook(void *self, void *renderer, BYTE *generator, BYTE *streams, uint32_t fill) {
    if (!main_thread) main_thread = GetCurrentThreadId();
    if (GetCurrentThreadId() != main_thread || !*api->fix_enabled || !reuse_enabled) return original_geometry(self, renderer, generator, streams, fill);
    return reuse_or_upload(self, renderer, generator, streams, fill);
}

static void report(BOOL start) {
    if (start) {
        memset((void *)&stats, 0, sizeof stats);
        foreign_allocations = 0;
        return;
    }
    api->log_line("  imposterreuse: %lld imposter uploads while on, %lld reused (%.1f MB), %ld allocations off the main thread%s", stats.uploads, stats.reused,
                  stats.reused_bytes / 1048576.0, foreign_allocations, verify ? "" : "; verify off");
    if (verify) api->log_line("  imposterreuse: %lld reused uploads whose source changed", stats.verify_mismatches);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    if (!s2_prologue_ready(api->engine + GEOMETRY_RVA, geometry_prologue, sizeof geometry_prologue) ||
        !s2_prologue_ready(api->engine + ALLOCATOR_RVA, allocator_prologue, sizeof allocator_prologue))
        return "the geometry path or the ring allocator differs from the analysed build";
    const char *error = api->hook(api->engine + ALLOCATOR_RVA, allocator_prologue, sizeof allocator_prologue, (const void *)allocator_hook,
                                  (void **)&original_allocator);
    if (!error)
        error = api->hook(api->engine + GEOMETRY_RVA, geometry_prologue, sizeof geometry_prologue, (const void *)geometry_hook, (void **)&original_geometry);
    if (error) return error;
    api->register_switch("imposterreuse", &reuse_enabled, report);
    api->register_switch("imposterverify", &verify, NULL);
    return NULL;
}
