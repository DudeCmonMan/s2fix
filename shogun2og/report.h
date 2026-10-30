#pragma once
#include <windows.h>

// The debug report (Ctrl+Shift+F6): a fixed, timed sequence with the camera kept still that records the system, the
// process and every fix, A/B captures and screenshots, written to perf-logs\temp and packed into
// perf-logs\report_<date-time>.zip. The snapshot (Ctrl+Shift+F5, before a graphics reset) records the same system, process
// and fix sections at once, without the sequence. Shared verbatim by the development and the release host.

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
    // One line on the engine's graphics state (its reset flag, the device's status), for the snapshot; may be NULL.
    void (*graphics_state)(char *out, size_t size);
} ReportHost;

void report_init(const ReportHost *host);
BOOL report_running(void);
// From the hotkey: runs the sequence on a thread of its own.
void report_start(void);
// A snapshot for a problem on screen: the frame being shown, then the graphics state, the system, the process, s2fix
// and the display driver's recent events, packed into perf-logs\snapshot_<date-time>.zip on a thread of its own, which
// then calls done(reason) if given. FALSE when it could not start (the report is running, the folder or the thread could
// not be made): done is not called then.
BOOL report_snapshot(const char *reason, void (*done)(const char *reason));
// On the main thread, around the engine's present.
void report_before_present(void);
void report_after_present(void);
