#pragma once
#include <windows.h>

// The debug report: a timed sequence of A/B captures and screenshots plus the system, process and fix state, zipped into
// perf-logs. The snapshot records the same state at once, for a problem on screen.

typedef struct {
    const char *name;
    volatile LONG *flag;
    void (*report)(BOOL start);
} ReportSwitch;

typedef struct {
    BYTE *engine;
    const char *engine_build;
    // The device global and the device vtables are where the targeted build has them.
    BOOL device_readable;
    const wchar_t *output_dir;
    const wchar_t *config_path;
    const char *run_id;
    const char *version;
    void (*log_line)(const char *format, ...);
    int (*switch_count)(void);
    ReportSwitch (*switch_at)(int index);
    // The host's A/B capture of that switch (0 = Apply Fixes), without its beeps; returns the capture's number.
    int (*capture)(int seconds, int switch_index);
    // perf-logs\<run>_<name>, where the host writes its log and capture files.
    void (*output_path)(wchar_t *path, const char *name);
    // For the snapshot; may be NULL.
    void (*graphics_state)(char *out, size_t size);
} ReportHost;

void report_init(const ReportHost *host);
BOOL report_running(void);
// From the hotkey: runs the sequence on a thread of its own.
void report_start(void);
// On a thread of its own, which then calls done(reason) if given; FALSE, done not called, when it could not start.
BOOL report_snapshot(const char *reason, void (*done)(const char *reason));
// On the main thread, around the engine's present.
void report_before_present(void);
void report_after_present(void);
