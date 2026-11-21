#include "s2module.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// fpslimit.dll against a stand-in engine: the ini's cap, whole numbers only, 0 or none off; the present it wraps gets
// its object and gives back its result; at a cap no tick ends before its slot, whatever its work; a tick over its
// slot doesn't make the next ones hurry; nothing is held while the debug report runs; the cap changed live. A busy PC
// only delays the test's ticks, so each is checked against its slot (the time since the first, never short), with the
// average loose.
#define ENGINE_BYTES 0x200000
#define PRESENT_CALL_RVA 0x162d4b
#define PRESENT_SLOT 0x5a0
#define TICK_RVA (PRESENT_CALL_RVA - 17)
#define FOLDER L"fpslimit_test"
#define INI L".\\" FOLDER L"\\s2prof.ini"
#define TICKS 60
// The first tick's own lateness (an interrupt as it ends) shifts the rest by as much; a limiter that doesn't hold is a
// whole slot early.
#define EARLY_MS 1.0

static BYTE *engine;
static void *vtable[PRESENT_SLOT / 8 + 1];
static void *engine_object = vtable;
static volatile LONG fixes_on = 1, *limit, report_on;
static int failures, checks;
static LONG presents, wrong_objects;
static LONGLONG frequency;
static S2Api api;
static S2ModuleMain module_main;
static HMODULE module;

static void log_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

static const char *patch(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size) {
    if (memcmp(target, expected, size)) return "site differs";
    memcpy(target, replacement, size);
    return NULL;
}

static const char *check_engine(const S2Check *table, int count) {
    (void)table, (void)count;
    return NULL;
}

static BOOL report_running(void) {
    return report_on != 0;
}

static void expect(const char *what, BOOL ok) {
    checks++;
    failures += !ok;
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
}

static uint64_t stand_in_present(void *self) {
    InterlockedIncrement(&presents);
    if (self != &engine_object) InterlockedIncrement(&wrong_objects);
    return 0x5a5a;
}

static LONGLONG now_ticks(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

static void spin_us(int microseconds) {
    LONGLONG until = now_ticks() + frequency * microseconds / 1000000;
    while (now_ticks() < until) YieldProcessor();
}

static double ms(LONGLONG ticks) {
    return 1000.0 * (double)ticks / (double)frequency;
}

// The engine's image as far as the module reads it (e_lfanew, SizeOfImage), and the main tick as far as it matters:
// sub rsp, 0x28; mov rcx, the engine object; mov rax, [rcx]; call [rax+0x5a0]; add rsp, 0x28; ret. The call is the
// engine's own bytes, which the module patches.
static void make_engine(void) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)engine;
    dos->e_lfanew = 0x40;
    ((IMAGE_NT_HEADERS64 *)(engine + 0x40))->OptionalHeader.SizeOfImage = ENGINE_BYTES;
    BYTE *tick = engine + TICK_RVA;
    const BYTE head[] = {0x48, 0x83, 0xec, 0x28, 0x48, 0xb9};
    memcpy(tick, head, sizeof head);
    void *object = &engine_object;
    memcpy(tick + sizeof head, &object, sizeof object);
    const BYTE load_vtable[] = {0x48, 0x8b, 0x01};
    memcpy(tick + sizeof head + sizeof object, load_vtable, sizeof load_vtable);
    const BYTE call[] = {0xff, 0x90, 0xa0, 0x05, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    memcpy(engine + PRESENT_CALL_RVA, call, sizeof call);
    vtable[PRESENT_SLOT / 8] = (void *)stand_in_present;
}

static uint64_t tick(void) {
    return ((uint64_t(*)(void))(void *)(engine + TICK_RVA))();
}

// value NULL: no ini at all. The engine's call is put back first, so each value installs afresh.
static const char *install(const wchar_t *value) {
    if (value) WritePrivateProfileStringW(L"fps", L"fps_limit", value, INI);
    else DeleteFileW(INI);
    make_engine();
    return module_main(&api);
}

// Ticks of the given work each (microseconds, cycling); the time of each tick's end after the first's, in ms.
static void ticks(const int *work, int kinds, int count, double *ends) {
    LONGLONG first = 0;
    for (int i = 0; i < count; i++) {
        spin_us(work[i % kinds]);
        tick();
        LONGLONG now = now_ticks();
        if (!i) first = now;
        ends[i] = ms(now - first);
    }
}

// The most a tick came before its slot (k slots after the first), in ms, and the average gap.
static void slots(const double *ends, int count, double slot_ms, double *early, double *mean) {
    *early = 0;
    for (int k = 1; k < count; k++)
        if (k * slot_ms - ends[k] > *early) *early = k * slot_ms - ends[k];
    *mean = ends[count - 1] / (count - 1);
}

static int finish(void) {
    if (module) FreeLibrary(module);
    DeleteFileW(FOLDER L"\\fixes\\fpslimit.dll");
    RemoveDirectoryW(FOLDER L"\\fixes");
    DeleteFileW(INI);
    RemoveDirectoryW(FOLDER);
    printf("test_fpslimit: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}

int main(void) {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&counter);
    frequency = counter.QuadPart;
    engine = VirtualAlloc(NULL, ENGINE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    CreateDirectoryW(FOLDER, NULL);
    CreateDirectoryW(FOLDER L"\\fixes", NULL);
    DeleteFileW(INI);
    CopyFileW(L"modules\\fpslimit.dll", FOLDER L"\\fixes\\fpslimit.dll", FALSE);
    api = (S2Api){.engine = engine, .fix_enabled = &fixes_on, .log_line = log_line, .patch = patch, .check_engine = check_engine,
                  .report_running = report_running};
    module = LoadLibraryW(FOLDER L"\\fixes\\fpslimit.dll");
    module_main = module ? (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main") : NULL;
    limit = module ? (volatile LONG *)(void *)GetProcAddress(module, "fps_limit") : NULL;
    if (!module_main || !limit) {
        printf("modules\\fpslimit.dll not found\n");
        failures++;
        return finish();
    }

    expect("no ini: off", install(NULL) != NULL && *limit == 0);
    expect("no value: off", install(L"") != NULL && *limit == 0);
    expect("fps_limit=0: off", install(L"0") != NULL && *limit == 0);
    static const wchar_t *const not_whole[] = {L"60.5", L"abc", L"-5", L"60x", L"+60", L"0x3c"};
    for (size_t i = 0; i < sizeof not_whole / sizeof *not_whole; i++) {
        char what[64];
        snprintf(what, sizeof what, "fps_limit=%ls is not a whole number: off", not_whole[i]);
        expect(what, install(not_whole[i]) != NULL && *limit == 0);
    }
    api.report_running = NULL;
    const char *older = install(L"100");
    api.report_running = report_running;
    expect("a host without report_running: refused", older && strstr(older, "older"));
    BOOL installed = install(L"100") == NULL && *limit == 100;
    expect("fps_limit=100 installs", installed);
    if (!installed) return finish();
    LONG before = presents;
    uint64_t result = tick();
    expect("the present runs once a tick on its object, and its result comes back",
           presents - before == 1 && !wrong_objects && result == 0x5a5a);

    // Work below the 10 ms slot, varied.
    static const int light[] = {0, 3000, 6000, 1500, 8000, 500};
    double ends[TICKS], early, mean;
    char what[160];
    ticks(light, 6, TICKS, ends);
    slots(ends, TICKS, 10, &early, &mean);
    snprintf(what, sizeof what, "at 100 a second, work 0-8 ms: no tick before its slot (at most %.3f ms early), %.2f ms apart on average (10)",
             early, mean);
    expect(what, early < EARLY_MS && mean < 10.5);

    // A hitch, 25 ms of work: from its end, the next ticks keep their whole slots.
    static const int hitch[] = {25000, 0, 0, 0, 0};
    double after[5];
    ticks(hitch, 5, 5, after);
    slots(after, 5, 10, &early, &mean);
    snprintf(what, sizeof what, "after a 25 ms tick none of the next comes before its slot (at most %.3f ms early): no burst", early);
    expect(what, early < EARLY_MS);

    // The debug report measures the game uncapped; the cap is back after it.
    static const int none[] = {0};
    report_on = 1;
    ticks(none, 1, 30, ends);
    report_on = 0;
    snprintf(what, sizeof what, "while the debug report runs: 30 ticks without work in %.2f ms", ends[29]);
    expect(what, ends[29] < 5);
    ticks(light, 6, 20, ends);
    slots(ends, 20, 10, &early, &mean);
    snprintf(what, sizeof what, "after it: no tick before its slot again (at most %.3f ms early), %.2f ms apart on average (10)", early, mean);
    expect(what, early < EARLY_MS && mean < 10.5);

    // Changed live: 50 a second, then none.
    *limit = 50;
    ticks(light, 6, 20, ends);
    slots(ends, 20, 20, &early, &mean);
    snprintf(what, sizeof what, "changed live to 50 a second: no tick before its slot (at most %.3f ms early), %.2f ms apart on average (20)",
             early, mean);
    expect(what, early < EARLY_MS && mean < 21);
    *limit = 0;
    ticks(none, 1, 30, ends);
    snprintf(what, sizeof what, "changed live to 0: 30 ticks without work in %.2f ms", ends[29]);
    expect(what, ends[29] < 5);
    return finish();
}
