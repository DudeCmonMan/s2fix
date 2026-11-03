#include "s2module.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// soundfilter.dll against a stand-in filter; given the game's mss32.dll, also against its real filter and call site.
#define FILTER_CALL_RVA 0x19972
#define FILTER_RVA 0x16bf0
#define PAGE 4096
#define BLOCK_PAGES 4
#define STATE_WORDS 12
#define FADE_WORD 7
#define NEXT_FADE_WORD 8
#define COEFFICIENT_WORD 4
#define NO_NEXT_FADE 0x7fff
#define FADE_OUT_NEXT 0x30000
#define RANDOM_RUNS 3000
#define FAULTED_PAST_THE_BLOCK 3

typedef int(__attribute__((regparm(3))) *FilterFn)(void *source, DWORD samples, DWORD unused, void *destination, const DWORD *state);

// The call's bytes from the DLL's file, not computed like the module's.
static const BYTE call_in_file[5] = {0xe8, 0x79, 0xd2, 0xff, 0xff};

static int failures, checks, log_lines;
static BYTE *block_end;
static S2Api api;
static struct {
    int calls;
    void *source, *destination;
    DWORD samples;
    const DWORD *state;
} stand_in_seen;

static void expect(const char *what, BOOL ok) {
    checks++;
    failures += !ok;
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
}

static void log_line(const char *format, ...) {
    log_lines++;
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

static const char *patch(BYTE *target, const BYTE *expected, const BYTE *replacement, size_t size) {
    if (memcmp(target, expected, size)) return "site differs";
    DWORD old;
    VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old);
    memcpy(target, replacement, size);
    VirtualProtect(target, size, old, &old);
    return NULL;
}

static int __attribute__((regparm(3))) stand_in_filter(void *source, DWORD samples, DWORD unused, void *destination, const DWORD *state) {
    (void)unused;
    stand_in_seen.calls++;
    stand_in_seen.source = source, stand_in_seen.samples = samples, stand_in_seen.destination = destination, stand_in_seen.state = state;
    return 0x55;
}

static LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    ExitProcess((BYTE *)e->ExceptionRecord->ExceptionInformation[1] == block_end ? FAULTED_PAST_THE_BLOCK : 1);
}

static short *new_block(void) {
    BYTE *block = VirtualAlloc(NULL, PAGE * (BLOCK_PAGES + 1), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(block + PAGE * BLOCK_PAGES, PAGE, PAGE_NOACCESS, &old);
    block_end = block + PAGE * BLOCK_PAGES;
    return (short *)block;
}

static void new_state(DWORD *state, DWORD fade, DWORD kind, DWORD next) {
    memset(state, 0, STATE_WORDS * sizeof *state);
    state[FADE_WORD] = fade | kind << 16;
    state[NEXT_FADE_WORD] = next;
    state[COEFFICIENT_WORD] = 0x1000;
}

static int run_unguarded(const char *miles_path, DWORD fade, DWORD kind) {
    HMODULE miles = LoadLibraryExA(miles_path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!miles) return 2;
    AddVectoredExceptionHandler(1, on_fault);
    short *samples = new_block();
    DWORD state[STATE_WORDS];
    new_state(state, fade, kind, NO_NEXT_FADE);
    ((FilterFn)(void *)((BYTE *)miles + FILTER_RVA))(samples, 0, 0, samples, state);
    return 0;
}

static DWORD faulting_run(const char *self, const char *miles_path, DWORD fade, DWORD kind) {
    char line[1024];
    snprintf(line, sizeof line, "\"%s\" \"%s\" --unguarded %lu %lu", self, miles_path, fade, kind);
    STARTUPINFOA startup = {.cb = sizeof startup};
    PROCESS_INFORMATION process;
    if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) return 0xffffffff;
    WaitForSingleObject(process.hThread, INFINITE);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0xffffffff;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

static void against_real_filter(HMODULE module, const char *self, const char *miles_path) {
    S2ModuleMain module_main = (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main");
    FilterFn guard = (FilterFn)(void *)GetProcAddress(module, "filter_guard");
    FilterFn *next = (FilterFn *)(void *)GetProcAddress(module, "filter");

    HMODULE miles = LoadLibraryExA(miles_path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!miles) {
        expect("the game's mss32.dll loads", FALSE);
        return;
    }
    BYTE *call = (BYTE *)miles + FILTER_CALL_RVA;
    expect("the filter's call is where the fix expects it", !memcmp(call, call_in_file, sizeof call_in_file));

    IMAGE_NT_HEADERS *headers = (IMAGE_NT_HEADERS *)((BYTE *)miles + ((IMAGE_DOS_HEADER *)miles)->e_lfanew);
    DWORD stamp = headers->FileHeader.TimeDateStamp, old;
    VirtualProtect(&headers->FileHeader.TimeDateStamp, sizeof stamp, PAGE_READWRITE, &old);
    headers->FileHeader.TimeDateStamp = stamp + 1;
    expect("another build of mss32.dll is refused and left unpatched", module_main(&api) && !memcmp(call, call_in_file, sizeof call_in_file));
    headers->FileHeader.TimeDateStamp = stamp;

    expect("the fix installs", !module_main(&api));
    expect("the call now reaches the guard", call[0] == 0xe8 && call + 5 + *(LONG *)(call + 1) == (BYTE *)guard);
    FilterFn real = (FilterFn)(void *)((BYTE *)miles + FILTER_RVA);
    expect("the guard's next step is the real filter", *next == real);

    static const DWORD fades[] = {1, 8, 15}, kinds[] = {1, 2, 3};
    short *samples = new_block();
    for (int f = 0; f < 3; f++) {
        for (int k = 0; k < 3; k++) {
            char what[96];
            snprintf(what, sizeof what, "no samples, fade %lu of kind %lu: without the guard it runs off the block", fades[f], kinds[k]);
            expect(what, faulting_run(self, miles_path, fades[f], kinds[k]) == FAULTED_PAST_THE_BLOCK);

            DWORD state[STATE_WORDS], state_before[STATE_WORDS];
            new_state(state, fades[f], kinds[k], NO_NEXT_FADE);
            memcpy(state_before, state, sizeof state);
            for (int i = 0; i < PAGE * BLOCK_PAGES / 2; i++) samples[i] = (short)(i * 37);
            int result = guard(samples, 0, 0, samples, state);
            BOOL untouched = !memcmp(state, state_before, sizeof state);
            for (int i = 0; i < PAGE * BLOCK_PAGES / 2; i++) untouched = untouched && samples[i] == (short)(i * 37);
            snprintf(what, sizeof what, "no samples, fade %lu of kind %lu: the guard returns 0 and touches nothing", fades[f], kinds[k]);
            expect(what, result == 0 && untouched);
        }
    }

    // The block ends at an unmapped page right after the samples, so reading or writing one too many faults.
    short *direct = (short *)block_end - 2048;
    static short original[2048], guarded[2048];
    srand(1);
    int differences = 0, runs = 0, no_sample_runs = 0;
    for (int run = 0; run < RANDOM_RUNS; run++) {
        DWORD count = rand() % 4 == 0 ? (DWORD)(rand() % 3) : 1 + (DWORD)(rand() % 2048);
        DWORD fade = rand() % 3 == 0 ? 0 : 1 + (DWORD)(rand() % 16);
        DWORD kind = 1 + (DWORD)(rand() % 3);
        if (!count && fade) continue;
        no_sample_runs += !count;
        DWORD state_a[STATE_WORDS], state_b[STATE_WORDS];
        new_state(state_a, fade, kind, rand() % 4 == 0 ? FADE_OUT_NEXT : NO_NEXT_FADE);
        for (int i = 0; i < 6; i++) state_a[i] = (DWORD)(int)(short)rand();
        memcpy(state_b, state_a, sizeof state_a);
        for (int i = 0; i < 2048; i++) original[i] = (short)(rand() * 3);
        short *start = direct + 2048 - count;

        memcpy(start, original, count * sizeof *start);
        int result_a = real(start, count, 0, start, state_a);
        memcpy(guarded, start, count * sizeof *start);

        memcpy(start, original, count * sizeof *start);
        int result_b = guard(start, count, 0, start, state_b);

        differences += result_a != result_b || memcmp(guarded, start, count * sizeof *start) || memcmp(state_a, state_b, sizeof state_a);
        runs++;
    }
    char what[96];
    snprintf(what, sizeof what, "%d other inputs, %d with no samples: the guard matches the real filter", runs, no_sample_runs);
    expect(what, runs > RANDOM_RUNS / 2 && no_sample_runs > 0 && !differences);
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[2], "--unguarded")) return run_unguarded(argv[1], strtoul(argv[3], NULL, 10), strtoul(argv[4], NULL, 10));

    HMODULE module = LoadLibraryW(L"modules\\soundfilter.dll");
    S2ModuleMain module_main = module ? (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main") : NULL;
    FilterFn guard = module ? (FilterFn)(void *)GetProcAddress(module, "filter_guard") : NULL;
    FilterFn *next = module ? (FilterFn *)(void *)GetProcAddress(module, "filter") : NULL;
    if (!module_main || !guard || !next) return printf("modules\\soundfilter.dll not found\n"), 1;

    api.log_line = log_line;
    api.patch = patch;
    expect("without mss32.dll the fix declines", module_main(&api) != NULL);

    *next = stand_in_filter;
    short sample[4] = {0};
    DWORD state[STATE_WORDS];
    static const struct {
        const char *what;
        DWORD samples, fade, kind;
        BOOL reaches_filter;
    } cases[] = {
        {"samples, no fade: goes to the filter", 64, 0, 1, TRUE},
        {"samples, fading: goes to the filter", 64, 8, 1, TRUE},
        {"one sample, fading: goes to the filter", 1, 1, 3, TRUE},
        {"no samples, no fade: goes to the filter", 0, 0, 1, TRUE},
        {"no samples, kind 3 finished: goes to the filter", 0, 0, 3, TRUE},
        {"no samples, fading: skipped", 0, 8, 1, FALSE},
        {"no samples, one fade sample left: skipped", 0, 1, 1, FALSE},
        {"no samples, fading out: skipped", 0, 15, 3, FALSE},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        new_state(state, cases[i].fade, cases[i].kind, NO_NEXT_FADE);
        memset(&stand_in_seen, 0, sizeof stand_in_seen);
        int result = guard(sample, cases[i].samples, 0, sample + 2, state);
        if (cases[i].reaches_filter)
            expect(cases[i].what, stand_in_seen.calls == 1 && result == 0x55 && stand_in_seen.source == sample && stand_in_seen.samples == cases[i].samples &&
                                      stand_in_seen.destination == sample + 2 && stand_in_seen.state == state);
        else
            expect(cases[i].what, stand_in_seen.calls == 0 && result == 0);
    }
    volatile LONG *skipped = (volatile LONG *)(void *)GetProcAddress(module, "blocks_skipped");
    expect("every skip is counted, the first one logged", skipped && *skipped == 3 && log_lines == 1);

    if (argc > 1) {
        char self[MAX_PATH];
        GetModuleFileNameA(NULL, self, sizeof self);
        against_real_filter(module, self, argv[1]);
    } else {
        printf("no mss32.dll given: the checks against the real filter are skipped\n");
    }
    printf("test_soundfilter: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
