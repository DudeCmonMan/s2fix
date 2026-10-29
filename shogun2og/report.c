#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "report.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <intrin.h>
#include <math.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <wincodec.h>
#include <winternl.h>

// The original game's render device: the swap chain and the immediate context the screenshots and GPU times use.
#define DEVICE_GLOBAL_RVA 0x1c9a93c
#define D3D11_DEVICE_VTABLE_RVA 0x17666f8
#define SWAP_CHAIN_OFFSET 0xbc
#define CONTEXT_OFFSET 0x108
// %APPDATA%\The Creative Assembly\Shogun2\scripts\ holds the game's settings.
#define PREFERENCES_PATH L"\\The Creative Assembly\\Shogun2\\scripts\\preferences.script.txt"
// An offline check runs the sequence with shorter times.
#ifndef BASELINE_MS
#define BASELINE_MS 10000
#define ALL_AB_SECONDS 20
#define FIX_BLOCK_MS 1000
#define SETTLE_MS 500
#define SHOT_WAIT_MS 3000
#endif
#define FIX_BLOCKS 4
// Frames this soon after a switch flips still carry the other mode's work in flight.
#define SKIP_AFTER_FLIP_MS 200
#define MAX_FRAMES (300 * 1000)
#define GPU_RING 8
#define BURST 4
#define JPEG_QUALITY 0.95f
#define MAX_FIXES 64

typedef struct {
    LONGLONG at;
    float gpu_ms;
    int8_t mode;
} FrameRecord;

typedef struct {
    ID3D11Query *disjoint, *start, *end;
    // The present it began after, and the frame record it times (-1 while nothing records).
    LONG present, frame;
    BOOL pending;
} GpuSet;

// A screenshot moves from requested (main copies the back buffer) to copied (main maps it a few frames later) to ready
// (its pixels are in memory for the report thread to encode).
enum { SHOT_FREE, SHOT_REQUESTED, SHOT_COPIED, SHOT_READY, SHOT_FAILED };

typedef struct {
    volatile LONG state;
    LONG copied_present;
    ID3D11Texture2D *staging;
    BYTE *pixels;
    UINT width, height;
    char why[96];
} Shot;

typedef struct {
    char *data;
    size_t length, capacity;
} Text;

typedef struct {
    int count;
    double mean, p50, p90, p95, p99, max;
    int gpu_count;
    double gpu_mean, gpu_p50, gpu_p99;
} Stats;

typedef struct {
    char name[64];
    Stats off, on;
    BOOL skipped;
    char note[96];
} FixResult;

typedef LONG(NTAPI *RtlGetVersionFn)(RTL_OSVERSIONINFOW *info);
typedef LONG(NTAPI *QueryTimerResolutionFn)(ULONG *coarsest, ULONG *finest, ULONG *current);
typedef HRESULT(WINAPI *GetThreadDescriptionFn)(HANDLE thread, PWSTR *description);
typedef BOOL(WINAPI *GetCpuSetsFn)(PSYSTEM_CPU_SET_INFORMATION information, ULONG length, PULONG returned, HANDLE process, ULONG flags);
typedef BOOL(WINAPI *GetProcessCpuSetsFn)(HANDLE process, PULONG ids, ULONG count, PULONG required);
typedef BOOL(WINAPI *GetThreadCpuSetsFn)(HANDLE thread, PULONG ids, ULONG count, PULONG required);
typedef HRESULT(WINAPI *CreateFactoryFn)(REFIID riid, void **factory);
typedef DWORD(WINAPI *PowerGetActiveSchemeFn)(HKEY root, GUID **scheme);
typedef DWORD(WINAPI *PowerReadFriendlyNameFn)(HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, UCHAR *buffer, DWORD *size);

static const GUID wic_factory_clsid = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
static const GUID wic_factory_iid = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
static const GUID png_container = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
static const GUID jpeg_container = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};
static const GUID bgr24_format = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
static const GUID texture2d_iid = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
static const GUID dxgi_device_iid = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const GUID dxgi_factory1_iid = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID dxgi_adapter3_iid = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};

// IDXGIAdapter3::QueryVideoMemoryInfo, called through the vtable: the 32-bit process's adapter description truncates
// memory sizes to 32 bits, which this reports in 64 bits.
typedef struct {
    UINT64 budget, usage, available_for_reservation, reservation;
} VideoMemoryInfo;
typedef HRESULT(STDMETHODCALLTYPE *QueryVideoMemoryFn)(void *adapter, UINT node, int group, VideoMemoryInfo *info);
#define QUERY_VIDEO_MEMORY_SLOT 14

static ReportHost host;
static volatile LONG running;
static LARGE_INTEGER frequency;
static wchar_t folder[MAX_PATH], temp_root[MAX_PATH];
static char stamp[32];

// Main thread state.
static FrameRecord *frames;
static volatile LONG frame_count, recording, gpu_stop, gpu_stopped;
// Every present while the report runs, recorded or not: readbacks wait on this.
static LONG presents;
static BOOL recorded_this_frame;
static volatile LONG *volatile mode_flag;
static GpuSet gpu_sets[GPU_RING];
static int current_gpu_set = -1;
static BOOL gpu_ready, gpu_broken;
static ID3D11Device *d3d_device;
static Shot shots[BURST];
static volatile LONG shots_wanted;

// Redaction: the user's profile folder, user name and computer name never leave in the report.
static char profile_utf8[MAX_PATH * 3], user_utf8[256], computer_utf8[256];

static double ms(LONGLONG ticks) {
    return (double)ticks * 1000.0 / (double)frequency.QuadPart;
}

static LONGLONG now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void to_utf8(const wchar_t *text, char *out, int size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, out, size, NULL, NULL)) out[0] = 0;
}

// ---- text ----

static void text_append(Text *t, const char *data, size_t length) {
    if (t->length + length + 1 > t->capacity) {
        size_t capacity = t->capacity ? t->capacity : 65536;
        while (t->length + length + 1 > capacity) capacity *= 2;
        char *grown = realloc(t->data, capacity);
        if (!grown) return;
        t->data = grown;
        t->capacity = capacity;
    }
    memcpy(t->data + t->length, data, length);
    t->length += length;
    t->data[t->length] = 0;
}

static void text_printf(Text *t, const char *format, ...) {
    char line[4096];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (length < 0) return;
    if (length >= (int)sizeof line) length = sizeof line - 1;
    text_append(t, line, (size_t)length);
}

static void heading(Text *t, const char *title) {
    text_printf(t, "\n== %s ==\n", title);
}

static BOOL equal_ignoring_case(const char *a, const char *b, size_t length) {
    for (size_t i = 0; i < length; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return FALSE;
    }
    return TRUE;
}

static void replace_all(Text *t, const char *needle, const char *replacement) {
    size_t needle_length = strlen(needle);
    if (needle_length < 3 || !t->data) return;
    Text out = {0};
    size_t i = 0;
    while (i < t->length) {
        if (i + needle_length <= t->length && equal_ignoring_case(t->data + i, needle, needle_length)) {
            text_append(&out, replacement, strlen(replacement));
            i += needle_length;
        } else {
            text_append(&out, t->data + i, 1);
            i++;
        }
    }
    free(t->data);
    *t = out;
}

static void redact(Text *t) {
    replace_all(t, profile_utf8, "%USERPROFILE%");
    replace_all(t, user_utf8, "<user>");
    replace_all(t, computer_utf8, "<computer>");
}

static void temp_path(wchar_t *path, const char *name) {
    wchar_t wide[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, MAX_PATH);
    swprintf(path, MAX_PATH, L"%ls\\%ls", folder, wide);
}

static BOOL write_file(const wchar_t *path, const void *data, size_t length) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    DWORD written = 0;
    BOOL ok = WriteFile(file, data, (DWORD)length, &written, NULL) && written == length;
    CloseHandle(file);
    return ok;
}

static BOOL read_file(const wchar_t *path, Text *out) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    char buffer[65536];
    DWORD got;
    while (ReadFile(file, buffer, sizeof buffer, &got, NULL) && got) text_append(out, buffer, got);
    CloseHandle(file);
    return TRUE;
}

// A text file copied into the report, redacted; a UTF-16 one (the game's settings) is converted to UTF-8 first.
static BOOL copy_text_file(const wchar_t *source, const char *name) {
    Text text = {0};
    if (!read_file(source, &text)) return FALSE;
    if (text.length >= 2 && (BYTE)text.data[0] == 0xff && (BYTE)text.data[1] == 0xfe) {
        int chars = (int)(text.length - 2) / 2;
        int bytes = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(text.data + 2), chars, NULL, 0, NULL, NULL);
        Text converted = {0};
        char *utf8 = malloc((size_t)bytes + 1);
        if (utf8) {
            WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(text.data + 2), chars, utf8, bytes, NULL, NULL);
            text_append(&converted, utf8, (size_t)bytes);
            free(utf8);
        }
        free(text.data);
        text = converted;
    }
    redact(&text);
    wchar_t path[MAX_PATH];
    temp_path(path, name);
    BOOL ok = write_file(path, text.data ? text.data : "", text.length);
    free(text.data);
    return ok;
}

// ---- the main thread: frames, GPU times, screenshots ----

static BYTE *render_device(void) {
    BYTE *device = host.engine && host.device_readable ? *(BYTE *volatile *)(host.engine + DEVICE_GLOBAL_RVA) : NULL;
    return device && *(BYTE **)device == host.engine + D3D11_DEVICE_VTABLE_RVA ? device : NULL;
}

static ID3D11DeviceContext *immediate_context(void) {
    BYTE *device = render_device();
    return device ? *(ID3D11DeviceContext **)(device + CONTEXT_OFFSET) : NULL;
}

static IDXGISwapChain *swap_chain(void) {
    BYTE *device = render_device();
    return device ? *(IDXGISwapChain **)(device + SWAP_CHAIN_OFFSET) : NULL;
}

// The back buffer formats the screenshots and the banner read and write.
static BOOL readable_format(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8X8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

static void make_gpu_sets(ID3D11DeviceContext *context) {
    ID3D11DeviceContext_GetDevice(context, &d3d_device);
    D3D11_QUERY_DESC disjoint = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp_desc = {D3D11_QUERY_TIMESTAMP, 0};
    for (int i = 0; i < GPU_RING; i++)
        if (FAILED(ID3D11Device_CreateQuery(d3d_device, &disjoint, &gpu_sets[i].disjoint)) ||
            FAILED(ID3D11Device_CreateQuery(d3d_device, &stamp_desc, &gpu_sets[i].start)) ||
            FAILED(ID3D11Device_CreateQuery(d3d_device, &stamp_desc, &gpu_sets[i].end)))
            gpu_broken = TRUE;
    gpu_ready = TRUE;
}

static void read_gpu_sets(ID3D11DeviceContext *context) {
    for (int i = 0; i < GPU_RING; i++) {
        GpuSet *set = &gpu_sets[i];
        if (!set->pending || presents - set->present < GPU_RING - 2) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
        uint64_t start, end;
        if (ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)set->disjoint, &disjoint, sizeof disjoint, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)set->start, &start, sizeof start, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)set->end, &end, sizeof end, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            if (presents - set->present > GPU_RING) set->pending = FALSE;
            continue;
        }
        set->pending = FALSE;
        if (!disjoint.Disjoint && disjoint.Frequency && set->frame >= 0 && set->frame < frame_count)
            frames[set->frame].gpu_ms = (float)((double)(end - start) * 1000.0 / (double)disjoint.Frequency);
    }
}

static BOOL convert_pixels(Shot *shot, const D3D11_MAPPED_SUBRESOURCE *mapped, DXGI_FORMAT format) {
    size_t row = (size_t)shot->width * 3;
    shot->pixels = malloc(row * shot->height);
    if (!shot->pixels) {
        snprintf(shot->why, sizeof shot->why, "out of memory for %u x %u pixels", shot->width, shot->height);
        return FALSE;
    }
    BOOL bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8X8_UNORM ||
                format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    BOOL ten_bit = format == DXGI_FORMAT_R10G10B10A2_UNORM;
    for (UINT y = 0; y < shot->height; y++) {
        const uint32_t *in = (const uint32_t *)((const BYTE *)mapped->pData + (size_t)y * mapped->RowPitch);
        BYTE *out = shot->pixels + y * row;
        for (UINT x = 0; x < shot->width; x++, out += 3) {
            uint32_t p = in[x];
            BYTE r, g, b;
            if (ten_bit) {
                r = (BYTE)((p & 0x3ff) >> 2);
                g = (BYTE)((p >> 10 & 0x3ff) >> 2);
                b = (BYTE)((p >> 20 & 0x3ff) >> 2);
            } else if (bgra) {
                b = (BYTE)p, g = (BYTE)(p >> 8), r = (BYTE)(p >> 16);
            } else {
                r = (BYTE)p, g = (BYTE)(p >> 8), b = (BYTE)(p >> 16);
            }
            out[0] = b, out[1] = g, out[2] = r;
        }
    }
    return TRUE;
}

// Before Present the back buffer holds the finished frame; it is copied (resolved first if multisampled) to a staging
// texture and read a few frames later, so main never waits on the GPU.
static void copy_back_buffer(Shot *shot, ID3D11DeviceContext *context) {
    IDXGISwapChain *chain = swap_chain();
    ID3D11Texture2D *back = NULL;
    if (!chain || FAILED(IDXGISwapChain_GetBuffer(chain, 0, &texture2d_iid, (void **)&back))) {
        snprintf(shot->why, sizeof shot->why, "no swap chain or back buffer");
        shot->state = SHOT_FAILED;
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(back, &desc);
    if (!readable_format(desc.Format)) {
        snprintf(shot->why, sizeof shot->why, "back buffer format %d is not one this report reads", (int)desc.Format);
        shot->state = SHOT_FAILED;
        ID3D11Texture2D_Release(back);
        return;
    }
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.MipLevels = staging_desc.ArraySize = 1;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    if (shot->staging) {
        D3D11_TEXTURE2D_DESC old;
        ID3D11Texture2D_GetDesc(shot->staging, &old);
        if (old.Width != desc.Width || old.Height != desc.Height || old.Format != desc.Format) {
            ID3D11Texture2D_Release(shot->staging);
            shot->staging = NULL;
        }
    }
    if (!shot->staging && FAILED(ID3D11Device_CreateTexture2D(d3d_device, &staging_desc, NULL, &shot->staging))) {
        snprintf(shot->why, sizeof shot->why, "CreateTexture2D for the staging copy failed");
        shot->state = SHOT_FAILED;
        ID3D11Texture2D_Release(back);
        return;
    }
    if (desc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC resolved_desc = staging_desc;
        resolved_desc.Usage = D3D11_USAGE_DEFAULT;
        resolved_desc.CPUAccessFlags = 0;
        ID3D11Texture2D *resolved;
        if (FAILED(ID3D11Device_CreateTexture2D(d3d_device, &resolved_desc, NULL, &resolved))) {
            snprintf(shot->why, sizeof shot->why, "CreateTexture2D for the resolve failed");
            shot->state = SHOT_FAILED;
            ID3D11Texture2D_Release(back);
            return;
        }
        ID3D11DeviceContext_ResolveSubresource(context, (ID3D11Resource *)resolved, 0, (ID3D11Resource *)back, 0, desc.Format);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)shot->staging, (ID3D11Resource *)resolved);
        ID3D11Texture2D_Release(resolved);
    } else {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)shot->staging, (ID3D11Resource *)back);
    }
    ID3D11Texture2D_Release(back);
    shot->width = desc.Width;
    shot->height = desc.Height;
    shot->copied_present = presents;
    shot->state = SHOT_COPIED;
}

static void map_copies(ID3D11DeviceContext *context) {
    for (int i = 0; i < BURST; i++) {
        Shot *shot = &shots[i];
        if (shot->state != SHOT_COPIED || presents - shot->copied_present < 2) continue;
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)shot->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(result)) {
            snprintf(shot->why, sizeof shot->why, "Map of the staging copy failed: 0x%08lx", (unsigned long)result);
            shot->state = SHOT_FAILED;
            continue;
        }
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D_GetDesc(shot->staging, &desc);
        BOOL ok = convert_pixels(shot, &mapped, desc.Format);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)shot->staging, 0);
        shot->state = ok ? SHOT_READY : SHOT_FAILED;
    }
}

// ---- the banner on screen ----

// "Snapshot Running: <stage>, <percent>" in the middle of the screen while the report runs: GDI renders it on main when
// its text changes (at stage boundaries, outside the measured blocks), and every frame it is copied onto the back buffer
// before Present, after the GPU's end timestamp and the screenshot copy, so neither includes it. A copy changes no
// pipeline state the engine caches.
static SRWLOCK overlay_lock = SRWLOCK_INIT;
static char overlay_text[160];
static volatile LONG overlay_version;
static LONG overlay_drawn_version;
static ID3D11Texture2D *overlay_texture;
static UINT overlay_width, overlay_height;
static DXGI_FORMAT overlay_format;

static void set_overlay(const char *stage, int percent) {
    AcquireSRWLockExclusive(&overlay_lock);
    snprintf(overlay_text, sizeof overlay_text, "Snapshot Running: %s, %d%%", stage, percent);
    ReleaseSRWLockExclusive(&overlay_lock);
    InterlockedIncrement(&overlay_version);
}

// The banner follows the back buffer, the resolution the game renders at: text 1/20 of its height, 70% of its width.
static void overlay_size(const D3D11_TEXTURE2D_DESC *target, UINT *font_height, UINT *width, UINT *height) {
    *font_height = target->Height / 20 < 24 ? 24 : target->Height / 20;
    *width = target->Width * 7 / 10;
    *height = *font_height * 2;
}

static void render_overlay(ID3D11DeviceContext *context, const D3D11_TEXTURE2D_DESC *target) {
    UINT font_height, width, height;
    overlay_size(target, &font_height, &width, &height);
    if (width < 64 || height > target->Height) return;
    if (!overlay_texture || overlay_width != width || overlay_height != height || overlay_format != target->Format) {
        if (overlay_texture) ID3D11Texture2D_Release(overlay_texture);
        overlay_texture = NULL;
        D3D11_TEXTURE2D_DESC desc = {width, height, 1, 1, target->Format, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(d3d_device, &desc, NULL, &overlay_texture))) return;
        overlay_width = width, overlay_height = height, overlay_format = target->Format;
    }
    wchar_t text[160];
    AcquireSRWLockShared(&overlay_lock);
    MultiByteToWideChar(CP_UTF8, 0, overlay_text, -1, text, 160);
    ReleaseSRWLockShared(&overlay_lock);
    BITMAPINFO info = {{sizeof(BITMAPINFOHEADER), (LONG)width, -(LONG)height, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    uint32_t *converted = malloc((size_t)width * height * 4);
    if (bitmap && bits && converted) {
        HGDIOBJ previous_bitmap = SelectObject(dc, bitmap);
        int border = height / 16 < 3 ? 3 : (int)height / 16;
        RECT whole = {0, 0, (LONG)width, (LONG)height}, inner = {border, border, (LONG)width - border, (LONG)height - border};
        HBRUSH frame = CreateSolidBrush(RGB(255, 170, 0)), panel = CreateSolidBrush(RGB(16, 16, 16));
        FillRect(dc, &whole, frame);
        FillRect(dc, &inner, panel);
        HFONT font = CreateFontW(-(int)font_height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ previous_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, text, -1, &inner, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        GdiFlush();
        const uint32_t *in = bits;
        BOOL bgra = target->Format == DXGI_FORMAT_B8G8R8A8_UNORM || target->Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                    target->Format == DXGI_FORMAT_B8G8R8X8_UNORM || target->Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        for (size_t i = 0; i < (size_t)width * height; i++) {
            uint32_t b = in[i] & 0xff, g = in[i] >> 8 & 0xff, r = in[i] >> 16 & 0xff;
            converted[i] = target->Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 0xc0000000u | (b * 1023 / 255) << 20 | (g * 1023 / 255) << 10 | r * 1023 / 255
                           : bgra                                             ? 0xff000000u | r << 16 | g << 8 | b
                                                                              : 0xff000000u | b << 16 | g << 8 | r;
        }
        ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)overlay_texture, 0, NULL, converted, width * 4, 0);
        SelectObject(dc, previous_font);
        SelectObject(dc, previous_bitmap);
        DeleteObject(font);
        DeleteObject(frame);
        DeleteObject(panel);
    }
    free(converted);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
}

static void draw_overlay(ID3D11DeviceContext *context) {
    IDXGISwapChain *chain = swap_chain();
    ID3D11Texture2D *back = NULL;
    if (!overlay_version || !d3d_device || !chain || FAILED(IDXGISwapChain_GetBuffer(chain, 0, &texture2d_iid, (void **)&back))) return;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(back, &desc);
    if (desc.SampleDesc.Count == 1 && readable_format(desc.Format)) {
        LONG version = overlay_version;
        UINT font_height, width, height;
        overlay_size(&desc, &font_height, &width, &height);
        if (version != overlay_drawn_version || !overlay_texture || overlay_format != desc.Format || overlay_width != width || overlay_height != height) {
            render_overlay(context, &desc);
            overlay_drawn_version = version;
        }
        if (overlay_texture && overlay_width <= desc.Width && overlay_height <= desc.Height)
            ID3D11DeviceContext_CopySubresourceRegion(context, (ID3D11Resource *)back, 0, (desc.Width - overlay_width) / 2, (desc.Height - overlay_height) / 2, 0,
                                                      (ID3D11Resource *)overlay_texture, 0, NULL);
    }
    ID3D11Texture2D_Release(back);
}

// Main alone uses these, so it lets them go once the report has stopped: a display change rebuilds the game's device,
// and anything made on the old one keeps it, and with it the window's flip-model swap chain, alive.
static volatile LONG release_wanted;

static void release_device_objects(void) {
    for (int i = 0; i < GPU_RING; i++) {
        if (gpu_sets[i].disjoint) ID3D11Query_Release(gpu_sets[i].disjoint);
        if (gpu_sets[i].start) ID3D11Query_Release(gpu_sets[i].start);
        if (gpu_sets[i].end) ID3D11Query_Release(gpu_sets[i].end);
        gpu_sets[i] = (GpuSet){0};
    }
    for (int i = 0; i < BURST; i++) {
        if (shots[i].staging) ID3D11Texture2D_Release(shots[i].staging);
        shots[i].staging = NULL;
    }
    if (overlay_texture) ID3D11Texture2D_Release(overlay_texture);
    overlay_texture = NULL;
    if (d3d_device) ID3D11Device_Release(d3d_device);
    d3d_device = NULL;
    gpu_ready = gpu_broken = FALSE;
    current_gpu_set = -1;
    release_wanted = 0;
}

void report_before_present(void) {
    if (release_wanted) release_device_objects();
    if (!running) return;
    ID3D11DeviceContext *context = immediate_context();
    LONG index = frame_count;
    recorded_this_frame = recording && index < MAX_FRAMES;
    if (recorded_this_frame) {
        volatile LONG *flag = mode_flag;
        frames[index] = (FrameRecord){now(), -1.0f, (int8_t)(flag ? *flag != 0 : -1)};
    }
    if (!context) return;
    if (!gpu_ready) make_gpu_sets(context);
    if (!gpu_broken && current_gpu_set >= 0) {
        GpuSet *set = &gpu_sets[current_gpu_set];
        ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)set->end);
        ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)set->disjoint);
        set->pending = TRUE;
        if (gpu_stop) current_gpu_set = -1;
    }
    if (gpu_stop) gpu_stopped = 1;
    // One back buffer copy per frame while a burst is wanted, so its frames are consecutive.
    if (shots_wanted && d3d_device)
        for (int i = 0; i < BURST; i++)
            if (shots[i].state == SHOT_REQUESTED) {
                copy_back_buffer(&shots[i], context);
                break;
            }
    if (!gpu_stop && d3d_device) draw_overlay(context);
}

void report_after_present(void) {
    if (!running) return;
    ID3D11DeviceContext *context = immediate_context();
    if (recorded_this_frame) InterlockedIncrement(&frame_count);
    recorded_this_frame = FALSE;
    presents++;
    if (!context || !gpu_ready) return;
    map_copies(context);
    if (gpu_broken) return;
    read_gpu_sets(context);
    if (gpu_stop) return;
    current_gpu_set = (current_gpu_set + 1) % GPU_RING;
    GpuSet *set = &gpu_sets[current_gpu_set];
    set->present = presents;
    set->frame = recording && frame_count < MAX_FRAMES ? frame_count : -1;
    set->pending = FALSE;
    ID3D11DeviceContext_Begin(context, (ID3D11Asynchronous *)set->disjoint);
    ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)set->start);
}

// ---- screenshots on the report thread ----

static BOOL encode(const Shot *shot, const wchar_t *path, BOOL jpeg) {
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    BOOL ok = SUCCEEDED(CoCreateInstance(&wic_factory_clsid, NULL, CLSCTX_INPROC_SERVER, &wic_factory_iid, (void **)&factory)) &&
              SUCCEEDED(IWICImagingFactory_CreateStream(factory, &stream)) && SUCCEEDED(IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE)) &&
              SUCCEEDED(IWICImagingFactory_CreateEncoder(factory, jpeg ? &jpeg_container : &png_container, NULL, &encoder)) &&
              SUCCEEDED(IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties));
    if (ok && jpeg) {
        PROPBAG2 option = {0};
        option.pstrName = L"ImageQuality";
        VARIANT value;
        memset(&value, 0, sizeof value);
        value.vt = VT_R4;
        value.fltVal = JPEG_QUALITY;
        IPropertyBag2_Write(properties, 1, &option, &value);
    }
    WICPixelFormatGUID format = bgr24_format;
    ok = ok && SUCCEEDED(IWICBitmapFrameEncode_Initialize(frame, properties)) && SUCCEEDED(IWICBitmapFrameEncode_SetSize(frame, shot->width, shot->height)) &&
         SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(frame, &format)) && IsEqualGUID(&format, &bgr24_format) &&
         SUCCEEDED(IWICBitmapFrameEncode_WritePixels(frame, shot->height, shot->width * 3, shot->width * 3 * shot->height, shot->pixels)) &&
         SUCCEEDED(IWICBitmapFrameEncode_Commit(frame)) && SUCCEEDED(IWICBitmapEncoder_Commit(encoder));
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    return ok;
}

// Takes count consecutive frames; the first can be lossless. Lines go into the report's screenshot list.
static void take_burst(Text *list, const char *state, int count, BOOL first_png) {
    for (int i = 0; i < count; i++) {
        if (shots[i].pixels) free(shots[i].pixels);
        shots[i].pixels = NULL;
        shots[i].why[0] = 0;
        shots[i].state = SHOT_REQUESTED;
    }
    shots_wanted = 1;
    LONGLONG deadline = now() + frequency.QuadPart * SHOT_WAIT_MS / 1000;
    for (;;) {
        int done = 0;
        for (int i = 0; i < count; i++) done += shots[i].state == SHOT_READY || shots[i].state == SHOT_FAILED;
        if (done == count || now() > deadline) break;
        Sleep(5);
    }
    shots_wanted = 0;
    // A copy main started just before the deadline finishes before its slot is looked at.
    Sleep(50);
    for (int i = 0; i < count; i++) {
        Shot *shot = &shots[i];
        BOOL jpeg = !(first_png && i == 0);
        char name[96];
        snprintf(name, sizeof name, "shot_%s_%d.%s", state, i + 1, jpeg ? "jpg" : "png");
        if (shot->state == SHOT_READY) {
            wchar_t path[MAX_PATH];
            temp_path(path, name);
            BOOL ok = encode(shot, path, jpeg);
            text_printf(list, "  %-34s %s (%u x %u)%s\n", name, state, shot->width, shot->height, ok ? "" : " ENCODING FAILED");
        } else {
            text_printf(list, "  %-34s %s: not taken (%s)\n", name, state, shot->state == SHOT_FAILED ? shot->why : "no frame presented in time");
        }
        free(shot->pixels);
        shot->pixels = NULL;
        shot->state = SHOT_FREE;
    }
}

// ---- frame statistics ----

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double percentile(const double *sorted, int count, double fraction) {
    if (!count) return 0;
    int i = (int)(fraction * (count - 1) + 0.5);
    return sorted[i < count ? i : count - 1];
}

// Frames from first to last with the given mode (-1: any), each frame's time being the interval since the one before;
// frames within SKIP_AFTER_FLIP_MS of a mode change are left out.
static void frame_stats(LONG first, LONG last, int mode, Stats *out) {
    memset(out, 0, sizeof *out);
    if (last - first < 2) return;
    double *intervals = malloc(sizeof(double) * (size_t)(last - first)), *gpu = malloc(sizeof(double) * (size_t)(last - first));
    if (!intervals || !gpu) {
        free(intervals);
        free(gpu);
        return;
    }
    LONGLONG skip = frequency.QuadPart * SKIP_AFTER_FLIP_MS / 1000, flipped_at = frames[first].at;
    int count = 0, gpu_count = 0;
    double sum = 0, gpu_sum = 0;
    for (LONG i = first + 1; i < last; i++) {
        if (frames[i].mode != frames[i - 1].mode) flipped_at = frames[i].at;
        if (mode >= 0 && frames[i].mode != mode) continue;
        if (frames[i].at - flipped_at < skip) continue;
        double interval = ms(frames[i].at - frames[i - 1].at);
        intervals[count++] = interval;
        sum += interval;
        if (frames[i].gpu_ms >= 0) {
            gpu[gpu_count++] = frames[i].gpu_ms;
            gpu_sum += frames[i].gpu_ms;
        }
    }
    qsort(intervals, (size_t)count, sizeof *intervals, compare_doubles);
    qsort(gpu, (size_t)gpu_count, sizeof *gpu, compare_doubles);
    out->count = count;
    out->mean = count ? sum / count : 0;
    out->p50 = percentile(intervals, count, 0.50);
    out->p90 = percentile(intervals, count, 0.90);
    out->p95 = percentile(intervals, count, 0.95);
    out->p99 = percentile(intervals, count, 0.99);
    out->max = count ? intervals[count - 1] : 0;
    out->gpu_count = gpu_count;
    out->gpu_mean = gpu_count ? gpu_sum / gpu_count : 0;
    out->gpu_p50 = percentile(gpu, gpu_count, 0.50);
    out->gpu_p99 = percentile(gpu, gpu_count, 0.99);
    free(intervals);
    free(gpu);
}

static void print_stats(Text *t, const char *label, const Stats *s) {
    text_printf(t, "  %-10s %6d frames  %7.1f fps  ms: mean %6.3f  p50 %6.3f  p90 %6.3f  p95 %6.3f  p99 %6.3f  max %7.3f", label, s->count,
                s->mean > 0 ? 1000.0 / s->mean : 0.0, s->mean, s->p50, s->p90, s->p95, s->p99, s->max);
    if (s->gpu_count)
        text_printf(t, "  | GPU ms (%d frames): mean %6.3f  p50 %6.3f  p99 %6.3f  (GPU busy %.0f%% of the frame)\n", s->gpu_count, s->gpu_mean, s->gpu_p50,
                    s->gpu_p99, s->mean > 0 ? 100.0 * s->gpu_mean / s->mean : 0.0);
    else
        text_printf(t, "  | GPU: no times\n");
}

static void start_recording(volatile LONG *flag, LONG *first) {
    mode_flag = flag;
    *first = frame_count;
    recording = 1;
}

static LONG stop_recording(void) {
    recording = 0;
    // The GPU times of the last frames arrive a few frames later.
    Sleep(150);
    return frame_count;
}

// ---- the snapshot ----

static void file_version(const wchar_t *path, char *out, size_t size) {
    out[0] = 0;
    DWORD handle, bytes = GetFileVersionInfoSizeW(path, &handle);
    if (!bytes) return;
    BYTE *data = malloc(bytes);
    if (!data) return;
    VS_FIXEDFILEINFO *fixed;
    UINT length;
    if (GetFileVersionInfoW(path, 0, bytes, data) && VerQueryValueW(data, L"\\", (void **)&fixed, &length) && length >= sizeof *fixed) {
        int used = snprintf(out, size, "%lu.%lu.%lu.%lu", fixed->dwFileVersionMS >> 16, fixed->dwFileVersionMS & 0xffff, fixed->dwFileVersionLS >> 16,
                            fixed->dwFileVersionLS & 0xffff);
        struct {
            WORD language, codepage;
        } *translation;
        if (used > 0 && (size_t)used < size && VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&translation, &length) && length >= 4) {
            const wchar_t *keys[] = {L"CompanyName", L"ProductName"};
            for (int k = 0; k < 2; k++) {
                wchar_t query[128];
                swprintf(query, 128, L"\\StringFileInfo\\%04x%04x\\%ls", translation->language, translation->codepage, keys[k]);
                wchar_t *value;
                if (VerQueryValueW(data, query, (void **)&value, &length) && length > 1) {
                    char utf8[256];
                    to_utf8(value, utf8, sizeof utf8);
                    used += snprintf(out + used, size - (size_t)used, ", %s", utf8);
                    if ((size_t)used >= size) break;
                }
            }
        }
    }
    free(data);
}

static void section_system(Text *t) {
    heading(t, "System");
    int registers[4];
    char brand[49] = {0};
    __cpuid(registers, 0x80000000);
    if ((unsigned)registers[0] >= 0x80000004u)
        for (int i = 0; i < 3; i++) {
            __cpuid(registers, 0x80000002 + i);
            memcpy(brand + i * 16, registers, 16);
        }
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    for (int i = (int)strlen(brand) - 1; i >= 0 && brand[i] == ' '; i--) brand[i] = 0;
    text_printf(t, "CPU: %s; %lu logical processors, active mask %llx\n", brand[0] ? brand : "(unknown)", info.dwNumberOfProcessors,
                (unsigned long long)info.dwActiveProcessorMask);
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &bytes);
    BYTE *buffer = bytes ? malloc(bytes) : NULL;
    if (buffer && GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer, &bytes)) {
        int cores = 0, smt_cores = 0;
        // Caches grouped by level and size: a dual-CCD X3D shows its two L3s apart.
        struct {
            int level;
            DWORD size;
            int count;
            char masks[256];
        } caches[8] = {0};
        int cache_kinds = 0;
        for (DWORD at = 0; at < bytes;) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer + at);
            if (item->Relationship == RelationProcessorCore) {
                cores++;
                smt_cores += (item->Processor.Flags & LTP_PC_SMT) != 0;
            } else if (item->Relationship == RelationCache && item->Cache.Level >= 2 && item->Cache.Type != CacheInstruction) {
                int k = 0;
                while (k < cache_kinds && (caches[k].level != item->Cache.Level || caches[k].size != item->Cache.CacheSize)) k++;
                if (k == cache_kinds && cache_kinds < 8) {
                    caches[k].level = item->Cache.Level;
                    caches[k].size = item->Cache.CacheSize;
                    cache_kinds++;
                }
                if (k < cache_kinds) {
                    size_t used = strlen(caches[k].masks);
                    if (used < sizeof caches[k].masks - 20)
                        snprintf(caches[k].masks + used, sizeof caches[k].masks - used, "%s%llx", used ? " " : "", (unsigned long long)item->Cache.GroupMask.Mask);
                    caches[k].count++;
                }
            } else if (item->Relationship == RelationNumaNode) {
                text_printf(t, "  NUMA node %lu, CPUs %llx\n", item->NumaNode.NodeNumber, (unsigned long long)item->NumaNode.GroupMask.Mask);
            }
            at += item->Size;
        }
        for (int k = 0; k < cache_kinds; k++)
            text_printf(t, "  %d x L%d cache %lu KB, on CPUs %s\n", caches[k].count, caches[k].level, caches[k].size / 1024, caches[k].masks);
        text_printf(t, "  %d physical cores, %d with SMT\n", cores, smt_cores);
    }
    free(buffer);
    GetCpuSetsFn get_cpu_sets = (GetCpuSetsFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation");
    ULONG length = 0;
    if (get_cpu_sets) get_cpu_sets(NULL, 0, &length, GetCurrentProcess(), 0);
    buffer = length ? malloc(length) : NULL;
    if (buffer && get_cpu_sets(( PSYSTEM_CPU_SET_INFORMATION)buffer, length, &length, GetCurrentProcess(), 0)) {
        text_printf(t, "CPU sets (id: logical, core, last-level cache, efficiency class, flags, rank):\n ");
        for (ULONG at = 0; at < length;) {
            PSYSTEM_CPU_SET_INFORMATION set = (PSYSTEM_CPU_SET_INFORMATION)(buffer + at);
            if (set->Type == CpuSetInformation)
                text_printf(t, " %lu:%u/%u/%u/%u/%02x/%u", set->CpuSet.Id, set->CpuSet.LogicalProcessorIndex, set->CpuSet.CoreIndex, set->CpuSet.LastLevelCacheIndex,
                            set->CpuSet.EfficiencyClass, set->CpuSet.AllFlags, set->CpuSet.SchedulingClass);
            at += set->Size;
        }
        text_printf(t, "\n");
    }
    free(buffer);
    MEMORYSTATUSEX memory;
    memset(&memory, 0, sizeof memory);
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory))
        text_printf(t, "Memory: %.1f GB, %.1f GB free (%lu%% in use); page file %.1f GB, %.1f GB free\n", memory.ullTotalPhys / 1073741824.0,
                    memory.ullAvailPhys / 1073741824.0, memory.dwMemoryLoad, memory.ullTotalPageFile / 1073741824.0, memory.ullAvailPageFile / 1073741824.0);
    RtlGetVersionFn get_version = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RTL_OSVERSIONINFOW os;
    memset(&os, 0, sizeof os);
    os.dwOSVersionInfoSize = sizeof os;
    char display[64] = "";
    DWORD ubr = 0, size = sizeof ubr;
    HKEY key;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ | KEY_WOW64_64KEY, &key)) {
        RegQueryValueExW(key, L"UBR", NULL, NULL, (BYTE *)&ubr, &size);
        wchar_t wide[64];
        DWORD wide_size = sizeof wide;
        if (!RegQueryValueExW(key, L"DisplayVersion", NULL, NULL, (BYTE *)wide, &wide_size)) to_utf8(wide, display, sizeof display);
        RegCloseKey(key);
    }
    if (get_version && !get_version(&os))
        text_printf(t, "Windows %lu.%lu build %lu.%lu %s\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, ubr, display);
    HMODULE powrprof = LoadLibraryW(L"powrprof.dll");
    PowerGetActiveSchemeFn active = powrprof ? (PowerGetActiveSchemeFn)(void *)GetProcAddress(powrprof, "PowerGetActiveScheme") : NULL;
    PowerReadFriendlyNameFn friendly = powrprof ? (PowerReadFriendlyNameFn)(void *)GetProcAddress(powrprof, "PowerReadFriendlyName") : NULL;
    GUID *scheme = NULL;
    if (active && friendly && !active(NULL, &scheme)) {
        wchar_t name[128] = L"";
        DWORD name_size = sizeof name;
        friendly(NULL, scheme, NULL, NULL, (UCHAR *)name, &name_size);
        char utf8[256];
        to_utf8(name, utf8, sizeof utf8);
        text_printf(t, "Power plan: %s\n", utf8);
        LocalFree(scheme);
    }
    if (powrprof) FreeLibrary(powrprof);
}

static void section_gpu(Text *t) {
    heading(t, "GPUs and displays");
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    CreateFactoryFn create = dxgi ? (CreateFactoryFn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *factory = NULL;
    if (!create || FAILED(create(&dxgi_factory1_iid, (void **)&factory))) {
        text_printf(t, "DXGI unavailable\n");
        return;
    }
    IDXGIAdapter1 *adapter;
    for (UINT a = 0; SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, a, &adapter)); a++) {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(adapter, &desc);
        LARGE_INTEGER driver = {0};
        IDXGIAdapter1_CheckInterfaceSupport(adapter, &dxgi_device_iid, &driver);
        char name[256];
        to_utf8(desc.Description, name, sizeof name);
        text_printf(t, "Adapter %u: %s; vendor %04x device %04x subsys %08x rev %u; driver %u.%u.%u.%u; LUID %08lx:%08lx%s\n", a, name, desc.VendorId,
                    desc.DeviceId, desc.SubSysId, desc.Revision, (unsigned)(driver.QuadPart >> 48), (unsigned)(driver.QuadPart >> 32 & 0xffff),
                    (unsigned)(driver.QuadPart >> 16 & 0xffff), (unsigned)(driver.QuadPart & 0xffff), desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
                    desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ? " (software)" : "");
        void *adapter3 = NULL;
        VideoMemoryInfo local = {0}, shared = {0};
        if (SUCCEEDED(IDXGIAdapter1_QueryInterface(adapter, &dxgi_adapter3_iid, &adapter3))) {
            QueryVideoMemoryFn query = (QueryVideoMemoryFn)(*(void ***)adapter3)[QUERY_VIDEO_MEMORY_SLOT];
            BOOL ok = SUCCEEDED(query(adapter3, 0, 0, &local)) && SUCCEEDED(query(adapter3, 0, 1, &shared));
            if (ok)
                text_printf(t, "  video memory: local budget %llu MB, this process uses %llu MB; shared budget %llu MB, uses %llu MB\n", local.budget >> 20,
                            local.usage >> 20, shared.budget >> 20, shared.usage >> 20);
            ((IUnknown *)adapter3)->lpVtbl->Release((IUnknown *)adapter3);
        } else {
            text_printf(t, "  video memory: %lu MB dedicated, %lu MB shared (32-bit sizes, may be truncated)\n", (unsigned long)(desc.DedicatedVideoMemory >> 20),
                        (unsigned long)(desc.SharedSystemMemory >> 20));
        }
        IDXGIOutput *output;
        for (UINT o = 0; SUCCEEDED(IDXGIAdapter1_EnumOutputs(adapter, o, &output)); o++) {
            DXGI_OUTPUT_DESC out;
            IDXGIOutput_GetDesc(output, &out);
            DEVMODEW mode = {.dmSize = sizeof mode};
            EnumDisplaySettingsW(out.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
            char device[64];
            to_utf8(out.DeviceName, device, sizeof device);
            static const char *const rotations[] = {"unspecified", "none", "90", "180", "270"};
            text_printf(t, "  output %u: %s, desktop %ld,%ld to %ld,%ld; %lu x %lu at %lu Hz, %lu bpp; rotation %s\n", o, device, out.DesktopCoordinates.left,
                        out.DesktopCoordinates.top, out.DesktopCoordinates.right, out.DesktopCoordinates.bottom, mode.dmPelsWidth, mode.dmPelsHeight,
                        mode.dmDisplayFrequency, mode.dmBitsPerPel, out.Rotation <= 4 ? rotations[out.Rotation] : "other");
            IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
}

static const char *swap_effect_name(DXGI_SWAP_EFFECT effect) {
    switch (effect) {
    case DXGI_SWAP_EFFECT_DISCARD: return "blt discard";
    case DXGI_SWAP_EFFECT_SEQUENTIAL: return "blt sequential";
    case 3: return "flip sequential";
    case 4: return "flip discard";
    default: return "other";
    }
}

static void section_game(Text *t) {
    heading(t, "Game");
    text_printf(t, "Engine: empire.retail.dll at %p, %s\n", (void *)host.engine, host.engine_build);
    BYTE *device = render_device();
    text_printf(t, "Render device: %s\n", device ? "DirectX 11" : "not DirectX 11, or not created yet");
    ID3D11DeviceContext *context = immediate_context();
    if (context) {
        ID3D11Device *d3d;
        ID3D11DeviceContext_GetDevice(context, &d3d);
        D3D11_FEATURE_DATA_THREADING threading = {0};
        ID3D11Device_CheckFeatureSupport(d3d, D3D11_FEATURE_THREADING, &threading, sizeof threading);
        text_printf(t, "D3D11 device: feature level %x, creation flags %x, driver command lists %s, concurrent creates %s\n",
                    (unsigned)ID3D11Device_GetFeatureLevel(d3d), ID3D11Device_GetCreationFlags(d3d), threading.DriverCommandLists ? "yes" : "NO (emulated)",
                    threading.DriverConcurrentCreates ? "yes" : "no");
        IDXGIDevice *dxgi;
        if (SUCCEEDED(ID3D11Device_QueryInterface(d3d, &dxgi_device_iid, (void **)&dxgi))) {
            IDXGIAdapter *adapter;
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter))) {
                DXGI_ADAPTER_DESC desc;
                IDXGIAdapter_GetDesc(adapter, &desc);
                char name[256];
                to_utf8(desc.Description, name, sizeof name);
                text_printf(t, "  on adapter %s (LUID %08lx:%08lx)\n", name, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
                IDXGIAdapter_Release(adapter);
            }
            IDXGIDevice_Release(dxgi);
        }
        ID3D11Device_Release(d3d);
    }
    IDXGISwapChain *chain = swap_chain();
    if (chain) {
        DXGI_SWAP_CHAIN_DESC desc;
        BOOL fullscreen = FALSE;
        IDXGISwapChain_GetDesc(chain, &desc);
        IDXGISwapChain_GetFullscreenState(chain, &fullscreen, NULL);
        text_printf(t, "Swap chain: %u x %u format %d, %u buffers, %s, flags %x, MSAA %u, %s, refresh %u/%u\n", desc.BufferDesc.Width, desc.BufferDesc.Height,
                    (int)desc.BufferDesc.Format, desc.BufferCount, swap_effect_name(desc.SwapEffect), desc.Flags, desc.SampleDesc.Count,
                    fullscreen ? "exclusive fullscreen" : desc.Windowed ? "windowed" : "fullscreen", desc.BufferDesc.RefreshRate.Numerator,
                    desc.BufferDesc.RefreshRate.Denominator);
        HWND window = desc.OutputWindow;
        RECT client, frame;
        if (window && GetClientRect(window, &client) && GetWindowRect(window, &frame))
            text_printf(t, "Window: client %ld x %ld, frame %ld,%ld to %ld,%ld, style %08lx, ex style %08lx\n", client.right, client.bottom, frame.left, frame.top,
                        frame.right, frame.bottom, (unsigned long)GetWindowLongW(window, GWL_STYLE), (unsigned long)GetWindowLongW(window, GWL_EXSTYLE));
    }
    wchar_t appdata[MAX_PATH], path[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        swprintf(path, MAX_PATH, L"%ls%ls", appdata, PREFERENCES_PATH);
        text_printf(t, "Game settings: %s\n", copy_text_file(path, "preferences.script.txt") ? "copied as preferences.script.txt" : "not found");
    }
}

static BOOL is_fix_switch(const char *name, const char *log) {
    char line[96];
    snprintf(line, sizeof line, "module %s.dll: installed", name);
    return strstr(log, line) != NULL;
}

static void section_s2fix(Text *t, const char *log, FixResult *fixes, int *fix_count, LONG *initial_states) {
    heading(t, "s2fix");
    text_printf(t, "Version %s, run %s, Apply Fixes %s\n", host.version, host.run_id, host.switch_count() && *host.switch_at(0).flag ? "ON" : "OFF");
    text_printf(t, "s2prof.ini: %s\n", copy_text_file(host.config_path, "s2prof.ini") ? "copied as s2prof.ini" : "not found");
    text_printf(t, "Fixes as the host installed them:\n");
    for (const char *at = log; (at = strstr(at, "module ")) != NULL;) {
        const char *end = strchr(at, '\n');
        size_t length = end ? (size_t)(end - at) : strlen(at);
        text_printf(t, "  %.*s\n", (int)length, at);
        at += length;
    }
    text_printf(t, "Switches at the start (live A/B per fix covers the switches named after an installed fix):\n");
    *fix_count = 0;
    for (int i = 0; i < host.switch_count(); i++) {
        ReportSwitch s = host.switch_at(i);
        initial_states[i] = *s.flag;
        BOOL fix = i > 0 && is_fix_switch(s.name, log);
        text_printf(t, "  %-20s %s%s\n", s.name, *s.flag ? "ON" : "OFF", fix ? "  (a fix)" : i == 0 ? "  (all fixes)" : "");
        if (fix && *fix_count < MAX_FIXES) {
            FixResult *r = &fixes[(*fix_count)++];
            memset(r, 0, sizeof *r);
            snprintf(r->name, sizeof r->name, "%s", s.name);
        }
    }
}

static void section_process(Text *t) {
    heading(t, "Process");
    HANDLE process = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX counters = {.cb = sizeof counters};
    if (GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&counters, sizeof counters))
        text_printf(t, "Memory: working set %.0f MB (peak %.0f), private %.0f MB, page faults %lu\n", counters.WorkingSetSize / 1048576.0,
                    counters.PeakWorkingSetSize / 1048576.0, counters.PrivateUsage / 1048576.0, counters.PageFaultCount);
    // The game is 32-bit and large-address-aware: how much of its 4 GB is taken, and the largest free block left.
    SIZE_T committed = 0, reserved = 0, free_bytes = 0, largest_free = 0, regions = 0;
    MEMORY_BASIC_INFORMATION region;
    for (BYTE *at = NULL; VirtualQuery(at, &region, sizeof region) == sizeof region; at = (BYTE *)region.BaseAddress + region.RegionSize) {
        regions++;
        if (region.State == MEM_COMMIT) committed += region.RegionSize;
        else if (region.State == MEM_RESERVE) reserved += region.RegionSize;
        else {
            free_bytes += region.RegionSize;
            if (region.RegionSize > largest_free) largest_free = region.RegionSize;
        }
        if ((uintptr_t)region.BaseAddress + region.RegionSize < (uintptr_t)region.BaseAddress) break;
    }
    text_printf(t, "Address space: %.0f MB committed, %.0f MB reserved, %.0f MB free (largest free block %.0f MB), %lu regions\n", committed / 1048576.0,
                reserved / 1048576.0, free_bytes / 1048576.0, largest_free / 1048576.0, (unsigned long)regions);
    DWORD handles = 0;
    GetProcessHandleCount(process, &handles);
    DWORD_PTR process_mask, system_mask;
    GetProcessAffinityMask(process, &process_mask, &system_mask);
    text_printf(t, "Handles %lu, priority class %lx, affinity %lx of %lx\n", handles, GetPriorityClass(process), (unsigned long)process_mask, (unsigned long)system_mask);
    GetProcessCpuSetsFn process_sets = (GetProcessCpuSetsFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessDefaultCpuSets");
    ULONG ids[256], required = 0;
    if (process_sets && process_sets(process, ids, 256, &required)) {
        text_printf(t, "Default CPU sets (%lu):", required);
        for (ULONG i = 0; i < required && i < 256; i++) text_printf(t, " %lu", ids[i]);
        text_printf(t, "%s\n", required ? "" : " none (all CPUs)");
    }
    QueryTimerResolutionFn timer = (QueryTimerResolutionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution");
    ULONG coarsest, finest, current;
    if (timer && !timer(&coarsest, &finest, &current))
        text_printf(t, "Timer resolution: %.3f ms now (finest %.3f, coarsest %.3f)\n", current / 10000.0, finest / 10000.0, coarsest / 10000.0);
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER k = {.LowPart = kernel.dwLowDateTime, .HighPart = kernel.dwHighDateTime}, u = {.LowPart = user.dwLowDateTime, .HighPart = user.dwHighDateTime};
        text_printf(t, "CPU time: %.1f s user, %.1f s kernel\n", u.QuadPart / 1e7, k.QuadPart / 1e7);
    }
}

static void section_modules(Text *t) {
    HMODULE modules[1024];
    DWORD needed = 0;
    EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed);
    int count = (int)(needed / sizeof *modules);
    if (count > 1024) count = 1024;
    char title[64];
    snprintf(title, sizeof title, "Loaded modules (%d)", count);
    heading(t, title);
    for (int i = 0; i < count; i++) {
        wchar_t path[MAX_PATH];
        MODULEINFO info = {0};
        if (!GetModuleFileNameExW(GetCurrentProcess(), modules[i], path, MAX_PATH)) continue;
        GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof info);
        char utf8[MAX_PATH * 3], version[512];
        to_utf8(path, utf8, sizeof utf8);
        file_version(path, version, sizeof version);
        text_printf(t, "  %p %8lu KB  %s  [%s]\n", info.lpBaseOfDll, info.SizeOfImage / 1024, utf8, version[0] ? version : "no version");
    }
}

static void section_threads(Text *t) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    GetThreadDescriptionFn describe = (GetThreadDescriptionFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    GetThreadCpuSetsFn thread_sets = (GetThreadCpuSetsFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadSelectedCpuSets");
    Text rows = {0};
    int count = 0;
    THREADENTRY32 entry = {.dwSize = sizeof entry};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        count++;
        char name[128] = "";
        PWSTR description = NULL;
        if (describe && SUCCEEDED(describe(thread, &description)) && description) {
            to_utf8(description, name, sizeof name);
            LocalFree(description);
        }
        PVOID start = NULL;
        NtQueryInformationThread(thread, (THREADINFOCLASS)9, &start, sizeof start, NULL);
        struct {
            LONG exit_status;
            PVOID teb;
            HANDLE process_id, thread_id;
            ULONG_PTR affinity;
            LONG priority, base_priority;
        } basic = {0};
        NtQueryInformationThread(thread, (THREADINFOCLASS)0, &basic, sizeof basic, NULL);
        HMODULE owner = NULL;
        char where[MAX_PATH * 3] = "?";
        if (start && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)start, &owner)) {
            wchar_t module[MAX_PATH];
            GetModuleFileNameW(owner, module, MAX_PATH);
            const wchar_t *base = wcsrchr(module, L'\\') ? wcsrchr(module, L'\\') + 1 : module;
            char utf8[MAX_PATH * 3];
            to_utf8(base, utf8, sizeof utf8);
            snprintf(where, sizeof where, "%s+0x%lx", utf8, (unsigned long)((BYTE *)start - (BYTE *)owner));
        }
        FILETIME created, exited, kernel, user;
        double cpu = 0;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user))
            cpu = (((ULONGLONG)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) + ((ULONGLONG)user.dwHighDateTime << 32 | user.dwLowDateTime)) / 1e7;
        char sets[256] = "";
        ULONG ids[32], required = 0;
        if (thread_sets && thread_sets(thread, ids, 32, &required) && required) {
            int used = 0;
            for (ULONG i = 0; i < required && i < 32 && used < (int)sizeof sets - 12; i++) used += snprintf(sets + used, sizeof sets - (size_t)used, "%s%lu", i ? "," : "", ids[i]);
        }
        text_printf(&rows, "  %6lu  prio %2ld (relative %+3ld)  mask %8lx  cpu %8.2f s  sets %-12s  %-28s %s\n", entry.th32ThreadID, basic.priority, basic.base_priority,
                    (unsigned long)basic.affinity, cpu, sets[0] ? sets : "-", where, name);
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    char title[128];
    snprintf(title, sizeof title, "Threads (%d): id, priority, CPU mask, CPU time, CPU sets, start, name", count);
    heading(t, title);
    if (rows.data) text_append(t, rows.data, rows.length);
    free(rows.data);
}

static void section_folder(Text *t) {
    heading(t, "Game folder (top level)");
    wchar_t exe[MAX_PATH], pattern[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    *wcsrchr(exe, L'\\') = 0;
    swprintf(pattern, MAX_PATH, L"%ls\\*", exe);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        char name[MAX_PATH * 3], version[512] = "";
        to_utf8(found.cFileName, name, sizeof name);
        BOOL directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        size_t length = wcslen(found.cFileName);
        if (!directory && length > 4 && (!_wcsicmp(found.cFileName + length - 4, L".dll") || !_wcsicmp(found.cFileName + length - 4, L".exe") ||
                                         !_wcsicmp(found.cFileName + length - 4, L".asi"))) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\%ls", exe, found.cFileName);
            file_version(path, version, sizeof version);
        }
        SYSTEMTIME written;
        FileTimeToSystemTime(&found.ftLastWriteTime, &written);
        text_printf(t, "  %s %10llu  %04u-%02u-%02u  %s%s%s%s\n", directory ? "<dir>" : "     ", (unsigned long long)found.nFileSizeHigh << 32 | found.nFileSizeLow,
                    written.wYear, written.wMonth, written.wDay, name, version[0] ? "  [" : "", version, version[0] ? "]" : "");
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

// ---- the zip ----

static uint32_t crc_table[256];

static void make_crc_table(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

typedef struct {
    char name[MAX_PATH];
    uint32_t crc, size, offset;
} ZipEntry;

static void put16(BYTE *at, uint16_t value) {
    at[0] = (BYTE)value, at[1] = (BYTE)(value >> 8);
}

static void put32(BYTE *at, uint32_t value) {
    put16(at, (uint16_t)value), put16(at + 2, (uint16_t)(value >> 16));
}

// Stored, not deflated: the screenshots are compressed already and the rest is small.
static BOOL write_zip(const wchar_t *zip_path, const wchar_t *source_folder, int *file_count) {
    ZipEntry *entries = calloc(512, sizeof *entries);
    HANDLE out = CreateFileW(zip_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!entries || out == INVALID_HANDLE_VALUE) {
        free(entries);
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        return FALSE;
    }
    SYSTEMTIME local;
    GetLocalTime(&local);
    uint16_t dos_time = (uint16_t)(local.wHour << 11 | local.wMinute << 5 | local.wSecond / 2),
             dos_date = (uint16_t)((local.wYear - 1980) << 9 | local.wMonth << 5 | local.wDay);
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", source_folder);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    uint32_t offset = 0;
    int count = 0;
    BOOL ok = TRUE;
    static BYTE buffer[1 << 20];
    DWORD written;
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || count == 512) continue;
            ZipEntry *e = &entries[count++];
            to_utf8(found.cFileName, e->name, sizeof e->name);
            e->offset = offset;
            uint16_t name_length = (uint16_t)strlen(e->name);
            BYTE header[30] = {0};
            put32(header, 0x04034b50);
            put16(header + 4, 20);
            put16(header + 6, 0x0808);
            put16(header + 10, dos_time);
            put16(header + 12, dos_date);
            put16(header + 26, name_length);
            ok = ok && WriteFile(out, header, sizeof header, &written, NULL) && WriteFile(out, e->name, name_length, &written, NULL);
            offset += sizeof header + name_length;
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\%ls", source_folder, found.cFileName);
            HANDLE in = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            uint32_t crc = 0xffffffffu, size = 0;
            DWORD got;
            while (in != INVALID_HANDLE_VALUE && ReadFile(in, buffer, sizeof buffer, &got, NULL) && got) {
                for (DWORD i = 0; i < got; i++) crc = crc_table[(crc ^ buffer[i]) & 0xff] ^ (crc >> 8);
                ok = ok && WriteFile(out, buffer, got, &written, NULL);
                size += got;
            }
            if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
            e->crc = ~crc;
            e->size = size;
            offset += size;
            BYTE descriptor[16];
            put32(descriptor, 0x08074b50);
            put32(descriptor + 4, e->crc);
            put32(descriptor + 8, size);
            put32(descriptor + 12, size);
            ok = ok && WriteFile(out, descriptor, sizeof descriptor, &written, NULL);
            offset += sizeof descriptor;
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    uint32_t directory_start = offset;
    for (int i = 0; i < count; i++) {
        ZipEntry *e = &entries[i];
        uint16_t name_length = (uint16_t)strlen(e->name);
        BYTE header[46] = {0};
        put32(header, 0x02014b50);
        put16(header + 4, 20);
        put16(header + 6, 20);
        put16(header + 8, 0x0808);
        put16(header + 12, dos_time);
        put16(header + 14, dos_date);
        put32(header + 16, e->crc);
        put32(header + 20, e->size);
        put32(header + 24, e->size);
        put16(header + 28, name_length);
        put32(header + 42, e->offset);
        ok = ok && WriteFile(out, header, sizeof header, &written, NULL) && WriteFile(out, e->name, name_length, &written, NULL);
        offset += sizeof header + name_length;
    }
    BYTE end[22] = {0};
    put32(end, 0x06054b50);
    put16(end + 8, (uint16_t)count);
    put16(end + 10, (uint16_t)count);
    put32(end + 12, offset - directory_start);
    put32(end + 16, directory_start);
    ok = ok && WriteFile(out, end, sizeof end, &written, NULL);
    CloseHandle(out);
    free(entries);
    *file_count = count;
    return ok;
}

static void remove_folder(const wchar_t *path) {
    wchar_t pattern[MAX_PATH], file[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", path);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            swprintf(file, MAX_PATH, L"%ls\\%ls", path, found.cFileName);
            DeleteFileW(file);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    RemoveDirectoryW(path);
}

// ---- the sequence ----

static void set_flag(volatile LONG *flag, LONG value) {
    InterlockedExchange(flag, value);
}

static void wait_ms(DWORD milliseconds) {
    Sleep(milliseconds);
}

static void move_capture_files(int number) {
    const char *suffixes[] = {"threads.tsv", "stacks.txt"};
    for (int i = 0; i < 2; i++) {
        char name[64];
        snprintf(name, sizeof name, "s2prof_%d_%s", number, suffixes[i]);
        wchar_t from[MAX_PATH], to[MAX_PATH];
        host.output_path(from, name);
        char target[96];
        snprintf(target, sizeof target, "capture_%s", suffixes[i]);
        temp_path(to, target);
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    }
}

static void print_ab(Text *t, const Stats *off, const Stats *on) {
    print_stats(t, "off", off);
    print_stats(t, "on", on);
    if (off->count && on->count)
        text_printf(t, "  on - off: mean %+.3f ms, p50 %+.3f, p95 %+.3f, p99 %+.3f; GPU mean %+.3f ms\n", on->mean - off->mean, on->p50 - off->p50,
                    on->p95 - off->p95, on->p99 - off->p99, on->gpu_mean - off->gpu_mean);
}

// Progress is the time spent against the time the sequence is planned to take.
static LONGLONG progress_started, progress_planned;

static void progress(const char *stage) {
    LONGLONG spent = now() - progress_started;
    int percent = progress_planned ? (int)(100 * spent / progress_planned) : 0;
    set_overlay(stage, percent > 99 ? 99 : percent);
}

static DWORD WINAPI report_thread(LPVOID unused) {
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    LONGLONG started = now();
    progress_started = started;
    progress_planned = 0;
    progress("system snapshot");
    MessageBeep(MB_OK);
    host.log_line("debug report: started; keep the camera still until the second beep (about 2 to 3 minutes)");
    Text report = {0}, shot_list = {0};
    static FixResult fixes[MAX_FIXES];
    static LONG initial_states[64];
    int fix_count = 0;
    SYSTEMTIME local;
    GetLocalTime(&local);
    text_printf(&report, "s2fix debug report, Total War: Shogun 2 (2011)\n%04u-%02u-%02u %02u:%02u:%02u, s2fix %s, run %s\n", local.wYear, local.wMonth, local.wDay,
                local.wHour, local.wMinute, local.wSecond, host.version, host.run_id);
    text_printf(&report, "The player kept the camera still. Times are frame-to-frame intervals at the engine's present; GPU times are D3D11\n"
                         "timestamp queries around each frame on the game's immediate context. A/B frames within %d ms of a switch are left out.\n",
                SKIP_AFTER_FLIP_MS);

    Text log = {0};
    wchar_t log_path[MAX_PATH];
    host.output_path(log_path, "s2prof.log");
    read_file(log_path, &log);
    if (!log.data) text_append(&log, "", 0);
    section_system(&report);
    section_gpu(&report);
    section_game(&report);
    section_s2fix(&report, log.data ? log.data : "", fixes, &fix_count, initial_states);
    section_process(&report);
    section_modules(&report);
    section_threads(&report);
    section_folder(&report);
    free(log.data);
    host.log_line("debug report: system snapshot done");
    int live_fixes = 0;
    for (int f = 0; f < fix_count; f++)
        for (int i = 0; i < host.switch_count(); i++)
            if (!strcmp(host.switch_at(i).name, fixes[f].name) && *host.switch_at(i).flag) live_fixes++;
    LONGLONG planned_ms = (now() - started) * 1000 / frequency.QuadPart + 3 * SETTLE_MS + 2 * 600 + BASELINE_MS + ALL_AB_SECONDS * 1000 + 300 +
                          (LONGLONG)live_fixes * (SETTLE_MS + 400 + FIX_BLOCKS * FIX_BLOCK_MS + 250) + 4000;
    progress_planned = planned_ms * frequency.QuadPart / 1000;
    progress("screenshots, all fixes on");

    ReportSwitch all = host.switch_at(0);
    LONG all_before = *all.flag;
    set_flag(all.flag, 1);
    wait_ms(SETTLE_MS);
    take_burst(&shot_list, "all_fixes_on", BURST, TRUE);
    LONG first;
    progress("baseline, all fixes on");
    start_recording(NULL, &first);
    wait_ms(BASELINE_MS);
    LONG last = stop_recording();
    Stats baseline;
    frame_stats(first, last, -1, &baseline);
    heading(&report, "Baseline: all fixes on, 10 s");
    print_stats(&report, "all", &baseline);
    host.log_line("debug report: baseline done");

    progress("screenshots, all fixes off");
    set_flag(all.flag, 0);
    wait_ms(SETTLE_MS);
    take_burst(&shot_list, "all_fixes_off", BURST, TRUE);
    set_flag(all.flag, 1);
    progress("all fixes on/off");
    wait_ms(SETTLE_MS);

    start_recording(all.flag, &first);
    int capture = host.capture(ALL_AB_SECONDS, 0);
    last = stop_recording();
    set_flag(all.flag, 1);
    if (capture) move_capture_files(capture);
    Stats off, on;
    frame_stats(first, last, 0, &off);
    frame_stats(first, last, 1, &on);
    heading(&report, "All fixes on vs off: 20 s, switching every 2 s, with the stack profile (capture_threads.tsv, capture_stacks.txt)");
    print_ab(&report, &off, &on);
    host.log_line("debug report: all-fixes A/B done; now each fix, %d of them", fix_count);

    for (int f = 0; f < fix_count; f++) {
        FixResult *r = &fixes[f];
        int index = -1;
        for (int i = 0; i < host.switch_count(); i++)
            if (!strcmp(host.switch_at(i).name, r->name)) index = i;
        if (index < 0) continue;
        ReportSwitch s = host.switch_at(index);
        LONG before = *s.flag;
        if (!before) {
            r->skipped = TRUE;
            snprintf(r->note, sizeof r->note, "off at the start, left off");
            continue;
        }
        progress(r->name);
        if (s.report) s.report(TRUE);
        set_flag(s.flag, 0);
        wait_ms(SETTLE_MS);
        char state[96];
        snprintf(state, sizeof state, "%.63s_off", r->name);
        take_burst(&shot_list, state, 1, FALSE);
        start_recording(s.flag, &first);
        for (int b = 0; b < FIX_BLOCKS; b++) {
            set_flag(s.flag, b % 2);
            wait_ms(FIX_BLOCK_MS);
        }
        last = stop_recording();
        set_flag(s.flag, before);
        if (s.report) s.report(FALSE);
        frame_stats(first, last, 0, &r->off);
        frame_stats(first, last, 1, &r->on);
        wait_ms(100);
    }
    for (int i = 0; i < host.switch_count(); i++) set_flag(host.switch_at(i).flag, initial_states[i]);
    set_flag(all.flag, all_before);

    heading(&report, "Each fix on vs off: 4 blocks of 1 s (off, on, off, on); a screenshot with the fix off (shot_<fix>_off_1.jpg)");
    text_printf(&report, "  %-16s %7s %7s %9s %9s %9s %9s %9s %9s %8s %8s\n", "fix", "frames", "frames", "mean off", "mean on", "on-off", "p99 off", "p99 on",
                "p99 on-off", "GPU off", "GPU on");
    for (int f = 0; f < fix_count; f++) {
        FixResult *r = &fixes[f];
        if (r->skipped) {
            text_printf(&report, "  %-16s %s\n", r->name, r->note);
            continue;
        }
        text_printf(&report, "  %-16s %7d %7d %9.3f %9.3f %+9.3f %9.3f %9.3f %+9.3f %8.3f %8.3f\n", r->name, r->off.count, r->on.count, r->off.mean, r->on.mean,
                    r->on.mean - r->off.mean, r->off.p99, r->on.p99, r->on.p99 - r->off.p99, r->off.gpu_mean, r->on.gpu_mean);
    }
    heading(&report, "Screenshots (the game's own image, taken before Present)");
    if (shot_list.data) text_append(&report, shot_list.data, shot_list.length);
    heading(&report, "Files");
    text_printf(&report, "  report.txt (this), s2prof.log (the host's log up to the end of the report), s2prof.ini, preferences.script.txt,\n"
                         "  capture_threads.tsv and capture_stacks.txt (the all-fixes A/B's per-thread CPU and stacks), proxy.log, crash reports if any,\n"
                         "  and the screenshots. User and computer names are replaced.\n");
    text_printf(&report, "\nReport took %.0f s.\n", ms(now() - started) / 1000.0);

    progress("packing the report");
    redact(&report);
    wchar_t path[MAX_PATH];
    temp_path(path, "report.txt");
    write_file(path, report.data, report.length);
    free(report.data);
    free(shot_list.data);
    host.output_path(path, "s2prof.log");
    copy_text_file(path, "s2prof.log");
    swprintf(path, MAX_PATH, L"%ls\\proxy.log", host.output_dir);
    copy_text_file(path, "proxy.log");
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*crash*", host.output_dir);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char name[MAX_PATH * 3];
            to_utf8(found.cFileName, name, sizeof name);
            swprintf(path, MAX_PATH, L"%ls\\%ls", host.output_dir, found.cFileName);
            copy_text_file(path, name);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }

    wchar_t zip[MAX_PATH];
    swprintf(zip, MAX_PATH, L"%ls\\report_%hs.zip", host.output_dir, stamp);
    int files = 0;
    BOOL zipped = write_zip(zip, folder, &files);
    if (zipped) remove_folder(folder);
    RemoveDirectoryW(temp_root);
    host.log_line(zipped ? "debug report: done in %.0f s -> perf-logs\\report_%s.zip (%d files)" : "debug report: FAILED to write perf-logs\\report_%s.zip; the files stay in perf-logs\\temp",
                  ms(now() - started) / 1000.0, stamp, files);
    CoUninitialize();
    // The open GPU query is ended on main before the hooks stop running.
    gpu_stop = 1;
    for (int waited = 0; !gpu_stopped && waited < 500; waited += 10) Sleep(10);
    MessageBeep(zipped ? MB_ICONASTERISK : MB_ICONHAND);
    running = 0;
    release_wanted = 1;
    return 0;
}

void report_init(const ReportHost *report_host) {
    host = *report_host;
    QueryPerformanceFrequency(&frequency);
    make_crc_table();
    wchar_t wide[MAX_PATH];
    DWORD size = MAX_PATH;
    if (GetEnvironmentVariableW(L"USERPROFILE", wide, MAX_PATH)) to_utf8(wide, profile_utf8, sizeof profile_utf8);
    if (GetUserNameW(wide, &size)) to_utf8(wide, user_utf8, sizeof user_utf8);
    size = MAX_PATH;
    if (GetComputerNameW(wide, &size)) to_utf8(wide, computer_utf8, sizeof computer_utf8);
}

BOOL report_running(void) {
    return running != 0;
}

void report_start(void) {
    if (InterlockedCompareExchange(&running, 1, 0)) return;
    if (!frames && !(frames = malloc(MAX_FRAMES * sizeof *frames))) {
        host.log_line("debug report: out of memory for the frame records");
        running = 0;
        return;
    }
    frame_count = 0;
    overlay_version = 0;
    gpu_stop = gpu_stopped = 0;
    current_gpu_set = -1;
    SYSTEMTIME local;
    GetLocalTime(&local);
    snprintf(stamp, sizeof stamp, "%04u%02u%02u-%02u%02u%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
    swprintf(temp_root, MAX_PATH, L"%ls\\temp", host.output_dir);
    swprintf(folder, MAX_PATH, L"%ls\\report_%hs", temp_root, stamp);
    CreateDirectoryW(host.output_dir, NULL);
    CreateDirectoryW(temp_root, NULL);
    if (!CreateDirectoryW(folder, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        host.log_line("debug report: cannot create perf-logs\\temp\\report_%s (error %lu)", stamp, GetLastError());
        running = 0;
        return;
    }
    HANDLE thread = CreateThread(NULL, 0, report_thread, NULL, 0, NULL);
    if (!thread) {
        host.log_line("debug report: CreateThread failed");
        running = 0;
        return;
    }
    CloseHandle(thread);
}
