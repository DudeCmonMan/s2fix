#include "s2module.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// fpslimit.dll against a stand-in engine: the ini's caps, whole numbers only, 0 or none off, the old fps_limit the
// default each new key overrides; the rendering it wraps gets its arguments and gives back its result; at a cap no tick
// ends before its slot, whatever its work; the campaign map's handler takes the campaign cap, any other the battle cap;
// a tick over its slot doesn't make the next ones hurry; nothing is held while the debug report runs; the caps changed
// live. A busy PC only delays the test's ticks, so each is checked against its slot (the time since the first, never
// short), with the average loose.
#define ENGINE_BYTES 0x1800000
#define RENDER_RVA 0xafaf0
#define RENDER_CALL_RVA 0xb7433
#define TICK_RVA (RENDER_CALL_RVA - 11)
#define APP_RVA 0x17c755c
#define CAMPAIGN_HANDLER_VTABLE_RVA 0x16068b0
#define FOLDER L"fpslimit_test"
#define INI L".\\" FOLDER L"\\s2prof.ini"
#define TICKS 60
// The first tick's own lateness (an interrupt as it ends) shifts the rest by as much; a limiter that doesn't hold is a
// whole slot early.
#define EARLY_MS 1.0

static BYTE *engine;
static volatile LONG fixes_on = 1, *battle, *campaign, report_on;
static int failures, checks;
static LONG renders, wrong_arguments;
static LONGLONG frequency;
static S2Api api;
static S2ModuleMain module_main;
static HMODULE module;
// The app's handler stack (count at +0x38, list at +0x3c): the menu's handler under the one on top.
static BYTE app[0x40];
static void *handlers[2];
static void *menu_handler, *top_handler;
static BYTE battle_vtable[4];

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

static DWORD __attribute__((thiscall)) stand_in_render(BYTE *self, DWORD a, DWORD b, DWORD c) {
    InterlockedIncrement(&renders);
    if (self != engine || a != 1 || b != 2 || c != 3) InterlockedIncrement(&wrong_arguments);
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

// The main tick as far as it matters here: push 3; push 2; push 1; mov ecx, engine; call the rendering; ret. The call is
// the engine's own bytes, which the module patches.
static void make_engine(void) {
    BYTE *tick = engine + TICK_RVA;
    const BYTE pushes[] = {0x6a, 0x03, 0x6a, 0x02, 0x6a, 0x01, 0xb9};
    memcpy(tick, pushes, sizeof pushes);
    *(BYTE **)(tick + 7) = engine;
    const BYTE call[] = {0xe8, 0xb8, 0x86, 0xff, 0xff, 0xc3};
    memcpy(engine + RENDER_CALL_RVA, call, sizeof call);
    BYTE *render = engine + RENDER_RVA;
    render[0] = 0xe9;
    *(LONG *)(render + 1) = (LONG)((BYTE *)(void *)stand_in_render - (render + 5));
    *(BYTE **)(engine + APP_RVA) = app;
    *(LONG *)(app + 0x38) = 2;
    *(void ***)(app + 0x3c) = handlers;
    menu_handler = battle_vtable + 1;
    handlers[0] = &menu_handler;
    handlers[1] = &top_handler;
}

// The handler on top: the campaign map's, a battle's or the menu's (only the menu's on the stack).
static void on_top(const char *mode) {
    if (!strcmp(mode, "menu")) *(LONG *)(app + 0x38) = 1;
    else {
        *(LONG *)(app + 0x38) = 2;
        top_handler = !strcmp(mode, "campaign") ? (void *)(engine + CAMPAIGN_HANDLER_VTABLE_RVA) : (void *)battle_vtable;
    }
}

static DWORD tick(void) {
    return ((DWORD(*)(void))(void *)(engine + TICK_RVA))();
}

// Each value NULL: the key is absent (all three: no ini at all). The engine's call is put back first, so each ini
// installs afresh.
static const char *install(const wchar_t *old, const wchar_t *battle_value, const wchar_t *campaign_value) {
    DeleteFileW(INI);
    if (old) WritePrivateProfileStringW(L"fps", L"fps_limit", old, INI);
    if (battle_value) WritePrivateProfileStringW(L"fps", L"battle_fps_limit", battle_value, INI);
    if (campaign_value) WritePrivateProfileStringW(L"fps", L"campaign_fps_limit", campaign_value, INI);
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

// Work below the slot, varied: no tick before its slot, the average near it.
static void expect_capped(const char *what, int count, double slot_ms) {
    static const int light[] = {0, 3000, 6000, 1500, 8000, 500};
    double ends[TICKS], early, mean;
    char text[200];
    ticks(light, 6, count, ends);
    slots(ends, count, slot_ms, &early, &mean);
    snprintf(text, sizeof text, "%s, work 0-8 ms: no tick before its slot (at most %.3f ms early), %.2f ms apart on average (%.0f)", what, early,
             mean, slot_ms);
    expect(text, early < EARLY_MS && mean < slot_ms * 1.05);
}

static void expect_uncapped(const char *what) {
    static const int none[] = {0};
    double ends[30];
    char text[160];
    ticks(none, 1, 30, ends);
    snprintf(text, sizeof text, "%s: 30 ticks without work in %.2f ms", what, ends[29]);
    expect(text, ends[29] < 5);
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
    battle = module ? (volatile LONG *)(void *)GetProcAddress(module, "battle_fps_limit") : NULL;
    campaign = module ? (volatile LONG *)(void *)GetProcAddress(module, "campaign_fps_limit") : NULL;
    if (!module_main || !battle || !campaign) {
        printf("modules\\fpslimit.dll not found\n");
        failures++;
        return finish();
    }

    expect("no ini: off", install(NULL, NULL, NULL) != NULL && *battle == 0 && *campaign == 0);
    expect("no values: off", install(L"", L"", L"") != NULL && *battle == 0 && *campaign == 0);
    expect("battle_fps_limit=0 and campaign_fps_limit=0: off", install(NULL, L"0", L"0") != NULL && *battle == 0 && *campaign == 0);
    static const wchar_t *const not_whole[] = {L"60.5", L"abc", L"-5", L"60x", L"+60", L"0x3c"};
    for (size_t i = 0; i < sizeof not_whole / sizeof *not_whole; i++) {
        char what[80];
        snprintf(what, sizeof what, "battle_fps_limit=%ls is not a whole number: off", not_whole[i]);
        expect(what, install(NULL, not_whole[i], NULL) != NULL && *battle == 0 && *campaign == 0);
    }
    expect("an old ini's fps_limit=100 caps both", install(L"100", NULL, NULL) == NULL && *battle == 100 && *campaign == 100);
    expect("battle_fps_limit=0 overrides fps_limit=100 in battle only", install(L"100", L"0", NULL) == NULL && *battle == 0 && *campaign == 100);
    expect("campaign_fps_limit=40 overrides fps_limit=100 on the campaign map only",
           install(L"100", NULL, L"40") == NULL && *battle == 100 && *campaign == 40);
    expect("campaign_fps_limit=abc: no cap on the campaign map, fps_limit=100 in battle",
           install(L"100", NULL, L"abc") == NULL && *battle == 100 && *campaign == 0);
    expect("fps_limit=abc with battle_fps_limit=30: 30 in battle, none on the campaign map",
           install(L"abc", L"30", NULL) == NULL && *battle == 30 && *campaign == 0);
    api.report_running = NULL;
    const char *older = install(NULL, L"100", L"50");
    api.report_running = report_running;
    expect("a host without report_running: refused", older && strstr(older, "older"));
    BOOL installed = install(NULL, L"100", L"50") == NULL && *battle == 100 && *campaign == 50;
    expect("battle_fps_limit=100 and campaign_fps_limit=50 install", installed);
    if (!installed) return finish();
    on_top("battle");
    LONG before = renders;
    DWORD result = tick();
    expect("the rendering runs once a tick with its arguments, and its result comes back",
           renders - before == 1 && !wrong_arguments && result == 0x5a5a);

    expect_capped("in battle at 100 a second", TICKS, 10);

    // A hitch, 25 ms of work: from its end, the next ticks keep their whole slots.
    static const int hitch[] = {25000, 0, 0, 0, 0};
    double after[5], early, mean;
    char what[160];
    ticks(hitch, 5, 5, after);
    slots(after, 5, 10, &early, &mean);
    snprintf(what, sizeof what, "after a 25 ms tick none of the next comes before its slot (at most %.3f ms early): no burst", early);
    expect(what, early < EARLY_MS);

    on_top("campaign");
    expect_capped("on the campaign map at 50 a second", 20, 20);
    on_top("menu");
    expect_capped("in the menu, the battle cap of 100 a second", 20, 10);

    // The debug report measures the game uncapped; the cap is back after it.
    on_top("battle");
    report_on = 1;
    expect_uncapped("while the debug report runs");
    report_on = 0;
    expect_capped("after it", 20, 10);

    // Changed live: the battle cap to 50, then both to none.
    *battle = 50;
    expect_capped("battle cap changed live to 50 a second", 20, 20);
    *battle = 0;
    expect_uncapped("battle cap changed live to 0");
    on_top("campaign");
    expect_capped("the campaign map's cap stays 50 a second", 20, 20);
    *campaign = 0;
    expect_uncapped("campaign cap changed live to 0");
    return finish();
}
