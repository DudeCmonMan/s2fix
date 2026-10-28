#pragma once
#include <windows.h>

// The debug report (Ctrl+Shift+F6): a fixed, timed sequence with the camera kept still that records the system, the
// process and every fix, A/B captures and screenshots, written to perf-logs\temp and packed into
// perf-logs\report_<date-time>.zip. Shared verbatim by the development and the release host.

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
} ReportHost;

void report_init(const ReportHost *host);
BOOL report_running(void);
// From the hotkey: runs the sequence on a thread of its own.
void report_start(void);
// On the main thread, around the engine's Frame::present.
void report_before_present(void);
void report_after_present(void);
