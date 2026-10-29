// The overhaul's own defines, which its includes below need.
#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <intrin.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

// Checks the overhaul's value copies and shader-library scan against a stand-in engine: SCENE with SHADERS inline, its
// constant buffers and parameters in the engine's layout at addresses each check picks, every allocation ending at a
// no-access page so a read past one faults where it happens. A load that replaces SCENE must leave no writer reading the
// old scene's buffers, and the scan must leave out memory that only looks like a parameter (s2fix issue 7's crashes, in
// the Complete Edition's overhaul, which this one was ported from).
// Usage: test_overhaul.exe [check number], all of them by default.
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
#define UNLOCKED_SLOT 1
#define LOCKED_SLOT 3
// What a freed scene's memory holds in the check: -0.5 as floats, a pointer above 2 GB, which this program can't map.
#define REUSED_BYTES 0xbf000000u
// Two of the engine's SHADER_PARAMETER classes: one keeping a typed copy of its value pointer at +0x24, one without.
#define TYPED_PARAMETER_RVA 0x174942c
#define PLAIN_PARAMETER_RVA 0x17494b8

typedef struct {
    BYTE *binding, *parameter, *shadow;
} FakeBuffer;

static int failures, checks, scene_changes;
static const char *running;
static BYTE engine_object[0x40];
static uint64_t stamp = 1;
static void *effect_vtable[0x5c / 4];
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
    printf("FAIL %s: access violation reading 0x%lx at %p\n", running, (unsigned long)exception->ExceptionRecord->ExceptionInformation[1],
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

// The engine's constructor, as far as the overhaul reads it: a cleared SCENE that g_engine points at.
static BYTE *new_scene(BYTE *at) {
    memset(at, 0, SCENE_BYTES);
    *(BYTE **)(engine_object + SCENE_OFFSET) = at;
    return at;
}

// A constant buffer at library offset `at` and its parameter at `parameter_at`, whose value is `value_offset` into the
// shadow; the device object and the shadow come from the heap.
static FakeBuffer add_buffer(BYTE *scene, uint32_t at, uint32_t parameter_at, uint32_t bytes, uint32_t value_offset) {
    BYTE *library = scene + LIBRARY_OFFSET, *device = calloc(1, 0x40);
    FakeBuffer fake = {library + at, library + parameter_at, calloc(1, bytes)};
    *(BYTE **)(device + 0x28) = fake.shadow;
    *(uint32_t *)(device + 0x2c) = bytes;
    *(BYTE **)(fake.binding + 0x1c) = device;
    *(BYTE **)fake.parameter = engine + TYPED_PARAMETER_RVA;
    *(BYTE **)(fake.parameter + 0xc) = fake.binding;
    *(BYTE **)(fake.parameter + 0x14) = *(BYTE **)(fake.parameter + 0x24) = fake.shadow + value_offset;
    return fake;
}

static void write_parameter(void) {
    **(float **)(written + 0x14) += 1.0f;
    touch_hook(written);
}

static BYTE __attribute__((thiscall)) single_writer(BYTE *object, void *view, uint32_t id, void *k) {
    (void)object, (void)view, (void)id, (void)k;
    write_parameter();
    return 1;
}

static BYTE __attribute__((thiscall)) material_writer(BYTE *object, void *view, void *k) {
    (void)object, (void)view, (void)k;
    write_parameter();
    return 1;
}

// One split view on main, as execute_hook runs it: the fork, one writer call in main's range, the join.
static void draw_view(int slot, BYTE *parameter) {
    static ViewSlot view;
    static const WriterArguments args;
    written = parameter;
    wrap_writers(&view);
    begin_values(&main_values);
    split_thread = GetCurrentThreadId();
    values_active = 1;
    call_writer(slot, (BYTE *)&effect, &args);
    values_active = 0;
    write_back_values(&main_values);
    main_values.drawing = FALSE;
}

// A slot that writes under the lock puts back the buffers it wrote before each call. After a load the old scene's
// memory holds other data; the Complete Edition's first crash read a buffer pointer from it (value_copy, from
// call_writer).
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
    for (uint32_t *q = (uint32_t *)old_scene; q < (uint32_t *)(old_scene + SCENE_BYTES); q++) *q = REUSED_BYTES;
    draw_view(LOCKED_SLOT, buffer.parameter);
    check(wrapped->learned[LOCKED_SLOT][0] == buffer.binding && !wrapped->learned[LOCKED_SLOT][1],
          "after a load the slot knows only the new scene's buffer");
    check(*(float *)buffer.shadow == 1.0f, "the new scene's buffer holds the writer's value");
    check(scene_changes == changes + 1, "the load is logged once");
}

// A slot that runs unlocked compares each buffer it wrote with the bytes of its first write. A new scene that lands
// over the old one can put a larger buffer where a smaller one was; the old reference copy is then read past its end.
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

// The scan of the library at a split view's fork, then the snapshot of every buffer's values. The Complete Edition's
// second crash copied from address 1: the shadow of library memory the scan took for a parameter.
static void lookalike_parameter(void) {
    running = "the shader library scan";
    BYTE *scene = new_scene(VirtualAlloc(NULL, SCENE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)), *library = scene + LIBRARY_OFFSET;
    FakeBuffer first = add_buffer(scene, 0x1000, 0x2000, 16, 0), second = add_buffer(scene, 0x800, 0x2100, 4096, 0);
    // A float in the middle of a buffer, in a class without the typed copy: only the shadow has to be a heap pointer.
    *(BYTE **)(library + 0x2200) = engine + PLAIN_PARAMETER_RVA;
    *(BYTE **)(library + 0x2200 + 0xc) = second.binding;
    *(BYTE **)(library + 0x2200 + 0x14) = second.shadow + 20;
    *(float *)first.shadow = 3.0f;
    *(float *)(second.shadow + 20) = 7.0f;
    BYTE *holder = calloc(1, 0x40), *device = calloc(1, 0x40);
    *(BYTE **)(holder + 0x1c) = device;
    *(BYTE **)(device + 0x28) = (BYTE *)1;
    *(uint32_t *)(device + 0x2c) = 16;
    *(BYTE **)(library + 0x3000) = engine + PLAIN_PARAMETER_RVA;
    *(BYTE **)(library + 0x3000 + 0xc) = holder;
    *(BYTE **)(library + 0x3000 + 0x14) = (BYTE *)5;
    frame_stamp = &stamp;
    check(prepare_contexts(), "the scan finds the library's parameters");
    check(contexts.record_count == 3 && contexts.buffer_count == 2, "three parameters in two buffers, the lookalike left out");
    BOOL shadows = TRUE;
    for (uint32_t b = 0; b < contexts.buffer_count; b++) shadows &= contexts.buffers[b].shadow == first.shadow || contexts.buffers[b].shadow == second.shadow;
    check(shadows, "every buffer's values are a real shadow");
    int a = library_buffer_index(*(BYTE **)(first.binding + 0x1c)), b = library_buffer_index(*(BYTE **)(second.binding + 0x1c));
    check(a >= 0 && b >= 0 && *(float *)(contexts.snapshot + contexts.buffers[a].data_offset) == 3.0f &&
              *(float *)(contexts.snapshot + contexts.buffers[b].data_offset + 20) == 7.0f,
          "the snapshot holds the shadows' values");
}

int main(int argc, char **argv) {
    int only = argc > 1 ? atoi(argv[1]) : 0;
    AddVectoredExceptionHandler(1, fault);
    api = &test_api;
    engine = VirtualAlloc(NULL, ENGINE_RVA + 4, MEM_RESERVE, PAGE_NOACCESS);
    VirtualAlloc(engine + ENGINE_RVA, 4, MEM_COMMIT, PAGE_READWRITE);
    *(BYTE **)(engine + ENGINE_RVA) = engine_object;
    WrappedClass *wrapped = wrapped_class(effect_vtable);
    wrapped->vtable = effect_vtable;
    wrapped->original[UNLOCKED_SLOT] = (void *)single_writer;
    wrapped->original[LOCKED_SLOT] = (void *)material_writer;
    wrapped->writes[LOCKED_SLOT] = 1;
    wrapped_class_count = 1;
    if (!only || only == 1) stale_learned();
    if (!only || only == 2) stale_reference();
    if (!only || only == 3) lookalike_parameter();
    printf("test_overhaul: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
