#include "s2module.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// campthrottle.dll against a stand-in engine: the ini's rate, whole numbers only; no answer is older than one period,
// and the switches and other threads reach the engine's check on every ask.
#define ENGINE_BYTES 0x900000
#define BUILD_CHECK_RVA 0x7818d0
#define FOLDER L"campthrottle_test"
#define INI L".\\" FOLDER L"\\s2prof.ini"
#define MAX_CONTEXTS 6000
#define SPREAD_CONTEXTS 64

typedef uintptr_t(__attribute__((thiscall)) *CheckFn)(BYTE *context);

// The call bytes from the engine's file, not computed like the module's.
static const struct {
    DWORD rva;
    BYTE bytes[5];
} sites[] = {{0x89fcc1, {0xe8, 0x0a, 0x1c, 0xee, 0xff}}, {0x89fd66, {0xe8, 0x65, 0x1b, 0xee, 0xff}}};

static BYTE *engine;
static volatile LONG fixes_on = 1, *throttle_flag, *verify_flag;
static volatile DWORD main_thread;
static int failures, checks;
static LONG check_runs;
static BYTE answers[MAX_CONTEXTS];
static LARGE_INTEGER last_check[MAX_CONTEXTS];
static LONGLONG period;
static S2Api api;
static S2ModuleMain module_main;

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
    if (!strcmp(name, "campthrottle")) throttle_flag = flag;
    if (!strcmp(name, "campthrottleverify")) verify_flag = flag;
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

static BYTE *context_of(int index) {
    return (BYTE *)(uintptr_t)(0x100000 + index * 0x230);
}

static int index_of(const BYTE *context) {
    return (int)(((uintptr_t)context - 0x100000) / 0x230);
}

// The upper bytes are garbage, as an engine check that returns only al leaves them.
static uintptr_t __attribute__((thiscall)) stand_in_check(BYTE *context) {
    InterlockedIncrement(&check_runs);
    int index = index_of(context);
    QueryPerformanceCounter(&last_check[index]);
    return 0xabcdef00u | answers[index];
}

static LONGLONG now_ticks(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

static void spin_ms(int milliseconds, LONGLONG frequency) {
    LONGLONG until = now_ticks() + frequency * milliseconds / 1000;
    while (now_ticks() < until) YieldProcessor();
}

static CheckFn site_target(int site) {
    BYTE *at = engine + sites[site].rva;
    return (CheckFn)(void *)(at + 5 + *(LONG *)(at + 1));
}

// A second ask inside the period of a fresh check, whatever the scheduler does: a stall past the period means another try.
static BOOL ask_within_period(CheckFn ask, int index, BYTE new_answer, BYTE *answer, LONG *runs, LONGLONG frequency, int wait_ms) {
    for (int attempt = 0; attempt < 5; attempt++) {
        spin_ms(wait_ms, frequency);
        ask(context_of(index));
        LONGLONG checked = last_check[index].QuadPart;
        answers[index] = new_answer;
        LONG before = check_runs;
        *answer = (BYTE)ask(context_of(index));
        *runs = check_runs - before;
        if (now_ticks() - checked < period) return TRUE;
        answers[index] = !new_answer;
    }
    return FALSE;
}

// hz NULL: no ini at all. The calls are put back first, so each value installs afresh.
static const char *install(const wchar_t *hz) {
    if (hz) WritePrivateProfileStringW(L"campthrottle", L"hz", hz, INI);
    else DeleteFileW(INI);
    for (size_t i = 0; i < sizeof sites / sizeof *sites; i++) memcpy(engine + sites[i].rva, sites[i].bytes, 5);
    throttle_flag = verify_flag = NULL;
    return module_main(&api);
}

static DWORD WINAPI ask_other_thread(LPVOID check) {
    return (BYTE)((CheckFn)check)(context_of(1));
}

int main(void) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    period = frequency.QuadPart / 30;
    engine = VirtualAlloc(NULL, ENGINE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    BYTE *check_entry = engine + BUILD_CHECK_RVA;
    check_entry[0] = 0xe9;
    *(LONG *)(check_entry + 1) = (LONG)((BYTE *)(void *)stand_in_check - (check_entry + 5));
    for (size_t i = 0; i < sizeof sites / sizeof *sites; i++) memcpy(engine + sites[i].rva, sites[i].bytes, 5);
    main_thread = GetCurrentThreadId();

    CreateDirectoryW(FOLDER, NULL);
    CreateDirectoryW(FOLDER L"\\fixes", NULL);
    DeleteFileW(INI);
    CopyFileW(L"modules\\campthrottle.dll", FOLDER L"\\fixes\\campthrottle.dll", FALSE);
    api = (S2Api){.engine = engine, .fix_enabled = &fixes_on, .log_line = log_line, .patch = patch, .register_switch = register_switch,
                  .main_thread = &main_thread, .check_engine = check_engine};
    HMODULE module = LoadLibraryW(FOLDER L"\\fixes\\campthrottle.dll");
    module_main = module ? (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main") : NULL;
    uint32_t *hz = module ? (uint32_t *)(void *)GetProcAddress(module, "throttle_hz") : NULL;
    if (!module_main || !hz) return printf("modules\\campthrottle.dll not found\n"), 1;

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
    const char *error = install(L"0");
    expect("hz=0: off", error && !strncmp(error, "off (", 5));
    expect("off: the engine's calls untouched", !memcmp(engine + sites[0].rva, sites[0].bytes, 5) && !memcmp(engine + sites[1].rva, sites[1].bytes, 5));
    expect("off: no switches", !throttle_flag && !verify_flag);

    // Faster than the clock: asks the engine each time, and no division by a zero period.
    error = install(L"99999999999");
    expect("a huge hz installs", !error);
    if (error) return printf("%s\n", error), 1;
    LONG huge_before = check_runs;
    answers[5990] = 1;
    BOOL huge_answer = (BYTE)site_target(0)(context_of(5990)) == 1;
    spin_ms(1, frequency.QuadPart);
    huge_answer = huge_answer && (BYTE)site_target(0)(context_of(5990)) == 1;
    expect("and the engine answers every time", huge_answer && check_runs == huge_before + 2);

    error = install(L"30");
    expect("hz=30 installs", !error);
    if (error) return printf("%s\n", error), 1;
    CheckFn ask = site_target(0);
    expect("both calls lead to the module", ask == site_target(1) && (BYTE *)(void *)ask != check_entry);
    expect("both switches registered", throttle_flag && verify_flag);

    // Past the phase a slot starts with, a check starts a full period.
    answers[0] = 1;
    LONG runs_now = check_runs;
    expect("first answer is the engine's", (BYTE)ask(context_of(0)) == 1 && check_runs == runs_now + 1);
    spin_ms(40, frequency.QuadPart);
    expect("a period later the engine is asked", (BYTE)ask(context_of(0)) == 1 && check_runs == runs_now + 2);
    BYTE reused;
    LONG reused_runs;
    BOOL fit = ask_within_period(ask, 0, 0, &reused, &reused_runs, frequency.QuadPart, 40);
    expect("inside the period the last answer is reused", fit && reused == 1 && reused_runs == 0);
    spin_ms(40, frequency.QuadPart);
    runs_now = check_runs;
    expect("a period later the new answer arrives", (BYTE)ask(context_of(0)) == 0 && check_runs == runs_now + 1);

    // Whatever the scheduler does, the age limit holds and no slot is checked more often than the rate.
    for (int i = 0; i < 16; i++) ask(context_of(i));
    LONG before = check_runs;
    LONGLONG began = now_ticks();
    BOOL age_held = TRUE;
    while (now_ticks() - began < frequency.QuadPart * 3 / 10) {
        for (int i = 0; i < 16; i++) {
            LONGLONG asked_at = now_ticks();
            ask(context_of(i));
            if (asked_at - last_check[i].QuadPart >= period) age_held = FALSE;
        }
    }
    LONGLONG elapsed = now_ticks() - began;
    LONG runs = check_runs - before;
    expect("no answer outlives its period", age_held);
    expect("each slot at most 30 checks a second", runs <= 16 * (elapsed / period + 2));
    expect("and checked again as the time passes", runs >= 16);

    // With 8 ms frames, a third of the slots a frame; not all of them on one. A stall can line them up, so three tries.
    BOOL spread = FALSE;
    for (int attempt = 0; attempt < 3 && !spread; attempt++) {
        LONG most = 0;
        for (int i = 100; i < 100 + SPREAD_CONTEXTS; i++) ask(context_of(i));
        for (int frame = 0; frame < 12; frame++) {
            spin_ms(8, frequency.QuadPart);
            LONG at_start = check_runs;
            for (int i = 100; i < 100 + SPREAD_CONTEXTS; i++) ask(context_of(i));
            if (check_runs - at_start > most) most = check_runs - at_start;
        }
        spread = most <= SPREAD_CONTEXTS * 3 / 4;
    }
    expect("the checks spread over the frames", spread);

    // Verify: the engine on every ask, the answer as without verify, and a stale one counted.
    volatile LONG64 *stats = (volatile LONG64 *)(void *)GetProcAddress(module, "throttle_stats");
    expect("stats exported", stats != NULL);
    *verify_flag = 1;
    answers[2] = 1;
    LONG64 differed_before = stats ? stats[2] : 0;
    fit = ask_within_period(ask, 2, 0, &reused, &reused_runs, frequency.QuadPart, 40);
    expect("verify: the last answer returned, the engine asked, the difference counted", fit && reused == 1 && reused_runs == 1 && stats && stats[2] > differed_before);
    *verify_flag = 0;

    // Each off route asks the engine every time.
    spin_ms(40, frequency.QuadPart);
    ask(context_of(3));
    LONG off_before = check_runs;
    *throttle_flag = 0;
    ask(context_of(3));
    ask(context_of(3));
    expect("switch off: the engine on every ask", check_runs == off_before + 2);
    *throttle_flag = 1;
    fixes_on = 0;
    ask(context_of(3));
    ask(context_of(3));
    expect("Apply Fixes off: the engine on every ask", check_runs == off_before + 4);
    fixes_on = 1;
    answers[1] = 1;
    HANDLE thread = CreateThread(NULL, 0, ask_other_thread, (LPVOID)(void *)ask, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    DWORD other_answer = 0;
    GetExitCodeThread(thread, &other_answer);
    CloseHandle(thread);
    expect("another thread: the engine's answer", other_answer == 1 && check_runs == off_before + 5);

    // More slots than the table holds: evictions cost checks, never another slot's answer.
    BOOL own_answers = TRUE;
    for (int i = 0; i < MAX_CONTEXTS; i++) answers[i] = (BYTE)(i & 1);
    spin_ms(40, frequency.QuadPart);
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if ((BYTE)ask(context_of(i)) != answers[i]) own_answers = FALSE;
    expect("a full table: every slot gets its own answer", own_answers);

    // The minimum: an answer holds for a whole second.
    error = install(L"1");
    expect("hz=1 installs again", !error);
    if (error) return printf("%s\n", error), 1;
    period = frequency.QuadPart;
    answers[20] = 1;
    ask = site_target(0);
    ask(context_of(20));
    fit = ask_within_period(ask, 20, 0, &reused, &reused_runs, frequency.QuadPart, 1050);
    expect("hz=1: the last answer is reused inside its second", fit && reused == 1 && reused_runs == 0);
    spin_ms(1050, frequency.QuadPart);
    runs_now = check_runs;
    expect("hz=1: a second later the new answer arrives", (BYTE)ask(context_of(20)) == 0 && check_runs == runs_now + 1);

    FreeLibrary(module);
    DeleteFileW(FOLDER L"\\fixes\\campthrottle.dll");
    RemoveDirectoryW(FOLDER L"\\fixes");
    DeleteFileW(INI);
    RemoveDirectoryW(FOLDER);
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
