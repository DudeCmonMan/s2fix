// The overhaul's own defines, which its includes below need.
#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d11_4.h>
#include <intrin.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The overhaul against a stand-in engine whose allocations end at a no-access page: a load must leave no writer reading
// the old scene, and the library scan must skip memory that only looks like a parameter (s2fix issue 7's crashes).
static void *guarded_malloc(size_t bytes) {
    size_t rounded = (bytes + 15) & ~(size_t)15, pages = (rounded + 4095) / 4096;
    BYTE *base = VirtualAlloc(NULL, (pages + 1) * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD protection;
    if (!base || !VirtualProtect(base + pages * 4096, 4096, PAGE_NOACCESS, &protection)) return NULL;
    return base + pages * 4096 - rounded;
}

static void *guarded_calloc(size_t count, size_t bytes) {
    return guarded_malloc(count * bytes);
}

static void guarded_free(void *block) {
    MEMORY_BASIC_INFORMATION region;
    if (block && VirtualQuery(block, &region, sizeof region)) VirtualFree(region.AllocationBase, 0, MEM_RELEASE);
}

#define malloc guarded_malloc
#define calloc guarded_calloc
#define free guarded_free
#undef _WIN32_WINNT
#undef COBJMACROS
#include "modules/overhaul.c"

#define SCENE_BYTES (LIBRARY_OFFSET + LIBRARY_SIZE)
#define LOCKED_SLOT 3
#define UNLOCKED_SLOT 1
// Stale bytes where a constant buffer's device object was: two floats, not a pointer.
#define REUSED_BYTES 0x3f00000044a8b000ull

typedef struct {
    BYTE *binding, *parameter, *shadow;
} FakeBuffer;

static int failures, checks, scene_changes;
static const char *running;
static BYTE engine_object[0x40];
static uint64_t stamp = 1;
static void *effect_vtable[0xc0 / 8];
static void **effect = effect_vtable;
static BYTE *written;

static void test_log(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    scene_changes += strstr(line, "a load replaced the scene") != NULL;
    printf("  log: %s\n", line);
}

static const S2Api test_api = {.log_line = test_log};

static LONG CALLBACK fault(EXCEPTION_POINTERS *exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    printf("FAIL %s: access violation reading 0x%llx at %p\n", running, (unsigned long long)exception->ExceptionRecord->ExceptionInformation[1],
           exception->ExceptionRecord->ExceptionAddress);
    printf("test_overhaul: %d checks, %d failures before the fault\n", checks, failures);
    fflush(stdout);
    ExitProcess(1);
}

static void check(BOOL ok, const char *what) {
    checks++;
    failures += !ok;
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
}

// The engine's constructor, as far as the overhaul reads it.
static BYTE *new_scene(BYTE *at) {
    memset(at, 0, SCENE_BYTES);
    *(BYTE **)(engine_object + 0x30) = at;
    return at;
}

// The device object and the shadow come from the heap, as the engine's do.
static FakeBuffer add_buffer(BYTE *scene, uint32_t at, uint32_t parameter_at, uint32_t bytes, uint32_t value_offset) {
    BYTE *library = scene + LIBRARY_OFFSET, *device = calloc(1, 0x50);
    FakeBuffer fake = {library + at, library + parameter_at, calloc(1, bytes)};
    *(BYTE **)(device + 0x40) = fake.shadow;
    *(uint32_t *)(device + 0x48) = bytes;
    *(BYTE **)(fake.binding + 0x30) = device;
    *(BYTE **)(fake.parameter + 0x18) = fake.binding;
    *(BYTE **)(fake.parameter + 0x28) = fake.shadow + value_offset;
    return fake;
}

static uint64_t test_writer(uint64_t object, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)object, (void)b, (void)c, (void)d, (void)e, (void)f;
    **(float **)(written + 0x28) += 1.0f;
    touch_hook(written);
    return 0;
}

// One split view on main, as execute_hook runs it: the fork, one writer call in main's range, the join.
static void draw_view(int slot, BYTE *parameter) {
    static ViewSlot view;
    written = parameter;
    wrap_writers(&view);
    begin_values(&main_values);
    split_thread = GetCurrentThreadId();
    values_active = 1;
    call_writer(slot, (uint64_t)&effect, 0, 0, 0, 0, 0);
    values_active = 0;
    write_back_values(&main_values);
    main_values.drawing = FALSE;
}

// After a load the old scene's memory holds other data; a locked slot must not read a buffer pointer from it.
static void stale_learned(void) {
    running = "a locked writer after a load";
    BYTE *old_scene = new_scene(VirtualAlloc(NULL, SCENE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    FakeBuffer old_buffer = add_buffer(old_scene, 0x1000, 0x2000, 16, 0);
    draw_view(LOCKED_SLOT, old_buffer.parameter);
    int changes = scene_changes;
    WrappedClass *wrapped = wrapped_class(effect_vtable);
    check(wrapped->learned[LOCKED_SLOT][0] == old_buffer.binding, "the locked slot learned the buffer it wrote");
    BYTE *scene = new_scene(VirtualAlloc(NULL, SCENE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    FakeBuffer buffer = add_buffer(scene, 0x1000, 0x2000, 16, 0);
    for (uint64_t *q = (uint64_t *)old_scene; q < (uint64_t *)(old_scene + SCENE_BYTES); q++) *q = REUSED_BYTES;
    draw_view(LOCKED_SLOT, buffer.parameter);
    check(wrapped->learned[LOCKED_SLOT][0] == buffer.binding && !wrapped->learned[LOCKED_SLOT][1],
          "after a load the slot knows only the new scene's buffer");
    check(*(float *)buffer.shadow == 1.0f, "the new scene's buffer holds the writer's value");
    check(scene_changes == changes + 1, "the load is logged once");
}

// A new scene over the old can put a larger buffer where a smaller one was, past the old reference copy's end.
static void stale_reference(void) {
    running = "an unlocked writer after a load";
    BYTE *region = VirtualAlloc(NULL, SCENE_BYTES + 0x800, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    BYTE *old_scene = new_scene(region);
    FakeBuffer small = add_buffer(old_scene, 0x1000, 0x2000, 16, 0);
    add_buffer(old_scene, 0x800, 0x2100, 4096, 0);
    draw_view(UNLOCKED_SLOT, small.parameter);
    int changes = scene_changes;
    BindingInfo *info = binding_info(small.binding, FALSE);
    check(info && !memcmp(info->reference, small.shadow, 16), "the unlocked slot keeps the bytes of its first write");
    BYTE *scene = new_scene(region + 0x800);
    add_buffer(scene, 0x1000, 0x2000, 16, 0);
    FakeBuffer large = add_buffer(scene, 0x800, 0x2100, 4096, 0);
    check(large.binding == small.binding, "the new scene's larger buffer sits where the old smaller one was");
    draw_view(UNLOCKED_SLOT, large.parameter);
    info = binding_info(large.binding, FALSE);
    check(info && !memcmp(info->reference, large.shadow, 4096) && binding_info_count == 1, "after a load the reference copy is the new buffer's");
    check(scene_changes == changes + 1, "the load is logged once");
}

// Library memory the scan takes for a parameter would be copied from a bogus shadow address.
static void lookalike_parameter(void) {
    running = "the shader library scan";
    BYTE *scene = new_scene(VirtualAlloc(NULL, SCENE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)), *library = scene + LIBRARY_OFFSET;
    FakeBuffer first = add_buffer(scene, 0x1000, 0x2000, 16, 0), second = add_buffer(scene, 0x800, 0x2100, 4096, 0);
    // A float in the middle of a buffer: values sit 4 bytes apart, so only the shadow has to be a heap pointer.
    *(BYTE **)(library + 0x2200 + 0x18) = second.binding;
    *(BYTE **)(library + 0x2200 + 0x28) = second.shadow + 20;
    *(float *)first.shadow = 3.0f;
    *(float *)(second.shadow + 20) = 7.0f;
    BYTE *holder = calloc(1, 0x40), *device = calloc(1, 0x50);
    *(BYTE **)(holder + 0x30) = device;
    *(BYTE **)(device + 0x40) = (BYTE *)1;
    *(uint32_t *)(device + 0x48) = 16;
    *(BYTE **)(library + 0x3000 + 0x18) = holder;
    *(BYTE **)(library + 0x3000 + 0x28) = (BYTE *)5;
    frame_stamp = &stamp;
    check(prepare_contexts(), "the scan finds the library's parameters");
    check(contexts.record_count == 3 && contexts.buffer_count == 2, "three parameters in two buffers, the lookalike left out");
    BOOL shadows = TRUE;
    for (uint32_t b = 0; b < contexts.buffer_count; b++) shadows &= contexts.buffers[b].shadow == first.shadow || contexts.buffers[b].shadow == second.shadow;
    check(shadows, "every buffer's values are a real shadow");
    int a = library_buffer_index(*(BYTE **)(first.binding + 0x30)), b = library_buffer_index(*(BYTE **)(second.binding + 0x30));
    check(a >= 0 && b >= 0 && *(float *)(contexts.snapshot + contexts.buffers[a].data_offset) == 3.0f &&
              *(float *)(contexts.snapshot + contexts.buffers[b].data_offset + 20) == 7.0f,
          "the snapshot holds the shadows' values");
}

static BYTE borrows_flag;

static BYTE borrows(BYTE *object) {
    (void)object;
    return borrows_flag;
}

static BYTE answers_two(BYTE *object) {
    (void)object;
    return 2;
}

// As the pass execute calls it: rax the effect's vtable, rcx the effect.
static BYTE call_query_stub(void (*stub)(void), void **vtable, BYTE *object) {
    uint64_t result;
    __asm__ volatile("mov %%rsp, %%r12\n\tand $-16, %%rsp\n\tsub $32, %%rsp\n\tcall *%3\n\tmov %%r12, %%rsp"
                     : "=a"(result), "+c"(object)
                     : "0"(vtable), "r"(stub)
                     : "rdx", "r8", "r9", "r10", "r11", "r12", "memory", "cc");
    return (BYTE)result;
}

static BYTE *fake_streams(uint64_t offset, BYTE *holder, BYTE *instances) {
    BYTE *streams = calloc(1, STREAMS_SIZE), *entries = calloc(1, STREAM_ENTRY_SIZE);
    *(BYTE **)entries = holder;
    *(BYTE **)streams = entries;
    *(uint32_t *)(streams + 0x08) = 1;
    *(BYTE **)(streams + 0x10) = instances;
    *(uint64_t *)(streams + 0x18) = offset;
    return streams;
}

static BYTE *fake_ring(uint32_t vtable_rva) {
    BYTE *ring = calloc(1, 0x10);
    *(BYTE **)ring = engine + vtable_rva;
    return ring;
}

static BYTE *fake_index_stream(uint32_t start) {
    BYTE *index_stream = calloc(1, INDEX_STREAM_SIZE);
    *(uint32_t *)(index_stream + 0x8) = start;
    return index_stream;
}

// An effect that borrows draws with another item's streams, which on a worker were the engine's shared rings (s2fix
// issue 12): the private item carries the source's streams instead, and the engine's query then says no on workers.
static void borrowed_streams(void) {
    running = "a worker's item whose effect borrows streams";
    static void *borrowing_vtable[0xf8 / 8];
    borrowing_vtable[BORROW_STREAMS_SLOT / 8] = borrowing_vtable[BORROW_INDEX_SLOT / 8] = (void *)borrows;
    BYTE *effect = calloc(1, 0xa0), *source = calloc(1, 0x80), *chunker = calloc(1, CHUNKER_SIZE);
    BYTE *own_holder = fake_ring(GEOMETRY_RING_VTABLE_RVA), *their_holder = fake_ring(GEOMETRY_RING_VTABLE_RVA), *instances = fake_ring(INSTANCE_RING_VTABLE_RVA);
    BYTE *own_copy = calloc(1, 0x10), *their_copy = calloc(1, 0x10), *instances_copy = calloc(1, 0x10);
    BYTE *item = calloc(1, DRAW_ITEM_SIZE), *source_item = calloc(1, DRAW_ITEM_SIZE);
    *(void ***)effect = borrowing_vtable;
    *(BYTE **)(effect + BORROWED_STREAMS_SOURCE) = *(BYTE **)(effect + BORROWED_INDEX_SOURCE) = source;
    *(BYTE **)(source + SOURCE_ITEM) = source_item;
    *(BYTE **)chunker = engine + CHUNKER_VTABLE_RVA;
    *(BYTE **)(item + 0x10) = effect;
    *(BYTE **)(item + 0x18) = chunker;
    *(BYTE **)(item + 0x20) = fake_streams(1, own_holder, instances);
    *(BYTE **)(item + 0x28) = fake_index_stream(10);
    *(BYTE **)(source_item + 0x20) = fake_streams(2, their_holder, instances);
    *(BYTE **)(source_item + 0x28) = fake_index_stream(20);
    Worker *worker = calloc(1, sizeof *worker);
    worker->clones[0] = (Clone){.original = own_holder, .copy = own_copy};
    worker->clones[1] = (Clone){.original = their_holder, .copy = their_copy};
    worker->clones[2] = (Clone){.original = instances, .copy = instances_copy};
    worker->clone_count = 3;
    BYTE *group = item;
    Job job = {.object = (BYTE *)&group, .kind = JOB_BATCH};

    borrows_flag = 1;
    prepare_private_item(worker, &job);
    BYTE *streams = *(BYTE **)(worker->item + 0x20), *index_stream = *(BYTE **)(worker->item + 0x28);
    check(streams == worker->streams && *(uint64_t *)(streams + 0x18) == 2 && **(BYTE ***)streams == their_holder && *(BYTE **)(streams + 0x10) == instances_copy,
          "a borrowing item's private copy carries the source item's vertex streams: its vertices as they are, its instances in the worker's ring");
    check(index_stream == worker->index_stream && *(uint32_t *)(index_stream + 0x8) == 20, "and the source item's index stream");

    borrows_flag = 0;
    prepare_private_item(worker, &job);
    check(*(uint64_t *)(worker->streams + 0x18) == 1 && *(BYTE **)worker->stream_entries == own_copy && *(BYTE **)(worker->streams + 0x10) == instances_copy &&
              *(uint32_t *)(worker->index_stream + 0x8) == 10,
          "an item that doesn't borrow keeps its own streams, its rings the worker's");

    borrows_flag = 1;
    *(BYTE **)(source + SOURCE_ITEM) = NULL;
    prepare_private_item(worker, &job);
    check(*(uint64_t *)(worker->streams + 0x18) == 1 && *(uint32_t *)(worker->index_stream + 0x8) == 10, "a source with no draw item yet leaves its own, as the engine does");

    TlsSetValue(tls_index, worker);
    check(!call_query_stub(overhaul_borrow_streams_stub, borrowing_vtable, effect) && !call_query_stub(overhaul_borrow_index_stub, borrowing_vtable, effect),
          "on a worker both borrow queries answer no");
    TlsSetValue(tls_index, NULL);
    check(call_query_stub(overhaul_borrow_streams_stub, borrowing_vtable, effect) && call_query_stub(overhaul_borrow_index_stub, borrowing_vtable, effect),
          "elsewhere they are the effect's own answer");
    borrowing_vtable[BORROW_INDEX_SLOT / 8] = (void *)answers_two;
    check(call_query_stub(overhaul_borrow_streams_stub, borrowing_vtable, effect) == 1 && call_query_stub(overhaul_borrow_index_stub, borrowing_vtable, effect) == 2,
          "each stub asks its own slot");
}

int main(int argc, char **argv) {
    int only = argc > 1 ? atoi(argv[1]) : 0;
    AddVectoredExceptionHandler(1, fault);
    api = &test_api;
    tls_index = TlsAlloc();
    engine = VirtualAlloc(NULL, ENGINE_RVA + 8, MEM_RESERVE, PAGE_NOACCESS);
    VirtualAlloc(engine + ENGINE_RVA, 8, MEM_COMMIT, PAGE_READWRITE);
    *(BYTE **)(engine + ENGINE_RVA) = engine_object;
    WrappedClass *wrapped = wrapped_class(effect_vtable);
    wrapped->vtable = effect_vtable;
    for (int s = 0; s < WRITER_SLOTS; s++) wrapped->original[s] = test_writer;
    wrapped->writes[LOCKED_SLOT] = 1;
    wrapped_class_count = 1;
    if (!only || only == 1) stale_learned();
    if (!only || only == 2) stale_reference();
    if (!only || only == 3) lookalike_parameter();
    if (!only || only == 4) borrowed_streams();
    printf("test_overhaul: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
