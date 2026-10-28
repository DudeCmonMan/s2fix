#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <imm.h>
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
#include "hook.h"
#include "wrap.h"
#include "checks/s2prof.h"
#include "report.h"

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
#define MAX_FIXES 64
#define MAX_FIX_NAME 32
// The render overhaul is one of the fixes: its switch follows Apply Fixes.
#define OVERHAUL_SWITCH "overhaul"
#define MAX_CRASH_REPORTS 8
#define CPP_EXCEPTION_CODE 0xe06d7363u
#define ENGINE_MODULE L"empire.retail.x64.dll"
// The frame tick stores each frame's timestamp at this global; counting its changes counts frames without hooking.
#define FRAME_STAMP_STORE_RVA 0x161fd1
#define FRAME_STAMP_RVA 0x2ead830
static const BYTE frame_stamp_store[] = {0x48, 0x89, 0x05, 0x58, 0xb8, 0xd4, 0x02};
// D3D11_RENDER_DEVICE::present, which runs on the game's main thread.
#define PRESENT_RVA 0x1ae7240
static const BYTE present_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x60};
// D3D11_RENDER_DEVICE::create_device (vtable +0xd0): D3D11CreateDevice through the global below with flags 1
// (D3D11_CREATE_DEVICE_SINGLETHREADED); the ID3D11Device goes to +0x180 of g_render_device.
#define CREATE_DEVICE_RVA 0x1ac7e80
static const BYTE create_device_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56};
// D3D11CreateDevice, which the device's constructor looks up in d3d11.dll.
#define D3D11_CREATE_DEVICE_SLOT_RVA 0x345cac0
#define RENDER_DEVICE_RVA 0x3450230
#define DEVICE_INTERFACE 0x180
// The fixes install in well under a second; past this the device is created without them.
#define HOLD_WAIT_MS 30000
#define WAIT_STEP_MS 100
// The targeted build: its timestamp and image size. Another mod's copy keeps the timestamp and these sections, and may
// add sections after them.
#define ENGINE_TIMESTAMP 0x6ac7c293u
#define ENGINE_IMAGE_SIZE 0x3744000u
#define BUILD_WARNING L"s2fix is not running with the targeted Shogun 2 build, not all fixes may work!"
#define LATE_MESSAGE                                                                                                      \
    L"s2fix was loaded after the game set up its graphics, so some of its fixes are off this session. Keep s2fix's "  \
    L"d3d11.dll in the game folder so it loads with the game."
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
typedef uint64_t (*CreateDeviceFn)(BYTE *device, uint64_t adapter);
typedef enum { ENGINE_NOT_LOADED, ENGINE_TARGETED, ENGINE_MODIFIED, ENGINE_OTHER } EngineBuild;
static EngineBuild engine_build;
// The render device global and vtable are where the targeted build has them, so the debug report may read the device.
static BOOL device_readable;
static const char *const engine_build_names[] = {"not loaded", "the targeted build", "the targeted build, modified by another mod",
                                                 "a different build"};
static const struct {
    const char *name;
    DWORD address, size;
} targeted_sections[] = {{".text", 0x1000, 0x2158adc},     {".rdata", 0x215a000, 0xce314a}, {".data", 0x2e3e000, 0x71fed0},
                         {".pdata", 0x355e000, 0x18c378}, {"_RDATA", 0x36eb000, 0x2404},   {".fptable", 0x36ee000, 0x100}};
typedef HRESULT(WINAPI *D3D11CreateDeviceFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
                                             D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
#define DLL_LOADED 1
typedef struct {
    ULONG flags;
    const UNICODE_STRING *full_name, *base_name;
    void *base;
    ULONG size;
} DllLoaded;
typedef void(CALLBACK *DllNotificationFn)(ULONG reason, const DllLoaded *loaded, void *context);
NTSTATUS NTAPI LdrRegisterDllNotification(ULONG flags, DllNotificationFn callback, void *context, void **cookie);


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
typedef enum { FIX_INSTALLED, FIX_OFF, FIX_REFUSED } FixState;

typedef struct {
    char name[MAX_FIX_NAME], module[MAX_PATH];
    FixState state;
    // From the fixes folder or the host's own sort fix, so in the closing line.
    BOOL counted;
    int first_switch;
} FixRecord;

static FixRecord fix_records[MAX_FIXES];
static int fix_count, open_fix;
static BOOL loading_fixes;
static char loading_module[MAX_PATH];
static BYTE *engine;
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
    if (stats->mode_switch == &switches[0] || stats->mode_switch->flag == &few_keys_enabled) log_sort_rows(stats);
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

static Switch *find_switch(const char *name);

static void sync_overhaul(void) {
    Switch *overhaul = find_switch(OVERHAUL_SWITCH);
    if (overhaul) *overhaul->flag = fixes_enabled;
}

static void set_apply_fixes(BOOL enabled) {
    fixes_enabled = enabled;
    sync_overhaul();
    log_line("Apply Fixes %s", fixes_enabled ? "ON" : "OFF");
}

static void toggle_apply_fixes(void) {
    set_apply_fixes(!fixes_enabled);
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
            if (flag == &fixes_enabled) sync_overhaul();
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
static BOOL capture_quiet;

static void capture(int seconds, Switch *ab) {
    int number = capture_number++;
    BOOL alternating = ab != NULL;
    Switch *mode_switch = ab ? ab : &switches[0];
    if (!capture_quiet) MessageBeep(MB_OK);
    LONG enabled_before = *mode_switch->flag;
    log_line("capture %d: sampling for %d s%s%s...", number, seconds, alternating ? ", switching every 2 s: " : "; Ctrl+Shift+F10 still toggles Apply Fixes",
             alternating ? ab->name : "");
    if (alternating && ab->report) ab->report(TRUE);
    find_frame_stamp();
    sort_mode_flag = mode_switch->flag;
    sortfix_reset_stats();
    stack_index = calloc(INDEX_SIZE, sizeof *stack_index);
    // A high-resolution timer rather than timeBeginPeriod, which would change the game's own timer behaviour.
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due = {.QuadPart = -10000};
    if (!stack_index || !timer || !SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE)) {
        log_line("capture %d: setup failed: %lu", number, GetLastError());
        if (timer) CloseHandle(timer);
        release_capture();
        if (!capture_quiet) MessageBeep(MB_ICONHAND);
        return;
    }
    CaptureStats stats = {.alternating = alternating, .mode_switch = mode_switch};
    BOOL ok = collect_modules() && collect_threads();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    ok = ok && sample(timer, &stats, seconds);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    CloseHandle(timer);
    if (alternating) *mode_switch->flag = enabled_before;
    sort_mode_flag = &fixes_enabled;
    if (mode_switch->flag == &fixes_enabled) sync_overhaul();
    ok = ok && write_threads_file(number, &stats) && write_stacks_file(number);
    if (ok) log_summary(&stats);
    if (ok && alternating && ab->report) ab->report(FALSE);
    log_line("capture %d: %s (%d threads, %zu unique stacks) -> %s_s2prof_%d_threads.tsv, %s_s2prof_%d_stacks.txt; %s now %s", number,
             ok ? "done" : "FAILED", thread_count, entry_count, run_id, number, run_id, number, mode_switch->name, *mode_switch->flag ? "ON" : "OFF");
    release_capture();
    if (!capture_quiet) MessageBeep(ok ? MB_ICONASTERISK : MB_ICONHAND);
}

// The debug report drives these: the switches, and the A/B capture without its own beeps (the report beeps once at its
// start and once at its end).

static int report_switch_count(void) {
    return switch_count;
}

static ReportSwitch report_switch_at(int index) {
    return (ReportSwitch){switches[index].name, switches[index].flag, switches[index].report};
}

static int report_capture(int seconds, int switch_index) {
    int number = capture_number;
    capture_quiet = TRUE;
    capture(seconds, &switches[switch_index]);
    capture_quiet = FALSE;
    return number;
}

static void start_report(void) {
    static BOOL initialised;
    if (!initialised) {
        report_init(&(ReportHost){.engine = engine,
                                  .engine_build = engine_build_names[engine_build],
                                  .device_readable = device_readable,
                                  .output_dir = output_dir,
                                  .config_path = config_path,
                                  .run_id = run_id,
                                  .version = S2FIX_VERSION,
                                  .log_line = log_line,
                                  .switch_count = report_switch_count,
                                  .switch_at = report_switch_at,
                                  .capture = report_capture,
                                  .output_path = output_path});
        initialised = TRUE;
    }
    report_start();
}

static void log_help(void) {
    log_line("Hotkeys (in game): Ctrl+Shift+F9 A/B capture %d s (Apply Fixes alternates every %d s) | Ctrl+Shift+F10 toggle Apply Fixes | Ctrl+Shift+F8 plain capture"
             " | Ctrl+Shift+F6 debug report (keep the camera still, about 3 minutes)",
             CAPTURE_SECONDS, AB_SEGMENT_SECONDS);
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

typedef uint64_t (*PresentFn)(BYTE *device);
static PresentFn original_present;
static CreateDeviceFn original_create_device;
static D3D11CreateDeviceFn real_d3d11_create_device;
static HANDLE engine_event;
static volatile LONG hold_ran;
static BOOL multithreaded_device, wrap_context;
static char device_hooks_error[192];
static volatile LONG build_warning_open;
static HHOOK button_hook;
static volatile DWORD main_thread_id;

static uint64_t present_hook(BYTE *device) {
    if (!main_thread_id) main_thread_id = GetCurrentThreadId();
    report_before_present();
    uint64_t result = original_present(device);
    report_after_present();
    return result;
}

// "off (...)": the player switched the fix off in s2prof.ini.
static BOOL switched_off(const char *error) {
    return !strncmp(error, "off (", 5);
}

static const char *end_fix(const char *error) {
    if (!open_fix) return error;
    FixRecord *fix = &fix_records[open_fix - 1];
    end_write_group();
    char undone[128] = "";
    if (error) {
        int hooks, patches;
        const char *undo_error = undo_write_group(open_fix, &hooks, &patches);
        if (switch_count > fix->first_switch) switch_count = fix->first_switch;
        if (undo_error) snprintf(undone, sizeof undone, "; undoing its writes failed: %s", undo_error);
        else if (hooks || patches) snprintf(undone, sizeof undone, " (undone: %d hooks, %d patches)", hooks, patches);
    }
    fix->state = !error ? FIX_INSTALLED : switched_off(error) ? FIX_OFF : FIX_REFUSED;
    size_t module_length = strcspn(fix->module, ".");
    if (strlen(fix->name) == module_length && !strncmp(fix->name, fix->module, module_length))
        log_line("module %s: %s%s", fix->module, error ? error : "installed", undone);
    else
        log_line("module %s: %s: %s%s", fix->module, fix->name, error ? error : "installed", undone);
    open_fix = 0;
    return error;
}

static void begin_fix(const char *name) {
    if (open_fix) end_fix(NULL);
    if (fix_count == MAX_FIXES) {
        log_line("fix %s: not tracked, all %d fix slots are used; it cannot be undone", name, MAX_FIXES);
        return;
    }
    FixRecord *fix = &fix_records[fix_count++];
    *fix = (FixRecord){.counted = loading_fixes, .first_switch = switch_count};
    snprintf(fix->name, sizeof fix->name, "%s", name);
    snprintf(fix->module, sizeof fix->module, "%s", loading_module[0] ? loading_module : name);
    open_fix = fix_count;
    begin_write_group(open_fix);
}

static const char *check_engine(const S2Check *checks, int count) {
    static char message[192];
    for (int i = 0; i < count; i++)
        if (!engine_hash_matches(engine, checks[i].rva, checks[i].size, checks[i].hash, checks[i].flags & S2_CHECK_ENTRY)) {
            snprintf(message, sizeof message, "the engine's %s (0x%llx) differs from the targeted build", checks[i].what,
                     (unsigned long long)(ENGINE_PREFERRED_BASE + checks[i].rva));
            return message;
        }
    return NULL;
}

static void log_fix_summary(ULONGLONG milliseconds) {
    int counts[3] = {0};
    char refused[512] = "";
    for (int i = 0; i < fix_count; i++) {
        if (!fix_records[i].counted) continue;
        counts[fix_records[i].state]++;
        if (fix_records[i].state == FIX_REFUSED && strlen(refused) + strlen(fix_records[i].name) + 3 < sizeof refused) {
            strcat(refused, counts[FIX_REFUSED] > 1 ? ", " : ": ");
            strcat(refused, fix_records[i].name);
        }
    }
    log_line("fixes: %d installed, %d off in s2prof.ini, %d refused%s (%llu ms)", counts[FIX_INSTALLED], counts[FIX_OFF], counts[FIX_REFUSED], refused,
             milliseconds);
}

// For the offline install check: the engine writes s2prof holds, and the fix (write group) each belongs to.
__declspec(dllexport) int s2prof_live_writes(LiveWrite *out, int capacity) {
    return list_live_writes(out, capacity);
}

__declspec(dllexport) const char *s2prof_fix(int group, int *state, BOOL *counted) {
    if (group < 1 || group > fix_count) return NULL;
    *state = fix_records[group - 1].state;
    *counted = fix_records[group - 1].counted;
    return fix_records[group - 1].name;
}

static S2Api module_api = {.fix_enabled = &fixes_enabled,
                           .log_line = log_line,
                           .hook = hook_function,
                           .register_switch = register_switch,
                           .patch = patch_checked,
                           .main_thread = &main_thread_id,
                           .set_hook_owner = set_hook_owner,
                           .end_fix = end_fix,
                           .begin_fix = begin_fix,
                           .check_engine = check_engine};

static void load_module(const char *utf8_path) {
    wchar_t path[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, path, MAX_PATH);
    const char *name = strrchr(utf8_path, '\\') ? strrchr(utf8_path, '\\') + 1 : utf8_path;
    char fix_name[MAX_FIX_NAME];
    snprintf(fix_name, sizeof fix_name, "%.*s", (int)strcspn(name, "."), name);
    snprintf(loading_module, sizeof loading_module, "%.*s", (int)sizeof loading_module - 1, name);
    begin_fix(fix_name);
    static char error[96];
    HMODULE module = LoadLibraryW(path);
    S2ModuleMain module_main = module ? (S2ModuleMain)(void *)GetProcAddress(module, "s2_module_main") : NULL;
    if (!module) {
        snprintf(error, sizeof error, "LoadLibrary failed: %lu", GetLastError());
        end_fix(error);
    } else if (!module_main) {
        end_fix("no s2_module_main export");
    } else {
        module_api.engine = engine;
        set_hook_owner(name);
        end_fix(module_main(&module_api));
    }
    loading_module[0] = 0;
}

// Fixes that moved into another module: an older s2fix's file left in fixes\ would install them twice.
static BOOL retired_module(const wchar_t *file) {
    static const wchar_t *const retired[] = {L"maincore.dll"};
    for (size_t i = 0; i < sizeof retired / sizeof *retired; i++)
        if (!_wcsicmp(file, retired[i])) return TRUE;
    return FALSE;
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
        if (retired_module(found.cFileName)) {
            log_line("%ls: from an older s2fix, now part of another fix; not loaded (it can be deleted)", found.cFileName);
            continue;
        }
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

// Set once the fixes are installed, for an offline check that drives engine functions only after that.
static HANDLE ready_event;

__declspec(dllexport) BOOL s2prof_wait_ready(DWORD milliseconds) {
    if (!ready_event) return FALSE;
    // The player may take longer over the build warning; the device must still wait for the fixes Launch Game installs.
    for (DWORD waited = 0; WaitForSingleObject(ready_event, WAIT_STEP_MS) != WAIT_OBJECT_0;)
        if (!build_warning_open && (waited += WAIT_STEP_MS) >= milliseconds) return FALSE;
    return TRUE;
}

static EngineBuild classify_engine(const BYTE *image) {
    if (!image) return ENGINE_NOT_LOADED;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != ENGINE_TIMESTAMP) return ENGINE_OTHER;
    if (nt->OptionalHeader.SizeOfImage == ENGINE_IMAGE_SIZE) return ENGINE_TARGETED;
    const int count = sizeof targeted_sections / sizeof *targeted_sections;
    if (nt->FileHeader.NumberOfSections < count) return ENGINE_OTHER;
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < count; i++, section++)
        if (strncmp((const char *)section->Name, targeted_sections[i].name, IMAGE_SIZEOF_SHORT_NAME) || section->VirtualAddress != targeted_sections[i].address ||
            section->Misc.VirtualSize != targeted_sections[i].size)
            return ENGINE_OTHER;
    return ENGINE_MODIFIED;
}

// MessageBox has no button texts of its own; these are set as the box opens.
static LRESULT CALLBACK name_buttons(int code, WPARAM wparam, LPARAM lparam) {
    if (code != HCBT_ACTIVATE) return CallNextHookEx(button_hook, code, wparam, lparam);
    SetDlgItemTextW((HWND)wparam, IDOK, L"Launch Game");
    SetDlgItemTextW((HWND)wparam, IDCANCEL, L"Exit Game");
    UnhookWindowsHookEx(button_hook);
    return 0;
}

// No owner window: the game's main thread is held in its device creation until the box is answered.
static BOOL launch_on_other_build(void) {
    wchar_t text[768];
    swprintf(text, sizeof text / sizeof *text,
             L"%ls\n\nempire.retail.x64.dll was modified or updated and does not match the expected build.\n\nFixes whose engine code still matches "
             L"are applied; the log in perf-logs lists the others. To launch without this message, set ignore_build_check=1 in s2prof.ini.",
             BUILD_WARNING);
    // The box takes no text, and Text Services on this thread could wait on the game's thread, blocked until we finish.
    ImmDisableIME(0);
    button_hook = SetWindowsHookExW(WH_CBT, name_buttons, NULL, GetCurrentThreadId());
    build_warning_open = TRUE;
    int answer = MessageBoxW(NULL, text, L"Shogun 2 fixes", MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    build_warning_open = FALSE;
    return answer != IDCANCEL;
}

static DWORD WINAPI notice_thread(LPVOID text) {
    // The box takes no text, and Text Services on this thread could wait on the game's thread.
    ImmDisableIME(0);
    MessageBoxW(NULL, text, L"Shogun 2 fixes", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

// On a thread of its own, so the controller goes on to install the fixes.
static void show_notice(const wchar_t *text) {
    HANDLE thread = CreateThread(NULL, 0, notice_thread, (LPVOID)text, 0, NULL);
    if (thread) CloseHandle(thread);
}

static HRESULT WINAPI create_d3d11_device(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
                                          const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count, UINT sdk_version, ID3D11Device **device,
                                          D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context) {
    UINT passed = multithreaded_device ? flags & ~D3D11_CREATE_DEVICE_SINGLETHREADED : flags;
    HRESULT result = real_d3d11_create_device(adapter, driver_type, software, passed, feature_levels, feature_level_count, sdk_version, device,
                                              feature_level, context);
    log_line("D3D11CreateDevice: flags 0x%x passed as 0x%x -> 0x%lx%s", flags, passed, result, SUCCEEDED(result) && wrap_context && device ? ", wrapped" : "");
    if (SUCCEEDED(result) && wrap_context && device) wrap_device(device, context);
    return result;
}

// The renderer starts: the fixes go in first, then the device is created through s2fix's D3D11CreateDevice.
static uint64_t create_device_hook(BYTE *device, uint64_t adapter) {
    hold_ran = TRUE;
    if (!s2prof_wait_ready(HOLD_WAIT_MS)) log_line("the fixes were not installed after %d s; the device is created without them", HOLD_WAIT_MS / 1000);
    void *volatile *slot = (void *volatile *)(engine + D3D11_CREATE_DEVICE_SLOT_RVA);
    real_d3d11_create_device = (D3D11CreateDeviceFn)*slot;
    // Without d3d11.dll the engine skips the call itself.
    if (real_d3d11_create_device) *slot = (void *)create_d3d11_device;
    uint64_t result = original_create_device(device, adapter);
    *slot = (void *)real_d3d11_create_device;
    return result;
}

// The device may be created soon after the engine loads, so its hook goes in at once, as s2fix or the engine loads.
static void engine_loaded(BYTE *base) {
    engine = base;
    const char *error = S2_CHECK_ENGINE(&module_api, device_checks);
    if (!error)
        error = hook_function(engine + CREATE_DEVICE_RVA, create_device_prologue, sizeof create_device_prologue, (const void *)create_device_hook,
                              (void **)&original_create_device);
    if (error) snprintf(device_hooks_error, sizeof device_hooks_error, "%s", error);
    SetEvent(engine_event);
}

static void CALLBACK dll_loaded(ULONG reason, const DllLoaded *loaded, void *context) {
    (void)context;
    size_t length = wcslen(ENGINE_MODULE);
    if (reason == DLL_LOADED && loaded->base_name->Length == length * sizeof(wchar_t) && !_wcsnicmp(loaded->base_name->Buffer, ENGINE_MODULE, length))
        engine_loaded(loaded->base);
}

static BOOL device_created(void) {
    BYTE *device = *(BYTE *volatile *)(engine + RENDER_DEVICE_RVA);
    return device && *(void *volatile *)(device + DEVICE_INTERFACE);
}


static DWORD WINAPI controller(LPVOID unused) {
    (void)unused;
    // Another loader may bring s2fix into the game's helper processes too; there the engine never loads.
    WaitForSingleObject(engine_event, INFINITE);
    DWORD console_error = open_console();
    start_run();
    AddVectoredExceptionHandler(1, report_crash);
    if (console_error) log_line("console unavailable: error %lu", console_error);
    engine_build = classify_engine(engine);
    log_line("s2prof running inside the game (engine at 0x%llx, %s).", (unsigned long long)(uintptr_t)engine, engine_build_names[engine_build]);
    const char *device_mismatch = S2_CHECK_ENGINE(&module_api, report_checks);
    device_readable = !device_mismatch;
    if (device_mismatch) log_line("the debug report's device readout (screenshots, GPU times) is off: %s", device_mismatch);
    if (device_hooks_error[0])
        log_line("device creation: not held for the fixes; multithreaded device and wrapped context off: %s", device_hooks_error);
    else
        log_line("device creation: held for the fixes; multithreaded device %s, context %s", multithreaded_device ? "on" : "off (multithreaded_device=0)",
                 wrap_context ? "wrapped" : "not wrapped (wrap_context=0)");
    if (!device_hooks_error[0] && !hold_ran && device_created() && (multithreaded_device || wrap_context)) {
        log_line("s2fix was loaded after the game created its graphics device: %s%s%s are off this session", multithreaded_device ? "the multithreaded device" : "",
                 multithreaded_device && wrap_context ? " and " : "", wrap_context ? "the wrapped context" : "");
        show_notice(LATE_MESSAGE);
    }
    const char *present_error =
        hook_function(engine + PRESENT_RVA, present_prologue, sizeof present_prologue, (const void *)present_hook, (void **)&original_present);
    if (present_error) log_line("main thread unknown (fixes that need it stay off): present hook: %s", present_error);
    if (engine_build == ENGINE_MODIFIED || engine_build == ENGINE_OTHER) {
        if (GetPrivateProfileIntW(L"s2prof", L"ignore_build_check", 0, config_path)) {
            log_line("build check off (ignore_build_check=1): installing every fix whose engine code still matches");
        } else if (launch_on_other_build()) {
            log_line("build warning: Launch Game; installing every fix whose engine code still matches");
        } else {
            log_line("build warning: Exit Game; ending the game");
            // The game is still starting on its own threads; its shutdown code isn't safe to run from here.
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
    ULONGLONG started = GetTickCount64();
    loading_fixes = TRUE;
    begin_fix("sortfix");
    const char *sort_error = S2_CHECK_ENGINE(&module_api, sortfix_checks);
    if (!sort_error) sort_error = sortfix_install(engine);
    if (!sort_error) register_switch("fewkeys", &few_keys_enabled, NULL);
    end_fix(sort_error);
    load_modules(fixes_dir);
    loading_fixes = FALSE;
    log_fix_summary(GetTickCount64() - started);
    set_apply_fixes(TRUE);
    log_help();
    SetEvent(ready_event);
    for (;;) {
        Sleep(50);
        // While the report runs it owns the switches and the capture, so the other hotkeys wait.
        BOOL busy = report_running();
        if (hotkey_pressed(VK_F10) && !busy) toggle_apply_fixes();
        if (hotkey_pressed(VK_F9) && !busy) capture(CAPTURE_SECONDS, &switches[0]);
        if (hotkey_pressed(VK_F8) && !busy) capture(CAPTURE_SECONDS, NULL);
        if (hotkey_pressed(VK_F6)) start_report();
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    // Present only when the process ends through ExitProcess; a fail-fast or TerminateProcess skips it.
    if (reason == DLL_PROCESS_DETACH && reserved) log_line("process exiting through ExitProcess");
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    // One s2fix per process: a second copy, from another loader or under another name, fails to load.
    wchar_t lock_name[64];
    swprintf(lock_name, sizeof lock_name / sizeof *lock_name, L"Local\\s2fix.%lu", GetCurrentProcessId());
    HANDLE instance_lock = CreateMutexW(NULL, FALSE, lock_name);
    if (!instance_lock || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instance_lock) CloseHandle(instance_lock);
        return FALSE;
    }
    DisableThreadLibraryCalls(instance);
    ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    engine_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    GetModuleFileNameW(instance, output_dir, MAX_PATH);
    wcscpy(fixes_dir, output_dir);
    wcscpy(wcsrchr(fixes_dir, L'\\'), L"\\fixes");
    wcscpy(config_path, output_dir);
    wcscpy(wcsrchr(config_path, L'\\'), L"\\s2prof.ini");
    wcscpy(wcsrchr(output_dir, L'\\'), L"\\perf-logs");
    multithreaded_device = GetPrivateProfileIntW(L"device", L"multithreaded_device", 0, config_path);
    wrap_context = GetPrivateProfileIntW(L"device", L"wrap_context", 0, config_path);
    // The thread first: a load that fails must leave no hook behind. It waits for the engine.
    HANDLE thread = CreateThread(NULL, 0, controller, NULL, 0, NULL);
    if (!thread) return FALSE;
    CloseHandle(thread);
    void *notification;
    BYTE *loaded = (BYTE *)GetModuleHandleW(ENGINE_MODULE);
    if (loaded)
        engine_loaded(loaded);
    else
        LdrRegisterDllNotification(0, dll_loaded, NULL, &notification);
    return TRUE;
}
