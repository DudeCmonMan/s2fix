#include "s2module.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// camplabels.dll against a stand-in engine: other factions' settlements never reach the unrest test, the player's reuse
// an answer for at most one period (the ini's rate, whole numbers only) and keep their checks spread over the frames,
// the single-label refresh always asks afresh, and the switches and other threads reach the engine's test on every call.
#define ENGINE_BYTES 0x1200000
#define TEST_RVA 0x8b9cd0
#define LAYOUT_RVA 0x112cad0
#define FOLDER L"camplabels_test"
#define INI L".\\" FOLDER L"\\s2prof.ini"
#define MAX_SETTLEMENTS 6000

typedef uintptr_t(__attribute__((thiscall)) *TestFn)(BYTE *settlement);

// The call bytes from the engine's file, not computed like the module's; each is followed by a ret here.
static const struct {
    DWORD rva;
    BYTE bytes[5];
} sites[] = {{0x112cc44, {0xe8, 0x87, 0xd0, 0x78, 0xff}}, {0x11240a6, {0xe8, 0x25, 0x8a, 0x00, 0x00}}};

static BYTE *engine;
BYTE *test_site, *refresh_site;
static volatile LONG fixes_on = 1, *labels_flag, *verify_flag;
static volatile DWORD main_thread;
static int failures, checks;
static LONG test_runs;
static BYTE answers[MAX_SETTLEMENTS];
static LARGE_INTEGER last_test[MAX_SETTLEMENTS];
static LONGLONG period, frequency;
static S2Api api;
static S2ModuleMain module_main;
static BYTE *const viewer = (BYTE *)0x7000, *const rival = (BYTE *)0x7400;

// The engine's call: ecx the settlement, edi the viewing faction, esi the settlement's owner.
uintptr_t ask_site(BYTE *settlement, BYTE *viewing, BYTE *owner);
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _ask_site\n"
        "_ask_site:\n"
        "  push edi\n"
        "  push esi\n"
        "  mov ecx, [esp + 12]\n"
        "  mov edi, [esp + 16]\n"
        "  mov esi, [esp + 20]\n"
        "  call dword ptr [_test_site]\n"
        "  pop esi\n"
        "  pop edi\n"
        "  ret\n"
        ".att_syntax\n");

// The single-label refresh's call: thiscall (labels; label, fade). Jumped to, so its arguments sit right above its own
// return address, as in the engine; the jump after it comes back here.
void refresh_call(BYTE *labels, BYTE *label, float fade);
void refresh_return(void);
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _refresh_call\n"
        "_refresh_call:\n"
        "  mov ecx, [esp + 4]\n"
        "  push dword ptr [esp + 12]\n"
        "  push dword ptr [esp + 12]\n"
        "  jmp dword ptr [_refresh_site]\n"
        ".globl _refresh_return\n"
        "_refresh_return:\n"
        "  ret\n"
        ".att_syntax\n");

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
static void register_switch(const char *name, volatile LONG *flag, void (*report)(BOOL)) {
    (void)report;
    if (!strcmp(name, "camplabels")) labels_flag = flag;
    if (!strcmp(name, "camplabelsverify")) verify_flag = flag;
}
static const char *check_engine(const S2Check *table, int count) {
    (void)table, (void)count;
    return NULL;
}

static void expect(const char *what, BOOL ok) {
    checks++;
    failures += !ok;
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
}

static BYTE *settlement_of(int index) {
    return (BYTE *)(uintptr_t)(0x100000 + index * 0x350);
}

static int index_of(const BYTE *settlement) {
    return (int)(((uintptr_t)settlement - 0x100000) / 0x350);
}

// The upper bytes are garbage, as an engine test that returns only al leaves them.
static uintptr_t __attribute__((thiscall)) stand_in_test(BYTE *settlement) {
    InterlockedIncrement(&test_runs);
    int index = index_of(settlement);
    QueryPerformanceCounter(&last_test[index]);
    return 0xabcdef00u | answers[index];
}

// The layout asks the test for the label's own settlement, as the engine's does.
static BYTE *layout_labels, *layout_label;
static float layout_fade;
static BYTE layout_answer;
static void __attribute__((thiscall)) stand_in_layout(BYTE *labels, BYTE *label, float fade) {
    layout_labels = labels, layout_label = label, layout_fade = fade;
    layout_answer = (BYTE)ask_site(label, viewer, viewer);
}

static LONGLONG now_ticks(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

static void spin_ms(int milliseconds) {
    LONGLONG until = now_ticks() + frequency * milliseconds / 1000;
    while (now_ticks() < until) YieldProcessor();
}

static BYTE own(int index) {
    return (BYTE)ask_site(settlement_of(index), viewer, viewer);
}

static BOOL leads_to_module(const BYTE *site, HMODULE module) {
    const BYTE *target = site + 5 + *(const LONG *)(site + 1);
    HMODULE owner = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(const void *)target, &owner);
    return site[0] == 0xe8 && owner == module;
}

// A second ask inside the period of a fresh test, whatever the scheduler does: a stall past the period, or the
// settlement's place in the period passing between the two asks (a fresh answer), means another try.
static BOOL ask_within_period(int index, BYTE new_answer, BYTE *answer, LONG *runs, int wait_ms) {
    for (int attempt = 0; attempt < 5; attempt++) {
        spin_ms(wait_ms);
        own(index);
        LONGLONG tested = last_test[index].QuadPart;
        answers[index] = new_answer;
        LONG before = test_runs;
        *answer = own(index);
        *runs = test_runs - before;
        if (now_ticks() - tested < period && *answer != new_answer) return TRUE;
        answers[index] = !new_answer;
    }
    return FALSE;
}

// Frames 1 ms apart after a still camera, when every settlement falls due at once: how many frames test any of them.
static int frames_testing_after_stall(int first, int count) {
    for (int i = first; i < first + count; i++) own(i);
    spin_ms(100);
    for (int i = first; i < first + count; i++) own(i);
    int frames = 0;
    LONGLONG began = now_ticks();
    while (now_ticks() - began < 2 * period) {
        spin_ms(1);
        LONG before = test_runs;
        for (int i = first; i < first + count; i++) own(i);
        frames += test_runs != before;
    }
    return frames;
}

// hz NULL: no ini at all. The calls are put back first, so each value installs afresh.
static const char *install(const wchar_t *hz) {
    if (hz) WritePrivateProfileStringW(L"camplabels", L"hz", hz, INI);
    else DeleteFileW(INI);
    for (size_t i = 0; i < sizeof sites / sizeof *sites; i++) memcpy(engine + sites[i].rva, sites[i].bytes, 5);
    labels_flag = verify_flag = NULL;
    return module_main(&api);
}

static DWORD WINAPI ask_other_thread(LPVOID unused) {
    (void)unused;
    return own(1);
}

static void jump_to(BYTE *at, const void *to) {
    at[0] = 0xe9;
    *(LONG *)(at + 1) = (LONG)((const BYTE *)to - (at + 5));
}

int main(void) {
    LARGE_INTEGER rate;
    QueryPerformanceFrequency(&rate);
    frequency = rate.QuadPart;
    period = frequency / 30;
    engine = VirtualAlloc(NULL, ENGINE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    jump_to(engine + TEST_RVA, (const void *)stand_in_test);
    jump_to(engine + LAYOUT_RVA, (const void *)stand_in_layout);
    for (size_t i = 0; i < sizeof sites / sizeof *sites; i++) memcpy(engine + sites[i].rva, sites[i].bytes, 5);
    test_site = engine + sites[0].rva;
    refresh_site = engine + sites[1].rva;
    test_site[5] = 0xc3;
    jump_to(refresh_site + 5, (const void *)refresh_return);
    main_thread = GetCurrentThreadId();

    CreateDirectoryW(FOLDER, NULL);
    CreateDirectoryW(FOLDER L"\\fixes", NULL);
    DeleteFileW(INI);
    CopyFileW(L"modules\\camplabels.dll", FOLDER L"\\fixes\\camplabels.dll", FALSE);
    api = (S2Api){.engine = engine, .fix_enabled = &fixes_on, .log_line = log_line, .patch = patch, .register_switch = register_switch,
                  .main_thread = &main_thread, .check_engine = check_engine};
    HMODULE module = LoadLibraryW(FOLDER L"\\fixes\\camplabels.dll");
    module_main = module ? (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main") : NULL;
    uint32_t *hz = module ? (uint32_t *)(void *)GetProcAddress(module, "labels_hz") : NULL;
    volatile LONG *skipped = module ? (volatile LONG *)(void *)GetProcAddress(module, "labels_skipped") : NULL;
    volatile LONG64 *stats = module ? (volatile LONG64 *)(void *)GetProcAddress(module, "label_stats") : NULL;
    if (!module_main || !hz || !skipped || !stats) return printf("modules\\camplabels.dll not found\n"), 1;

    expect("no ini: the default, 30", !install(NULL) && *hz == 30);
    expect("no value: the default, 30", !install(L"") && *hz == 30);
    static const wchar_t *const not_whole[] = {L"5.5", L"abc", L"-5", L"30x", L"+5", L"0x10"};
    for (size_t i = 0; i < sizeof not_whole / sizeof *not_whole; i++) {
        char what[64];
        snprintf(what, sizeof what, "hz=%ls is not a whole number: the default, 30", not_whole[i]);
        expect(what, !install(not_whole[i]) && *hz == 30);
    }
    expect("hz=1, the minimum, installs", !install(L"1") && *hz == 1);
    expect("hz=7", !install(L"7") && *hz == 7);

    // 0 turns off only the reuse: other factions' settlements are still skipped, the player's always asked.
    expect("hz=0 installs", !install(L"0") && *hz == 0);
    expect("hz=0: the test's call leads to the module, the refresh's call is the engine's",
           leads_to_module(test_site, module) && !memcmp(refresh_site, sites[1].bytes, 5));
    answers[10] = 1;
    LONG runs_now = test_runs, skipped_now = *skipped;
    BYTE rival_answer = (BYTE)ask_site(settlement_of(10), viewer, rival);
    expect("hz=0: another faction's settlement is skipped, answer 0", rival_answer == 0 && test_runs == runs_now && *skipped == skipped_now + 1);
    expect("hz=0: the player's own asks the engine every time", own(10) == 1 && own(10) == 1 && test_runs == runs_now + 2);

    // Faster than the clock: asks the engine each time, and no division by a zero period.
    expect("a huge hz installs", !install(L"99999999999"));
    runs_now = test_runs;
    answers[5990] = 1;
    BOOL huge = own(5990) == 1;
    spin_ms(1);
    huge = huge && own(5990) == 1;
    expect("and the engine answers every time", huge && test_runs == runs_now + 2);

    const char *error = install(L"30");
    expect("hz=30 installs", !error);
    if (error) return printf("%s\n", error), 1;
    expect("both calls lead to the module", leads_to_module(test_site, module) && leads_to_module(refresh_site, module));
    expect("both switches registered", labels_flag && verify_flag);

    answers[11] = 1;
    runs_now = test_runs, skipped_now = *skipped;
    expect("another faction's settlement is skipped, answer 0", (BYTE)ask_site(settlement_of(11), viewer, rival) == 0 && test_runs == runs_now &&
                                                                 *skipped == skipped_now + 1);

    // Past the phase a settlement starts with, a test starts a full period.
    answers[0] = 1;
    runs_now = test_runs;
    expect("first answer is the engine's", own(0) == 1 && test_runs == runs_now + 1);
    spin_ms(40);
    expect("a period later the engine is asked", own(0) == 1 && test_runs == runs_now + 2);
    BYTE reused;
    LONG reused_runs;
    BOOL fit = ask_within_period(0, 0, &reused, &reused_runs, 40);
    expect("inside the period the last answer is reused", fit && reused == 1 && reused_runs == 0);
    spin_ms(40);
    runs_now = test_runs;
    expect("a period later the new answer arrives", own(0) == 0 && test_runs == runs_now + 1);

    // The refresh asks afresh inside the period, and its answer is the one reused afterwards.
    BOOL refreshed = FALSE, kept = FALSE;
    for (int attempt = 0; attempt < 5 && !(refreshed && kept); attempt++) {
        answers[12] = 0;
        spin_ms(40);
        own(12);
        LONGLONG tested = last_test[12].QuadPart;
        answers[12] = 1;
        runs_now = test_runs;
        refresh_call((BYTE *)0x9000, settlement_of(12), -1.0f);
        refreshed = layout_answer == 1 && test_runs == runs_now + 1;
        answers[12] = 0;
        kept = own(12) == 1 && test_runs == runs_now + 1;
        if (now_ticks() - tested >= period) refreshed = kept = FALSE;
    }
    expect("the single-label refresh asks the engine inside the period", refreshed);
    expect("and the next layouts reuse the refresh's answer", kept);
    expect("the refresh passes the engine's arguments", layout_labels == (BYTE *)0x9000 && layout_label == settlement_of(12) && layout_fade == -1.0f);
    expect("refreshes counted", stats[2] > 0);

    // Whatever the scheduler does, the age limit holds and no settlement is tested more often than the rate.
    for (int i = 20; i < 36; i++) own(i);
    LONG before = test_runs;
    LONGLONG began = now_ticks();
    BOOL age_held = TRUE;
    while (now_ticks() - began < frequency * 3 / 10) {
        for (int i = 20; i < 36; i++) {
            LONGLONG asked_at = now_ticks();
            own(i);
            if (asked_at - last_test[i].QuadPart >= period) age_held = FALSE;
        }
    }
    LONGLONG elapsed = now_ticks() - began;
    LONG runs = test_runs - before;
    expect("no answer outlives its period", age_held);
    expect("each settlement at most 30 tests a second", runs <= 16 * (elapsed / period + 2));
    expect("and tested again as the time passes", runs >= 16);
    // Kept together, 16 settlements test in 2 or 3 frames in all; spread over the period, in about 24.
    int spread = 0;
    for (int attempt = 0; attempt < 3 && spread < 16; attempt++) spread = frames_testing_after_stall(60, 16);
    expect("settlements due in one frame spread over the frames again", spread >= 16);

    // Verify: the engine on every reuse, the answer as without verify, and a stale one counted.
    *verify_flag = 1;
    answers[2] = 1;
    LONG64 differed_before = stats[3];
    fit = ask_within_period(2, 0, &reused, &reused_runs, 40);
    expect("verify: the last answer returned, the engine asked, the difference counted", fit && reused == 1 && reused_runs == 1 && stats[3] > differed_before);
    *verify_flag = 0;

    // Each off route asks the engine every time, other factions' settlements included.
    spin_ms(40);
    own(3);
    answers[3] = answers[13] = 1;
    LONG off_before = test_runs;
    *labels_flag = 0;
    BOOL off_answers = own(3) == 1 && own(3) == 1 && (BYTE)ask_site(settlement_of(13), viewer, rival) == 1;
    expect("switch off: the engine on every call", off_answers && test_runs == off_before + 3);
    *labels_flag = 1;
    fixes_on = 0;
    off_answers = own(3) == 1 && own(3) == 1 && (BYTE)ask_site(settlement_of(13), viewer, rival) == 1;
    expect("Apply Fixes off: the engine on every call", off_answers && test_runs == off_before + 6);
    fixes_on = 1;
    answers[1] = 0;
    own(1);
    answers[1] = 1;
    HANDLE thread = CreateThread(NULL, 0, ask_other_thread, NULL, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    DWORD other_answer = 0;
    GetExitCodeThread(thread, &other_answer);
    CloseHandle(thread);
    expect("another thread: the engine's answer, not the main thread's last one", other_answer == 1 && test_runs == off_before + 8);

    // More settlements than the table holds: evictions cost tests, never another settlement's answer.
    BOOL own_answers = TRUE;
    for (int i = 100; i < MAX_SETTLEMENTS; i++) answers[i] = (BYTE)(i & 1);
    spin_ms(40);
    for (int i = 100; i < MAX_SETTLEMENTS; i++)
        if (own(i) != answers[i]) own_answers = FALSE;
    expect("a full table: every settlement gets its own answer", own_answers);

    // The minimum: an answer holds for a whole second.
    error = install(L"1");
    expect("hz=1 installs again", !error);
    if (error) return printf("%s\n", error), 1;
    period = frequency;
    answers[40] = 1;
    own(40);
    fit = ask_within_period(40, 0, &reused, &reused_runs, 1050);
    expect("hz=1: the last answer is reused inside its second", fit && reused == 1 && reused_runs == 0);
    spin_ms(1050);
    runs_now = test_runs;
    expect("hz=1: a second later the new answer arrives", own(40) == 0 && test_runs == runs_now + 1);

    FreeLibrary(module);
    DeleteFileW(FOLDER L"\\fixes\\camplabels.dll");
    RemoveDirectoryW(FOLDER L"\\fixes");
    DeleteFileW(INI);
    RemoveDirectoryW(FOLDER);
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
