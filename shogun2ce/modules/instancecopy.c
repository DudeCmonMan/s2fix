#include "../s2module.h"
#include "../checks/instancecopy.h"
#include <stdint.h>
#include <string.h>
#include <emmintrin.h>
#include <intrin.h>

// The instance gathers use their 16-byte loop only when source, destination and size are 16-aligned, else memcpy;
// this does the same copies with unaligned 16-byte moves.
#define INSTANCE_GATHER_RVA 0x1b21d80
#define RECORD_GATHER_RVA 0x1b21cd0

typedef struct {
    uint32_t bytes, stride;
    uint8_t *data;
} InstanceBuffer;

typedef struct {
    LONG64 calls[2], records[2], cycles[2], odd_sizes;
} InstanceCopyStats;

// [0] the instance gather, [1] the record gather.
__declspec(dllexport) volatile InstanceCopyStats instance_copy_stats[2];
// 1 alternates the engine's and the new copy call by call, timing both.
__declspec(dllexport) volatile LONG alternate_for_comparison = 0;

typedef uint8_t *(*RecordLookupFn)(void *lookup, uint32_t id);

typedef uint64_t (*GatherFn)(uint8_t *gatherer, InstanceBuffer *buffer, const uint32_t *ids);

static const BYTE instance_gather_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
static const BYTE record_gather_prologue[] = {0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57};
static GatherFn original_instance_gather, original_record_gather;
static volatile LONG *fix_enabled;

// The tail is one more 16-byte move ending at the record's end, rewriting a few copied bytes with the same values.
static void copy_record(uint8_t *destination, const uint8_t *source, uint32_t size) {
    if (size < 16) {
        memcpy(destination, source, size);
        return;
    }
    uint32_t copied = 0;
    for (; copied + 16 <= size; copied += 16)
        _mm_storeu_si128((__m128i *)(destination + copied), _mm_loadu_si128((const __m128i *)(source + copied)));
    if (copied < size) _mm_storeu_si128((__m128i *)(destination + size - 16), _mm_loadu_si128((const __m128i *)(source + size - 16)));
}

// The engine's loop, re-reading the record count each iteration; the record size is stored just before the gatherer.
static uint64_t gather(int which, uint8_t *gatherer, InstanceBuffer *buffer, const uint32_t *ids) {
    uint32_t size = *(const uint32_t *)(gatherer - 8), offset = 0, lookup_slot = which ? 0 : 1, source_offset = which ? 0 : 8;
    void *lookup = *(void **)(gatherer + 0x10);
    for (uint32_t i = 0; i < buffer->bytes / buffer->stride; i++, offset += size) {
        RecordLookupFn lookup_record = (*(RecordLookupFn **)lookup)[lookup_slot];
        copy_record(buffer->data + offset, lookup_record(lookup, ids[i]) + source_offset, size);
    }
    if (size % 16) instance_copy_stats[which].odd_sizes++;
    return buffer->bytes / buffer->stride;
}

// Plain increments: only main gathers, and a locked op would wait for the write-combined buffer's stores to drain.
static uint64_t timed_gather(int which, GatherFn original, uint8_t *gatherer, InstanceBuffer *buffer, const uint32_t *ids) {
    if (!*fix_enabled) return original(gatherer, buffer, ids);
    static LONG64 call_number;
    int engine = alternate_for_comparison && (++call_number & 1);
    uint64_t started = __rdtsc();
    uint64_t records = engine ? original(gatherer, buffer, ids) : gather(which, gatherer, buffer, ids);
    instance_copy_stats[which].cycles[engine] += __rdtsc() - started;
    instance_copy_stats[which].calls[engine]++;
    instance_copy_stats[which].records[engine] += records;
    return records;
}

static uint64_t instance_gather_hook(uint8_t *gatherer, InstanceBuffer *buffer, const uint32_t *ids) {
    return timed_gather(0, original_instance_gather, gatherer, buffer, ids);
}

static uint64_t record_gather_hook(uint8_t *gatherer, InstanceBuffer *buffer, const uint32_t *ids) {
    return timed_gather(1, original_record_gather, gatherer, buffer, ids);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *api) {
    const char *mismatch = S2_CHECK_ENGINE(api, instancecopy_checks);
    if (mismatch) return mismatch;
    fix_enabled = api->fix_enabled;
    const char *error = api->hook(api->engine + INSTANCE_GATHER_RVA, instance_gather_prologue, sizeof instance_gather_prologue,
                                  (const void *)instance_gather_hook, (void **)&original_instance_gather);
    return error ? error : api->hook(api->engine + RECORD_GATHER_RVA, record_gather_prologue, sizeof record_gather_prologue, (const void *)record_gather_hook,
                                     (void **)&original_record_gather);
}
