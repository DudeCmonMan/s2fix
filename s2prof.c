#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <errno.h>
#include <intrin.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include "s2module.h"
#include "sortfix.h"

#define CAPTURE_SECONDS 20
#define PROGRESS_SECONDS 5
#define AB_SEGMENT_SECONDS 2
#define HOTKEY_POLL_TICKS 50
#define OTHER_THREAD_TICK_INTERVAL 10
#define MAX_THREADS 512
#define MAX_MODULES 1024
#define MAX_DEPTH 64
#define INDEX_SIZE (1u << 20)
#define TOP_OTHER_THREADS 6
#define TOP_MODULES 5
#define TIMER_HIGH_RESOLUTION 0x2
#define THREAD_BASIC_INFORMATION_CLASS 0
#define THREAD_WIN32_START_ADDRESS_CLASS 9
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#define MAX_SWITCHES 64
#define HOTKEY_SWITCH "overhaul"
#define MAX_CRASH_REPORTS 8
#define CPP_EXCEPTION_CODE 0xe06d7363u
#define ENGINE_MODULE L"empire.retail.x64.dll"
// The frame tick stores each frame's timestamp at this global; counting its changes counts frames without hooking.
#define FRAME_STAMP_STORE_RVA 0x161fd1
#define FRAME_STAMP_RVA 0x2ead830
static const BYTE frame_stamp_store[] = {0x48, 0x89, 0x05, 0x58, 0xb8, 0xd4, 0x02};
static const char *const mode_names[2] = {"off", "on"};

typedef struct {
    LARGE_INTEGER kernel_time, user_time, create_time;
    ULONG wait_time;
    PVOID start_address;
    HANDLE process_id, thread_id;
    LONG priority, base_priority;
    ULONG context_switches, thread_state, wait_reason;
} SystemThread;

typedef struct {
    ULONG next_entry_offset, thread_count;
    BYTE skipped_before_pid[0x48];
    HANDLE process_id;
    BYTE skipped_before_threads[0xa8];
    SystemThread threads[];
} SystemProcess;

_Static_assert(sizeof(SystemThread) == 0x50, "SYSTEM_THREAD_INFORMATION layout");
_Static_assert(offsetof(SystemProcess, process_id) == 0x50, "SYSTEM_PROCESS_INFORMATION layout");
_Static_assert(offsetof(SystemProcess, threads) == 0x100, "SYSTEM_PROCESS_INFORMATION layout");

typedef struct {
    LONG exit_status;
    PVOID teb;
    HANDLE process_id, thread_id;
    ULONG_PTR affinity;
    LONG priority, base_priority;
} ThreadBasicInfo;

typedef HRESULT(WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR *);

typedef struct {
    const char *name;
    volatile LONG *flag;
    void (*report)(BOOL start);
} Switch;

typedef struct {
    DWORD id;
    HANDLE handle;
    BOOL is_main, full_rate;
    uintptr_t start_address, stack_base;
    char name[128];
    ULONG64 cycles_mark, cycles[2];
    ULONG switches_mark, switches[2];
    uint32_t samples[2];
} Thread;

typedef struct {
    uintptr_t base, end;
    char name[64];
} Module;

typedef struct {
    uint32_t thread, depth, count;
    size_t frames_at;
} StackEntry;

typedef struct {
    BOOL alternating;
    Switch *mode_switch;
    uint32_t ticks, frames[2], page_faults[2];
    double seconds[2], tsc_hz;
} CaptureStats;

static wchar_t output_dir[MAX_PATH], fixes_dir[MAX_PATH], config_path[MAX_PATH];
static HANDLE console_output;
static volatile LONG crash_reports;
static char run_id[32];
static int capture_number = 1;
static volatile ULONG64 *frame_stamp;
static const char *sortfix_error = "not installed yet";
static BOOL hotkey_was_down[256];
static Thread threads[MAX_THREADS];
static int thread_count;
static ULONG switch_readings[MAX_THREADS];
static Module modules[MAX_MODULES];
static int module_count;
static StackEntry *entries;
static size_t entry_count, entry_capacity;
static uintptr_t *frame_pool;
static size_t frame_pool_count, frame_pool_capacity;
static uint32_t *stack_index;
static Switch switches[MAX_SWITCHES] = {{"fixes", &fixes_enabled, NULL}};
static int switch_count = 1;

static void output_path(wchar_t *path, const char *name) {
    char file[MAX_PATH];
    snprintf(file, sizeof file, "%s_%s", run_id, name);
    wcscpy(path, output_dir);
    wcscat(path, L"\\");
    size_t used = wcslen(path);
    MultiByteToWideChar(CP_UTF8, 0, file, -1, path + used, MAX_PATH - used);
}

static void console_line(const char *line, int length) {
    DWORD written;
    if (console_output) WriteFile(console_output, line, length, &written, NULL);
}

static int format_line(char *line, size_t size, const char *format, va_list args) {
    int length = vsnprintf(line, size - 1, format, args);
    if (length > (int)size - 2) length = size - 2;
    line[length++] = '\n';
    return length;
}

static void log_line(const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    int length = format_line(line, sizeof line, format, args);
    va_end(args);
    wchar_t path[MAX_PATH];
    output_path(path, "s2prof.log");
    FILE *file = _wfopen(path, L"a");
    if (file) {
        fwrite(line, 1, length, file);
        fclose(file);
    } else {
        MessageBeep(MB_ICONHAND);
    }
    console_line(line, length);
}

static void find_frame_stamp(void) {
    if (frame_stamp) return;
    BYTE *engine = (BYTE *)GetModuleHandleW(ENGINE_MODULE);
    if (engine && !memcmp(engine + FRAME_STAMP_STORE_RVA, frame_stamp_store, sizeof frame_stamp_store)) {
        frame_stamp = (volatile ULONG64 *)(engine + FRAME_STAMP_RVA);
        return;
    }
    log_line("FPS counter disabled: %s", engine ? "engine code differs from the analysed build" : "empire.retail.x64.dll not loaded");
}

static int compare_modules(const void *a, const void *b) {
    uintptr_t x = ((const Module *)a)->base, y = ((const Module *)b)->base;
    return x < y ? -1 : x > y;
}

static BOOL collect_modules(void) {
    HMODULE handles[MAX_MODULES];
    DWORD needed;
    if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof handles, &needed)) {
        log_line("EnumProcessModules failed: %lu", GetLastError());
        return FALSE;
    }
    if (needed > sizeof handles) {
        log_line("more than %d modules loaded", MAX_MODULES);
        return FALSE;
    }
    module_count = needed / sizeof(HMODULE);
    for (int i = 0; i < module_count; i++) {
        MODULEINFO info;
        GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof info);
        modules[i].base = (uintptr_t)info.lpBaseOfDll;
        modules[i].end = modules[i].base + info.SizeOfImage;
        GetModuleBaseNameA(GetCurrentProcess(), handles[i], modules[i].name, sizeof modules[i].name);
    }
    qsort(modules, module_count, sizeof *modules, compare_modules);
    return TRUE;
}

static const Module *module_of(uintptr_t address) {
    int low = 0, high = module_count - 1;
    while (low <= high) {
        int middle = (low + high) / 2;
        if (address < modules[middle].base) high = middle - 1;
        else if (address >= modules[middle].end) low = middle + 1;
        else return &modules[middle];
    }
    return NULL;
}

static void format_address(char *out, size_t size, uintptr_t address) {
    const Module *module = module_of(address);
    if (module) snprintf(out, size, "%s+0x%llx", module->name, (unsigned long long)(address - module->base));
    else snprintf(out, size, "0x%llx", (unsigned long long)address);
}

static uintptr_t main_entry_point(void) {
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *headers = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    return (uintptr_t)(base + headers->OptionalHeader.AddressOfEntryPoint);
}

static void read_thread_name(Thread *thread) {
    GetThreadDescriptionFn get_description = (GetThreadDescriptionFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    PWSTR description = NULL;
    if (!get_description || FAILED(get_description(thread->handle, &description))) return;
    WideCharToMultiByte(CP_UTF8, 0, description, -1, thread->name, sizeof thread->name, NULL, NULL);
    LocalFree(description);
}

static BOOL collect_threads(void) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        log_line("CreateToolhelp32Snapshot failed: %lu", GetLastError());
        return FALSE;
    }
    DWORD process_id = GetCurrentProcessId(), sampler_id = GetCurrentThreadId();
    uintptr_t entry_point = main_entry_point();
    THREADENTRY32 entry = {.dwSize = sizeof entry};
    thread_count = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == sampler_id) continue;
        if (thread_count == MAX_THREADS) {
            log_line("more than %d threads", MAX_THREADS);
            CloseHandle(snapshot);
            return FALSE;
        }
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!handle) continue;
        Thread *thread = &threads[thread_count];
        memset(thread, 0, sizeof *thread);
        thread->id = entry.th32ThreadID;
        thread->handle = handle;
        ThreadBasicInfo basic;
        NTSTATUS status = NtQueryInformationThread(handle, (THREADINFOCLASS)THREAD_BASIC_INFORMATION_CLASS, &basic, sizeof basic, NULL);
        if (status < 0) {
            log_line("ThreadBasicInformation failed for %lu: 0x%08lx", thread->id, (unsigned long)status);
            CloseHandle(handle);
            continue;
        }
        thread->stack_base = (uintptr_t)((NT_TIB *)basic.teb)->StackBase;
        NtQueryInformationThread(handle, (THREADINFOCLASS)THREAD_WIN32_START_ADDRESS_CLASS, &thread->start_address, sizeof thread->start_address, NULL);
        thread->is_main = thread->start_address == entry_point;
        read_thread_name(thread);
        // Overhaul workers do main-thread work, so they are sampled at main's rate.
        thread->full_rate = thread->is_main || !strncmp(thread->name, "overhaul", 8);
        thread_count++;
    }
    CloseHandle(snapshot);
    return TRUE;
}

static BOOL read_context_switches(void) {
    ULONG size = 1 << 20;
    void *buffer = NULL;
    NTSTATUS status;
    do {
        free(buffer);
        size += 1 << 16;
        buffer = malloc(size);
        status = NtQuerySystemInformation(SystemProcessInformation, buffer, size, &size);
    } while (status == STATUS_INFO_LENGTH_MISMATCH);
    if (status < 0) {
        log_line("NtQuerySystemInformation failed: 0x%08lx", (unsigned long)status);
        free(buffer);
        return FALSE;
    }
    DWORD process_id = GetCurrentProcessId();
    for (SystemProcess *process = buffer;; process = (SystemProcess *)((BYTE *)process + process->next_entry_offset)) {
        if ((DWORD)(uintptr_t)process->process_id == process_id) {
            for (ULONG i = 0; i < process->thread_count; i++)
                for (int t = 0; t < thread_count; t++)
                    if (threads[t].id == (DWORD)(uintptr_t)process->threads[i].thread_id) switch_readings[t] = process->threads[i].context_switches;
            free(buffer);
            return TRUE;
        }
        if (!process->next_entry_offset) break;
    }
    free(buffer);
    log_line("own process missing from SystemProcessInformation");
    return FALSE;
}

static BOOL mark_counters(void) {
    if (!read_context_switches()) return FALSE;
    for (int i = 0; i < thread_count; i++) {
        QueryThreadCycleTime(threads[i].handle, &threads[i].cycles_mark);
        threads[i].switches_mark = switch_readings[i];
    }
    return TRUE;
}

static BOOL accumulate_counters(int mode) {
    if (!read_context_switches()) return FALSE;
    for (int i = 0; i < thread_count; i++) {
        Thread *thread = &threads[i];
        ULONG64 cycles = 0;
        QueryThreadCycleTime(thread->handle, &cycles);
        thread->cycles[mode] += cycles - thread->cycles_mark;
        thread->switches[mode] += switch_readings[i] - thread->switches_mark;
        thread->cycles_mark = cycles;
        thread->switches_mark = switch_readings[i];
    }
    return TRUE;
}

// Bounded by the thread's own stack: a bad unwind on a foreign stack must not read past it.
static int unwind_frames(CONTEXT context, uintptr_t stack_base, uintptr_t *frames) {
    int depth = 0;
    while (depth < MAX_DEPTH && context.Rip && context.Rsp < stack_base - 8) {
        frames[depth++] = context.Rip;
        if (!module_of(context.Rip)) break;
        DWORD64 image_base;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, NULL);
        if (!function) {
            context.Rip = *(DWORD64 *)context.Rsp;
            context.Rsp += 8;
            continue;
        }
        PVOID handler_data;
        DWORD64 establisher_frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher_frame, NULL);
    }
    return depth;
}

static int walk_stack(const Thread *thread, uintptr_t *frames) {
    CONTEXT context = {.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER};
    if (!GetThreadContext(thread->handle, &context)) return 0;
    return unwind_frames(context, thread->stack_base, frames);
}

// Error-severity NTSTATUS codes and MSVC C++ exceptions; informational ones (thread naming, debug prints) are skipped.
static BOOL is_reported(DWORD code) {
    return (code & 0xc0000000u) == 0xc0000000u || code == CPP_EXCEPTION_CODE;
}

#define DUMPED_BYTES 64

// The bytes a register points at, when that is committed readable memory: the objects the faulting code was using.
static void log_memory(const char *name, DWORD64 address) {
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery((void *)address, &region, sizeof region) || region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) ||
        address + DUMPED_BYTES > (DWORD64)region.BaseAddress + region.RegionSize)
        return;
    char text[DUMPED_BYTES * 3 + 1];
    for (int i = 0; i < DUMPED_BYTES; i++) snprintf(text + 3 * i, 4, "%02x ", ((const BYTE *)address)[i]);
    log_line("  [%s] %s", name, text);
}

// First chance, so the report is written before the game's own crash handling; a fault something later handles is
// reported too, which is why only the first few are.
static LONG CALLBACK report_crash(EXCEPTION_POINTERS *pointers) {
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    if (!is_reported(record->ExceptionCode) || InterlockedIncrement(&crash_reports) > MAX_CRASH_REPORTS) return EXCEPTION_CONTINUE_SEARCH;
    collect_modules();
    char where[128], name[128] = "";
    format_address(where, sizeof where, (uintptr_t)record->ExceptionAddress);
    Thread self = {.handle = GetCurrentThread()};
    read_thread_name(&self);
    snprintf(name, sizeof name, "%s", self.name);
    log_line("CRASH: exception 0x%08lx at %s on thread %lu \"%s\"", record->ExceptionCode, where, GetCurrentThreadId(), name);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        log_line("  %s address 0x%llx", record->ExceptionInformation[0] == 0 ? "reading" : record->ExceptionInformation[0] == 1 ? "writing" : "executing",
                 (unsigned long long)record->ExceptionInformation[1]);
    const CONTEXT *c = pointers->ContextRecord;
    log_line("  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp);
    log_line("  r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx rsp %llx", c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14,
             c->R15, c->Rsp);
    const char *names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9", "r12", "r13", "r14", "r15"};
    const DWORD64 values[] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->R8, c->R9, c->R12, c->R13, c->R14, c->R15};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++) log_memory(names[i], values[i]);
    ULONG_PTR stack_low, stack_high;
    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    uintptr_t frames[MAX_DEPTH];
    int depth = unwind_frames(*c, stack_high, frames);
    for (int i = 0; i < depth; i++) {
        char frame[128];
        format_address(frame, sizeof frame, frames[i]);
        log_line("  #%d %s", i, frame);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static uint64_t hash_stack(uint32_t key, const uintptr_t *frames, int depth) {
    uint64_t hash = 1469598103934665603ull ^ key;
    for (int i = 0; i < depth; i++) hash = (hash ^ frames[i]) * 1099511628211ull;
    return hash;
}

static BOOL append_stack(uint32_t key, const uintptr_t *frames, int depth) {
    if (entry_count == entry_capacity) {
        entry_capacity = entry_capacity ? entry_capacity * 2 : 4096;
        entries = realloc(entries, entry_capacity * sizeof *entries);
    }
    if (frame_pool_count + depth > frame_pool_capacity) {
        frame_pool_capacity = frame_pool_capacity ? frame_pool_capacity * 2 : 1 << 18;
        frame_pool = realloc(frame_pool, frame_pool_capacity * sizeof *frame_pool);
    }
    if (!entries || !frame_pool) {
        log_line("out of memory after %zu stacks", entry_count);
        return FALSE;
    }
    entries[entry_count++] = (StackEntry){key, depth, 1, frame_pool_count};
    memcpy(frame_pool + frame_pool_count, frames, depth * sizeof *frames);
    frame_pool_count += depth;
    return TRUE;
}

// Stacks are keyed by thread index and sort-fix mode, so one capture holds separate profiles for each mode.
static BOOL record_stack(uint32_t key, const uintptr_t *frames, int depth) {
    if (entry_count * 2 >= INDEX_SIZE) {
        log_line("stack table full at %zu unique stacks", entry_count);
        return FALSE;
    }
    for (uint32_t slot = hash_stack(key, frames, depth) & (INDEX_SIZE - 1);; slot = (slot + 1) & (INDEX_SIZE - 1)) {
        uint32_t stored = stack_index[slot];
        if (!stored) {
            if (!append_stack(key, frames, depth)) return FALSE;
            stack_index[slot] = entry_count;
            return TRUE;
        }
        StackEntry *entry = &entries[stored - 1];
        if (entry->thread == key && entry->depth == (uint32_t)depth && !memcmp(frame_pool + entry->frames_at, frames, depth * sizeof *frames)) {
            entry->count++;
            return TRUE;
        }
    }
}

// Thread cycle times count at the invariant TSC rate; GetThreadTimes only advances in 15.6 ms ticks.
static double cpu_percent(const Thread *thread, const CaptureStats *stats, int mode) {
    return stats->seconds[mode] > 0 ? 100.0 * thread->cycles[mode] / (stats->seconds[mode] * stats->tsc_hz) : 0;
}

static double frames_or_one(const CaptureStats *stats, int mode) {
    return stats->frames[mode] ? stats->frames[mode] : 1;
}

static double sort_ms(const SortStats *stats) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return 1000.0 * stats->ticks / frequency.QuadPart;
}

static void thread_label(char *out, size_t size, const Thread *thread) {
    if (thread->name[0]) snprintf(out, size, "%s", thread->name);
    else format_address(out, size, thread->start_address);
}

static BOOL write_threads_file(int number, const CaptureStats *stats) {
    char name[64];
    wchar_t path[MAX_PATH];
    snprintf(name, sizeof name, "s2prof_%d_threads.tsv", number);
    output_path(path, name);
    FILE *file = _wfopen(path, L"w");
    if (!file) {
        log_line("cannot write threads file: %d", errno);
        return FALSE;
    }
    fprintf(file, "# ticks=%u tsc_mhz=%.1f alternating=%d main_hz=1000 other_hz=%d\n", stats->ticks, stats->tsc_hz / 1e6, stats->alternating,
            1000 / OTHER_THREAD_TICK_INTERVAL);
    for (int mode = 0; mode < 2; mode++) {
        fprintf(file, "# mode_%s switch=%s seconds=%.3f frames=%u", mode_names[mode], stats->mode_switch->name, stats->seconds[mode], stats->frames[mode]);
        for (int caller = 0; caller < SORT_CALLERS; caller++) {
            const SortStats *sort = &sort_stats[mode][caller];
            fprintf(file, " sort%d_calls=%lld sort%d_items=%lld sort%d_max_items=%lld sort%d_ms=%.3f sort%d_presorted=%lld sort%d_descents=%lld sort%d_moved=%lld",
                    caller, sort->calls, caller, sort->items, caller, sort->max_items, caller, sort_ms(sort), caller, sort->presorted, caller,
                    sort->descents, caller, sort->moved);
        }
        fprintf(file, "\n");
    }
    fprintf(file, "tid\trole\tname\tstart\tcpu_pct_off\tcpu_pct_on\tcycles_off\tcycles_on\tswitches_off\tswitches_on\tsamples_off\tsamples_on\n");
    for (int i = 0; i < thread_count; i++) {
        const Thread *thread = &threads[i];
        char start[128];
        format_address(start, sizeof start, thread->start_address);
        fprintf(file, "%lu\t%s\t%s\t%s\t%.1f\t%.1f\t%llu\t%llu\t%lu\t%lu\t%u\t%u\n", thread->id,
                thread->is_main ? "main" : thread->full_rate ? "worker" : "other", thread->name,
                start, cpu_percent(thread, stats, 0), cpu_percent(thread, stats, 1), (unsigned long long)thread->cycles[0],
                (unsigned long long)thread->cycles[1], thread->switches[0], thread->switches[1], thread->samples[0], thread->samples[1]);
    }
    fclose(file);
    return TRUE;
}

static BOOL write_stacks_file(int number) {
    char name[64];
    wchar_t path[MAX_PATH];
    snprintf(name, sizeof name, "s2prof_%d_stacks.txt", number);
    output_path(path, name);
    FILE *file = _wfopen(path, L"w");
    if (!file) {
        log_line("cannot write stacks file: %d", errno);
        return FALSE;
    }
    for (size_t i = 0; i < entry_count; i++) {
        const StackEntry *entry = &entries[i];
        fprintf(file, "%lu@%s", threads[entry->thread % MAX_THREADS].id, mode_names[entry->thread / MAX_THREADS]);
        for (int frame = entry->depth - 1; frame >= 0; frame--) {
            char text[128];
            format_address(text, sizeof text, frame_pool[entry->frames_at + frame]);
            fprintf(file, ";%s", text);
        }
        fprintf(file, " %u\n", entry->count);
    }
    fclose(file);
    return TRUE;
}

static void log_row(const char *label, const CaptureStats *stats, const double values[2], const char *format) {
    char line[256], cell[64];
    int length = snprintf(line, sizeof line, "  %-26s", label);
    for (int mode = 0; mode < 2; mode++) {
        if (stats->seconds[mode] <= 0) continue;
        snprintf(cell, sizeof cell, format, values[mode]);
        length += snprintf(line + length, sizeof line - length, "%12s", cell);
    }
    log_line("%s", line);
}

static void log_sort_rows(const CaptureStats *stats) {
    for (int caller = 0; caller < SORT_CALLERS; caller++) {
        const SortStats *off = &sort_stats[0][caller], *on = &sort_stats[1][caller];
        if (!off->calls && !on->calls) continue;
        char label[64];
        double ms[2] = {sort_ms(off) / frames_or_one(stats, 0), sort_ms(on) / frames_or_one(stats, 1)};
        double calls[2] = {off->calls / frames_or_one(stats, 0), on->calls / frames_or_one(stats, 1)};
        double items[2] = {off->calls ? (double)off->items / off->calls : 0, on->calls ? (double)on->items / on->calls : 0};
        double max_items[2] = {(double)off->max_items, (double)on->max_items};
        double presorted[2] = {off->calls ? 100.0 * off->presorted / off->calls : 0, on->calls ? 100.0 * on->presorted / on->calls : 0};
        double descents[2] = {off->calls ? (double)off->descents / off->calls : 0, on->calls ? (double)on->descents / on->calls : 0};
        double moved[2] = {off->calls ? (double)off->moved / off->calls : 0, on->calls ? (double)on->moved / on->calls : 0};
        snprintf(label, sizeof label, "%s sort ms/frame", sort_caller_names[caller]);
        log_row(label, stats, ms, "%.3f");
        snprintf(label, sizeof label, "%s sorts/frame", sort_caller_names[caller]);
        log_row(label, stats, calls, "%.2f");
        snprintf(label, sizeof label, "%s items/sort", sort_caller_names[caller]);
        log_row(label, stats, items, "%.0f");
        snprintf(label, sizeof label, "%s max items", sort_caller_names[caller]);
        log_row(label, stats, max_items, "%.0f");
        snprintf(label, sizeof label, "%s already sorted", sort_caller_names[caller]);
        log_row(label, stats, presorted, "%.0f%%");
        snprintf(label, sizeof label, "%s out-of-order keys", sort_caller_names[caller]);
        log_row(label, stats, descents, "%.0f");
        snprintf(label, sizeof label, "%s items moved (fix)", sort_caller_names[caller]);
        log_row(label, stats, moved, "%.0f");
    }
}

static void log_main_thread_modules(const Switch *mode_switch, int mode) {
    static uint32_t counts[MAX_MODULES + 1];
    memset(counts, 0, sizeof counts);
    uint32_t total = 0;
    for (size_t i = 0; i < entry_count; i++) {
        const StackEntry *entry = &entries[i];
        if (entry->thread / MAX_THREADS != (uint32_t)mode || !threads[entry->thread % MAX_THREADS].is_main) continue;
        const Module *module = module_of(frame_pool[entry->frames_at]);
        counts[module ? module - modules : MAX_MODULES] += entry->count;
        total += entry->count;
    }
    if (!total) return;
    log_line("main thread time by module (%s %s):", mode_switch->name, mode_names[mode]);
    for (int shown = 0; shown < TOP_MODULES; shown++) {
        int best = 0;
        for (int m = 1; m <= MAX_MODULES; m++)
            if (counts[m] > counts[best]) best = m;
        if (!counts[best]) break;
        log_line("  %5.1f%%  %s", 100.0 * counts[best] / total, best == MAX_MODULES ? "(no module)" : modules[best].name);
        counts[best] = 0;
    }
}

static double total_cycles(const Thread *thread) {
    return (double)thread->cycles[0] + thread->cycles[1];
}

static void log_summary(const CaptureStats *stats) {
    char header[128];
    int length = snprintf(header, sizeof header, "  %-26s", stats->mode_switch->name);
    for (int mode = 0; mode < 2; mode++)
        if (stats->seconds[mode] > 0) length += snprintf(header + length, sizeof header - length, "%12s", mode == 1 ? "ON" : "OFF");
    log_line("%s", header);
    log_row("seconds", stats, stats->seconds, "%.1f");
    if (frame_stamp) {
        double fps[2], frame_ms[2];
        for (int mode = 0; mode < 2; mode++) {
            fps[mode] = stats->seconds[mode] > 0 ? stats->frames[mode] / stats->seconds[mode] : 0;
            frame_ms[mode] = 1000.0 * stats->seconds[mode] / frames_or_one(stats, mode);
        }
        log_row("fps", stats, fps, "%.1f");
        log_row("ms per frame", stats, frame_ms, "%.2f");
    }
    for (int i = 0; i < thread_count; i++) {
        if (!threads[i].is_main) continue;
        double busy[2] = {cpu_percent(&threads[i], stats, 0), cpu_percent(&threads[i], stats, 1)};
        double switches[2] = {threads[i].switches[0] / frames_or_one(stats, 0), threads[i].switches[1] / frames_or_one(stats, 1)};
        log_row("main thread % of a core", stats, busy, "%.0f%%");
        log_row("main context switches/frame", stats, switches, "%.0f");
    }
    double page_faults[2] = {stats->page_faults[0] / frames_or_one(stats, 0), stats->page_faults[1] / frames_or_one(stats, 1)};
    log_row("page faults/frame", stats, page_faults, "%.0f");
    if (stats->mode_switch == &switches[0]) log_sort_rows(stats);
    if (sortfix_allocation_failures) log_line("  sort fix fell back to the original %ld times (allocation failed)", sortfix_allocation_failures);
    int order[MAX_THREADS], others = 0;
    for (int i = 0; i < thread_count; i++)
        if (!threads[i].is_main) order[others++] = i;
    for (int i = 0; i < others; i++)
        for (int j = i + 1; j < others; j++)
            if (total_cycles(&threads[order[j]]) > total_cycles(&threads[order[i]])) {
                int swap = order[i];
                order[i] = order[j];
                order[j] = swap;
            }
    log_line("busiest other threads (%% of a core, %s off / on):", stats->mode_switch->name);
    for (int i = 0; i < others && i < TOP_OTHER_THREADS; i++) {
        char label[128];
        thread_label(label, sizeof label, &threads[order[i]]);
        log_line("  %5.1f%% / %5.1f%%  %s", cpu_percent(&threads[order[i]], stats, 0), cpu_percent(&threads[order[i]], stats, 1), label);
    }
    for (int mode = 0; mode < 2; mode++) log_main_thread_modules(stats->mode_switch, mode);
}

static void release_capture(void) {
    for (int i = 0; i < thread_count; i++) CloseHandle(threads[i].handle);
    thread_count = 0;
    free(entries);
    free(frame_pool);
    free(stack_index);
    entries = NULL;
    frame_pool = NULL;
    stack_index = NULL;
    entry_count = entry_capacity = frame_pool_count = frame_pool_capacity = 0;
}

static BOOL hotkey_pressed(int key) {
    BOOL down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(key) & 0x8000);
    BOOL pressed = down && !hotkey_was_down[key];
    hotkey_was_down[key] = down;
    return pressed;
}

static void set_apply_fixes(BOOL enabled) {
    if (sortfix_error) {
        log_line("Apply Fixes unavailable: the sort fix did not install: %s", sortfix_error);
        return;
    }
    fixes_enabled = enabled;
    log_line("Apply Fixes %s", fixes_enabled ? "ON" : "OFF");
}

static void toggle_apply_fixes(void) {
    set_apply_fixes(!fixes_enabled);
}

static void set_switch(Switch *target, BOOL enabled) {
    if (target == &switches[0]) {
        set_apply_fixes(enabled);
        return;
    }
    *target->flag = enabled;
    log_line("%s %s", target->name, enabled ? "ON" : "OFF");
}

static Switch *find_switch(const char *name) {
    for (int i = 0; i < switch_count; i++)
        if (!strcmp(switches[i].name, name)) return &switches[i];
    return NULL;
}

// A module loaded again registers its switches again: the new flag replaces the old one.
static void register_switch(const char *name, volatile LONG *flag, void (*report)(BOOL start)) {
    for (int i = 1; i < switch_count; i++)
        if (!strcmp(switches[i].name, name)) {
            switches[i] = (Switch){name, flag, report};
            return;
        }
    if (switch_count == MAX_SWITCHES) {
        log_line("switch %s not registered: all %d slots are used", name, MAX_SWITCHES);
        return;
    }
    switches[switch_count++] = (Switch){name, flag, report};
}

// A/B runs flip the switch directly; only the fix switch can be unavailable.
static const char *switch_unavailable(const Switch *target) {
    return target == &switches[0] ? sortfix_error : NULL;
}

static DWORD process_page_faults(void) {
    PROCESS_MEMORY_COUNTERS counters = {.cb = sizeof counters};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
    return counters.PageFaultCount;
}

static BOOL sample(HANDLE timer, CaptureStats *stats, int seconds) {
    LARGE_INTEGER frequency, started, segment_started, now;
    QueryPerformanceFrequency(&frequency);
    if (!mark_counters()) return FALSE;
    QueryPerformanceCounter(&started);
    segment_started = started;
    unsigned long long tsc_started = __rdtsc();
    ULONG64 last_stamp = frame_stamp ? *frame_stamp : 0;
    LONGLONG elapsed = 0, next_progress = PROGRESS_SECONDS * frequency.QuadPart, next_flip = AB_SEGMENT_SECONDS * frequency.QuadPart;
    uint32_t frames_at_progress = 0;
    DWORD page_faults_mark = process_page_faults();
    volatile LONG *flag = stats->mode_switch->flag;
    int mode = *flag != 0;
    uintptr_t frames[MAX_DEPTH];
    while (elapsed < seconds * frequency.QuadPart) {
        WaitForSingleObject(timer, INFINITE);
        if (stats->alternating && elapsed >= next_flip) {
            *flag = !*flag;
            next_flip += AB_SEGMENT_SECONDS * frequency.QuadPart;
        }
        if (!stats->alternating && stats->ticks % HOTKEY_POLL_TICKS == 0 && hotkey_pressed(VK_F10)) toggle_apply_fixes();
        int current = *flag != 0;
        if (current != mode) {
            QueryPerformanceCounter(&now);
            stats->seconds[mode] += (double)(now.QuadPart - segment_started.QuadPart) / frequency.QuadPart;
            DWORD page_faults = process_page_faults();
            stats->page_faults[mode] += page_faults - page_faults_mark;
            page_faults_mark = page_faults;
            if (!accumulate_counters(mode)) return FALSE;
            segment_started = now;
            mode = current;
        }
        if (frame_stamp && *frame_stamp != last_stamp) {
            last_stamp = *frame_stamp;
            stats->frames[mode]++;
        }
        for (int i = 0; i < thread_count; i++) {
            Thread *thread = &threads[i];
            if (!thread->full_rate && stats->ticks % OTHER_THREAD_TICK_INTERVAL) continue;
            if (SuspendThread(thread->handle) == (DWORD)-1) continue;
            int depth = walk_stack(thread, frames);
            ResumeThread(thread->handle);
            // Recording allocates, so it only happens after resuming: the suspended thread may hold the heap lock.
            if (!depth) continue;
            thread->samples[mode]++;
            if (!record_stack(i + mode * MAX_THREADS, frames, depth)) return FALSE;
        }
        stats->ticks++;
        QueryPerformanceCounter(&now);
        elapsed = now.QuadPart - started.QuadPart;
        if (elapsed >= next_progress) {
            uint32_t frames_now = stats->frames[0] + stats->frames[1];
            if (frame_stamp)
                log_line("  %2lld s  %5.1f fps  (%s %s)", next_progress / frequency.QuadPart,
                         (frames_now - frames_at_progress) / (double)PROGRESS_SECONDS, stats->mode_switch->name, *flag ? "ON" : "OFF");
            else
                log_line("  %2lld s", next_progress / frequency.QuadPart);
            frames_at_progress = frames_now;
            next_progress += PROGRESS_SECONDS * frequency.QuadPart;
        }
    }
    stats->seconds[mode] += (double)(now.QuadPart - segment_started.QuadPart) / frequency.QuadPart;
    stats->page_faults[mode] += process_page_faults() - page_faults_mark;
    stats->tsc_hz = (__rdtsc() - tsc_started) / ((double)elapsed / frequency.QuadPart);
    return accumulate_counters(mode);
}

// ab is the switch an A/B capture alternates, or NULL for a plain capture, whose profile splits by the fix switch.
static void capture(int seconds, Switch *ab) {
    int number = capture_number++;
    BOOL alternating = ab != NULL;
    Switch *mode_switch = ab ? ab : &switches[0];
    if (alternating && switch_unavailable(ab)) {
        log_line("A/B capture of %s unavailable: %s", ab->name, switch_unavailable(ab));
        return;
    }
    MessageBeep(MB_OK);
    LONG enabled_before = *mode_switch->flag;
    log_line("capture %d: sampling for %d s%s%s...", number, seconds, alternating ? ", switching every 2 s: " : "; Ctrl+Shift+F10 still toggles Apply Fixes",
             alternating ? ab->name : "");
    if (alternating && ab->report) ab->report(TRUE);
    find_frame_stamp();
    sortfix_reset_stats();
    stack_index = calloc(INDEX_SIZE, sizeof *stack_index);
    // A high-resolution timer rather than timeBeginPeriod, which would change the game's own timer behaviour.
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due = {.QuadPart = -10000};
    if (!stack_index || !timer || !SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE)) {
        log_line("capture %d: setup failed: %lu", number, GetLastError());
        if (timer) CloseHandle(timer);
        release_capture();
        MessageBeep(MB_ICONHAND);
        return;
    }
    CaptureStats stats = {.alternating = alternating, .mode_switch = mode_switch};
    BOOL ok = collect_modules() && collect_threads();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    ok = ok && sample(timer, &stats, seconds);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    CloseHandle(timer);
    if (alternating) *mode_switch->flag = enabled_before;
    ok = ok && write_threads_file(number, &stats) && write_stacks_file(number);
    if (ok) log_summary(&stats);
    if (ok && alternating && ab->report) ab->report(FALSE);
    log_line("capture %d: %s (%d threads, %zu unique stacks) -> %s_s2prof_%d_threads.tsv, %s_s2prof_%d_stacks.txt; %s now %s", number,
             ok ? "done" : "FAILED", thread_count, entry_count, run_id, number, run_id, number, mode_switch->name, *mode_switch->flag ? "ON" : "OFF");
    release_capture();
    MessageBeep(ok ? MB_ICONASTERISK : MB_ICONHAND);
}

static void log_help(void) {
    log_line("Hotkeys (in game): Ctrl+Shift+F9 A/B capture %d s (Apply Fixes alternates every %d s) | Ctrl+Shift+F10 toggle Apply Fixes | Ctrl+Shift+F8 plain capture",
             CAPTURE_SECONDS, AB_SEGMENT_SECONDS);
    if (find_switch(HOTKEY_SWITCH)) log_line("Ctrl+Shift+F7 toggles %s", HOTKEY_SWITCH);
}

static void start_run(void) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    snprintf(run_id, sizeof run_id, "%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    char folder[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, output_dir, -1, folder, sizeof folder, NULL, NULL);
    if (!CreateDirectoryW(output_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        log_line("cannot create %s (error %lu); captures will fail", folder, GetLastError());
    log_line("run %s: logs go to %s", run_id, folder);
}

static S2Api module_api = {.fix_enabled = &fixes_enabled,
                           .log_line = log_line,
                           .hook = hook_function,
                           .register_switch = register_switch};

static void load_module(const char *utf8_path) {
    wchar_t path[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, path, MAX_PATH);
    const char *name = strrchr(utf8_path, '\\') ? strrchr(utf8_path, '\\') + 1 : utf8_path;
    HMODULE module = LoadLibraryW(path);
    if (!module) {
        log_line("module %s: LoadLibrary failed: %lu", name, GetLastError());
        return;
    }
    S2ModuleMain module_main = (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main");
    if (!module_main) {
        log_line("module %s: no s2_module_main export", name);
        return;
    }
    module_api.engine = (BYTE *)GetModuleHandleW(ENGINE_MODULE);
    const char *error = module_main(&module_api);
    log_line("module %s: %s", name, error ? error : "installed");
}

static void load_modules(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*.dll", dir);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        log_line("no modules loaded from %ls (error %lu)", dir, GetLastError());
        return;
    }
    do {
        wchar_t path[MAX_PATH];
        char utf8_path[MAX_PATH * 3];
        swprintf(path, MAX_PATH, L"%ls\\%ls", dir, found.cFileName);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8_path, sizeof utf8_path, NULL, NULL);
        load_module(utf8_path);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

// Shows the log live. Closing a console ends its process, the game, so the window has no close
// button and ignores Ctrl+C. It opens while the proxy loads, before the game has a window to take focus from.
static DWORD open_console(void) {
    if (!GetPrivateProfileIntW(L"s2prof", L"console", 0, config_path)) return ERROR_SUCCESS;
    // ACCESS_DENIED means the process already has a console, such as when started from one; that one is used.
    if (!AllocConsole() && GetLastError() != ERROR_ACCESS_DENIED) return GetLastError();
    SetConsoleTitleW(L"s2prof");
    SetConsoleCtrlHandler(NULL, TRUE);
    DeleteMenu(GetSystemMenu(GetConsoleWindow(), FALSE), SC_CLOSE, MF_BYCOMMAND);
    console_output = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (console_output == INVALID_HANDLE_VALUE) {
        console_output = NULL;
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

static DWORD WINAPI controller(LPVOID unused) {
    (void)unused;
    DWORD console_error = open_console();
    start_run();
    AddVectoredExceptionHandler(1, report_crash);
    if (console_error) log_line("console unavailable: error %lu", console_error);
    sortfix_error = sortfix_install();
    log_line("s2prof running inside the game.");
    if (sortfix_error) log_line("sort fix unavailable: %s; Apply Fixes cannot be turned on", sortfix_error);
    load_modules(fixes_dir);
    set_apply_fixes(TRUE);
    log_help();
    Switch *hotkey_switch = find_switch(HOTKEY_SWITCH);
    for (;;) {
        Sleep(50);
        if (hotkey_pressed(VK_F10)) toggle_apply_fixes();
        if (hotkey_pressed(VK_F7) && hotkey_switch) set_switch(hotkey_switch, !*hotkey_switch->flag);
        if (hotkey_pressed(VK_F9)) capture(CAPTURE_SECONDS, &switches[0]);
        if (hotkey_pressed(VK_F8)) capture(CAPTURE_SECONDS, NULL);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    // Present only when the process ends through ExitProcess; a fail-fast or TerminateProcess skips it.
    if (reason == DLL_PROCESS_DETACH && reserved) log_line("process exiting through ExitProcess");
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    GetModuleFileNameW(instance, output_dir, MAX_PATH);
    wcscpy(fixes_dir, output_dir);
    wcscpy(wcsrchr(fixes_dir, L'\\'), L"\\fixes");
    wcscpy(config_path, output_dir);
    wcscpy(wcsrchr(config_path, L'\\'), L"\\s2prof.ini");
    wcscpy(wcsrchr(output_dir, L'\\'), L"\\perf-logs");
    HANDLE thread = CreateThread(NULL, 0, controller, NULL, 0, NULL);
    if (!thread) return FALSE;
    CloseHandle(thread);
    return TRUE;
}
