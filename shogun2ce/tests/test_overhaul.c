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
    printf("test_overhaul: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
