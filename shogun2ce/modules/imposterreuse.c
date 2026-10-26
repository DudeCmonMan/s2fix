#include "../s2module.h"
#include <stdint.h>
#include <intrin.h>

// Each shadow cascade and the main pass upload the same imposter quads into the geometry ring (geometry path
// sub_1819fa680, writer sub_181b76be0): ~208 uploads and 7.6 MB a frame, three quarters of them repeats of an upload
// earlier in the frame. A repeat whose bytes are still in the ring (same holder, no wrap since) reuses that offset
// instead of allocating, mapping and copying again, so the GPU reads the same bytes from the same buffer. Skipping the
// repeats keeps the ring from wrapping mid-frame, so ~150 of the ~156 repeats are reused.
#define GEOMETRY_RVA 0x19fa680
#define WRITER_RVA 0x1b76be0
#define ALLOCATOR_RVA 0x19eb920
#define FRAME_STAMP_RVA 0x2ead830
#define QUAD_BYTES 304
#define SLOT_BITS 10
#define MAX_HOLDERS 16

typedef struct {
    LONG64 uploads, reused, reused_bytes, verify_mismatches, cycles[2], frames[2];
} Stats;

typedef struct {
    BYTE *mesh, *source, *holder;
    uint32_t start, count, bytes, offset;
    uint64_t stamp, epoch, hash;
} Upload;

typedef struct {
    BYTE *holder;
    uint64_t epoch;
} Holder;

typedef uint64_t (*GeometryFn)(void *, BYTE *, BYTE *, BYTE *, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*AllocatorFn)(BYTE *, void *, void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint32_t (*CountFn)(BYTE *);
typedef BYTE *(*StreamWriterFn)(BYTE *, uint32_t);
typedef BYTE (*FlagFn)(BYTE *);

__declspec(dllexport) volatile Stats stats;
// 0 uploads every time; exported so it can be A/B tested while the game runs.
__declspec(dllexport) volatile LONG reuse_enabled = 1;
// 1 hashes the source on every reuse and counts mismatches with the reused upload.
__declspec(dllexport) volatile LONG verify;
// 1 reuses only on even frames and times both kinds, for a same-session comparison.
__declspec(dllexport) volatile LONG alternate_frames;

static const BYTE geometry_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
static const BYTE allocator_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x57};
static BYTE *engine;
static volatile LONG *fix_enabled;
static GeometryFn original_geometry;
static AllocatorFn original_allocator;
static volatile uint64_t *frame_stamp;
static Upload uploads[1 << SLOT_BITS];
static Holder holders[MAX_HOLDERS];
static int holder_count;
// The tables are main-thread only. Execute is the only caller and runs on main until the overhaul's workers start, which
// happens after its serial warm-up, so the first caller is main.
static volatile LONG main_thread;

static BOOL on_main_thread(void) {
    LONG self = (LONG)GetCurrentThreadId();
    InterlockedCompareExchange(&main_thread, self, 0);
    return main_thread == self;
}

static void *method(void *object, int offset) {
    return (*(void ***)object)[offset / 8];
}

// Generation of a ring: bumped whenever an allocation restarts it (wrap, new frame) or replaces its buffer.
static Holder *holder_of(BYTE *holder) {
    for (int i = 0; i < holder_count; i++)
        if (holders[i].holder == holder) return &holders[i];
    // Sharing the last entry only bumps generations more often, which costs reuse, never correctness.
    if (holder_count == MAX_HOLDERS) return &holders[MAX_HOLDERS - 1];
    holders[holder_count] = (Holder){holder, 1};
    return &holders[holder_count++];
}

static uint64_t allocator_hook(BYTE *holder, void *a2, void *a3, uint64_t a4, uint64_t a5, uint32_t *start, uint64_t a7, uint64_t a8) {
    if (!on_main_thread()) return original_allocator(holder, a2, a3, a4, a5, (uint64_t)start, a7, a8);
    uint32_t position = *(uint32_t *)(holder + 0x28), stamp = *(uint32_t *)(holder + 0x78), size = *(uint32_t *)(holder + 0x20);
    BYTE *buffer = *(BYTE **)(holder + 0x18);
    // The allocator rebuilds the buffer, possibly at the same address, when it has none or a resize is pending.
    BOOL rebuilds = !size || (size != *(uint32_t *)(holder + 0x24) && *(BYTE *)(holder + 0x70));
    uint64_t result = original_allocator(holder, a2, a3, a4, a5, (uint64_t)start, a7, a8);
    // A restart always places the allocation at 0; a position compare alone misses one larger than the space it skipped.
    if (rebuilds || *start == 0 || *(uint32_t *)(holder + 0x28) <= position || *(uint32_t *)(holder + 0x78) != stamp ||
        *(BYTE **)(holder + 0x18) != buffer)
        holder_of(holder)->epoch++;
    return result;
}

static uint64_t hash_bytes(const BYTE *data, uint32_t bytes) {
    uint64_t hash = 0x9e3779b97f4a7c15ull;
    for (uint32_t i = 0; i + 8 <= bytes; i += 8) hash = (hash ^ *(const uint64_t *)(data + i)) * 0x100000001b3ull;
    return hash;
}

// Same steps as the engine's geometry path for one imposter stream on the allocating branch; anything else goes to it.
static uint64_t reuse_or_upload(void *a1, BYTE *renderer, BYTE *generator, BYTE *streams, uint64_t fill, uint64_t a6, uint64_t a7, uint64_t a8) {
    if ((BYTE)fill != 1) return original_geometry(a1, renderer, generator, streams, fill, a6, a7, a8);
    uint32_t vertices = ((CountFn)method(generator, 0x78))(generator);
    if (vertices > ((CountFn)method(generator, 0x68))(generator) || ((CountFn)method(generator, 0x88))(generator) != 1)
        return original_geometry(a1, renderer, generator, streams, fill, a6, a7, a8);
    BYTE *writer = ((StreamWriterFn)method(generator, 0x90))(generator, 0);
    if (method(writer, 0) != engine + WRITER_RVA || ((FlagFn)method(generator, 0x108))(generator) != 1 || ((FlagFn)method(generator, 0x130))(generator))
        return original_geometry(a1, renderer, generator, streams, fill, a6, a7, a8);
    BYTE *stream = *(BYTE **)streams, *holder = *(BYTE **)stream, *mesh = *(BYTE **)(writer + 8);
    uint32_t bytes = vertices * *(uint32_t *)(stream + 0xc), start = *(uint32_t *)(mesh + 0x100), count = *(uint32_t *)(mesh + 0x104);
    if (!bytes || !*(BYTE **)(holder + 0x18)) return original_geometry(a1, renderer, generator, streams, fill, a6, a7, a8);
    BYTE *source = *(BYTE **)(*(BYTE **)(mesh + 0xa0) + 8) + (uint64_t)start * QUAD_BYTES;
    uint64_t stamp = *frame_stamp;
    Upload *slot = &uploads[(((uintptr_t)mesh >> 4) * 0x9e3779b97f4a7c15ull + start) >> (64 - SLOT_BITS) & ((1 << SLOT_BITS) - 1)];
    stats.uploads++;
    if (slot->stamp == stamp && slot->mesh == mesh && slot->start == start && slot->count == count && slot->source == source && slot->holder == holder &&
        slot->bytes == bytes && slot->epoch == holder_of(holder)->epoch) {
        if (verify && hash_bytes(source, count * QUAD_BYTES) != slot->hash) stats.verify_mismatches++;
        *(uint32_t *)(stream + 0x10) = bytes;
        *(uint32_t *)(stream + 0x8) = slot->offset;
        *(uint32_t *)(streams + 0x28) = vertices;
        stats.reused++;
        stats.reused_bytes += bytes;
        return 1;
    }
    uint64_t result = original_geometry(a1, renderer, generator, streams, fill, a6, a7, a8);
    if ((BYTE)result && *(uint32_t *)(stream + 0x10) == bytes)
        *slot = (Upload){mesh, source, holder, start, count, bytes, *(uint32_t *)(stream + 0x8), stamp, holder_of(holder)->epoch,
                         verify ? hash_bytes(source, count * QUAD_BYTES) : 0};
    return result;
}

static uint64_t geometry_hook(void *a1, BYTE *renderer, BYTE *generator, BYTE *streams, uint64_t fill, uint64_t a6, uint64_t a7, uint64_t a8) {
    if (!on_main_thread()) return original_geometry(a1, renderer, generator, streams, fill, a6, a7, a8);
    static uint64_t last_stamp, frame_number;
    BOOL new_frame = *frame_stamp != last_stamp;
    if (new_frame) {
        last_stamp = *frame_stamp;
        frame_number++;
    }
    int reuse = *fix_enabled && reuse_enabled && !(alternate_frames && (frame_number & 1));
    if (new_frame) stats.frames[reuse]++;
    uint64_t started = __rdtsc();
    uint64_t result = reuse ? reuse_or_upload(a1, renderer, generator, streams, fill, a6, a7, a8)
                            : original_geometry(a1, renderer, generator, streams, fill, a6, a7, a8);
    stats.cycles[reuse] += __rdtsc() - started;
    return result;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    engine = api->engine;
    fix_enabled = api->fix_enabled;
    frame_stamp = (volatile uint64_t *)(engine + FRAME_STAMP_RVA);
    const char *error = api->hook(engine + ALLOCATOR_RVA, allocator_prologue, sizeof allocator_prologue, (const void *)allocator_hook,
                                  (void **)&original_allocator);
    return error ? error : api->hook(engine + GEOMETRY_RVA, geometry_prologue, sizeof geometry_prologue, (const void *)geometry_hook,
                                     (void **)&original_geometry);
}
