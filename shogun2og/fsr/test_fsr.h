// What s2fsr.dll's two offline tests share: the exports, devices, texture data, and the engine's camera layout.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "s2fsr.h"

struct Fsr {
    S2FsrInit init;
    S2FsrCreate create;
    S2FsrFrameFn frame;
    S2FsrRelease release;
    S2FsrShutdown shutdown;
    S2FsrTextures textures;
    S2FsrOpaque opaque;
};

inline bool load_fsr(const char *path, Fsr *f) {
    HMODULE lib = LoadLibraryA(path);
    if (!lib) return false;
    f->init = (S2FsrInit)(void *)GetProcAddress(lib, "s2fsr_init");
    f->create = (S2FsrCreate)(void *)GetProcAddress(lib, "s2fsr_create");
    f->frame = (S2FsrFrameFn)(void *)GetProcAddress(lib, "s2fsr_frame");
    f->release = (S2FsrRelease)(void *)GetProcAddress(lib, "s2fsr_release");
    f->shutdown = (S2FsrShutdown)(void *)GetProcAddress(lib, "s2fsr_shutdown");
    f->textures = (S2FsrTextures)(void *)GetProcAddress(lib, "s2fsr_textures");
    f->opaque = (S2FsrOpaque)(void *)GetProcAddress(lib, "s2fsr_opaque");
    return f->init && f->create && f->frame && f->release && f->shutdown && f->textures && f->opaque;
}

inline void log_line(const char *line) { printf("    log: %s\n", line); }

// With the debug layer where it is installed, so its errors count too.
inline ID3D11Device *make_device(D3D_DRIVER_TYPE type, D3D_FEATURE_LEVEL level, ID3D11DeviceContext **context) {
    ID3D11Device *d = NULL;
    for (UINT flags : {(UINT)D3D11_CREATE_DEVICE_DEBUG, 0u})
        if (SUCCEEDED(D3D11CreateDevice(NULL, type, NULL, flags, &level, 1, D3D11_SDK_VERSION, &d, NULL, context))) {
            ID3D11InfoQueue *q = NULL;
            if (SUCCEEDED(d->QueryInterface(__uuidof(ID3D11InfoQueue), (void **)&q))) {
                D3D11_MESSAGE_SEVERITY quiet[] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
                D3D11_INFO_QUEUE_FILTER filter = {};
                filter.DenyList.NumSeverities = 2, filter.DenyList.pSeverityList = quiet;
                q->AddStorageFilterEntries(&filter);
                q->SetMessageCountLimit((UINT64)-1);
                q->Release();
            }
            return d;
        }
    return NULL;
}

inline void device_name(ID3D11Device *d, char *name, size_t size) {
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc = {};
    d->QueryInterface(__uuidof(IDXGIDevice), (void **)&dx);
    dx->GetAdapter(&adapter);
    adapter->GetDesc(&desc);
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, (int)size, NULL, NULL);
    adapter->Release();
    dx->Release();
}

// The debug layer's errors since the last call (-1 without the layer), each kind of message printed once; it forgets them.
inline int debug_errors(ID3D11Device *d) {
    ID3D11InfoQueue *q = NULL;
    if (FAILED(d->QueryInterface(__uuidof(ID3D11InfoQueue), (void **)&q))) return -1;
    int errors = 0;
    std::vector<D3D11_MESSAGE_ID> seen;
    std::vector<int> counts;
    for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; i++) {
        SIZE_T size = 0;
        q->GetMessage(i, NULL, &size);
        std::vector<char> buffer(size);
        D3D11_MESSAGE *m = (D3D11_MESSAGE *)buffer.data();
        q->GetMessage(i, m, &size);
        errors += m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR;
        size_t k = 0;
        while (k < seen.size() && seen[k] != m->ID) k++;
        if (k == seen.size()) {
            seen.push_back(m->ID), counts.push_back(0);
            printf("    debug layer %s: %.*s\n", m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR ? "error" : "warning",
                   (int)m->DescriptionByteLength, m->pDescription);
        }
        counts[k]++;
    }
    for (size_t k = 0; k < seen.size(); k++)
        if (counts[k] > 1) printf("    debug layer: message %d %d times\n", (int)seen[k], counts[k]);
    q->ClearStoredMessages();
    q->Release();
    return errors;
}

inline ULONG refs(IUnknown *u) {
    u->AddRef();
    return u->Release();
}

inline ID3D11Texture2D *texture(ID3D11Device *d, int w, int h, DXGI_FORMAT format, UINT bind,
                                D3D11_USAGE usage = D3D11_USAGE_DEFAULT) {
    D3D11_TEXTURE2D_DESC t = {};
    t.Width = w, t.Height = h, t.MipLevels = 1, t.ArraySize = 1, t.Format = format, t.SampleDesc.Count = 1;
    t.BindFlags = bind, t.Usage = usage;
    if (usage == D3D11_USAGE_STAGING) t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *tex = NULL;
    d->CreateTexture2D(&t, NULL, &tex);
    return tex;
}

inline unsigned short to_half(float f) {
    unsigned u;
    memcpy(&u, &f, 4);
    unsigned sign = (u >> 16) & 0x8000, e = (u >> 23) & 0xff, m = u & 0x7fffff;
    if (e < 113) return (unsigned short)sign;
    if (e > 142) return (unsigned short)(sign | 0x7c00);
    return (unsigned short)(sign | ((e - 112) << 10) | ((m + 0x1000) >> 13));
}

inline float from_half(unsigned short h) {
    unsigned e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e == 31 ? (m ? NAN : INFINITY) : e ? ldexpf((float)(m | 0x400), (int)e - 25) : ldexpf((float)m, -24);
    return h & 0x8000 ? -v : v;
}

// R11G11B10_FLOAT's channels: unsigned, 5 exponent bits (bias 15) and 6 or 5 mantissa bits.
inline unsigned to_small_float(float f, int mantissa) {
    if (!(f > 0)) return 0;
    int e;
    float m = frexpf(f, &e);
    int biased = e + 14;
    unsigned fraction = (unsigned)lroundf((m * 2 - 1) * (float)(1 << mantissa));
    if (fraction == 1u << mantissa) fraction = 0, biased++;
    return biased <= 0 ? 0 : (unsigned)biased << mantissa | fraction;
}

inline float from_small_float(unsigned v, int mantissa) {
    unsigned e = v >> mantissa, m = v & ((1u << mantissa) - 1);
    if (e == 31) return m ? NAN : INFINITY;
    return e ? ldexpf(1 + (float)m / (float)(1 << mantissa), (int)e - 15) : ldexpf((float)m / (float)(1 << mantissa), -14);
}

// A grey image into an R11G11B10_FLOAT or R16G16B16A16_FLOAT texture.
inline void upload(ID3D11DeviceContext *c, ID3D11Texture2D *t, const std::vector<float> &grey) {
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    if (d.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
        std::vector<unsigned> pixels(grey.size());
        for (size_t i = 0; i < grey.size(); i++)
            pixels[i] = to_small_float(grey[i], 6) | to_small_float(grey[i], 6) << 11 | to_small_float(grey[i], 5) << 22;
        c->UpdateSubresource(t, 0, NULL, pixels.data(), d.Width * 4, 0);
    } else {
        std::vector<unsigned short> pixels(grey.size() * 4);
        for (size_t i = 0; i < grey.size(); i++)
            pixels[i * 4] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = to_half(grey[i]), pixels[i * 4 + 3] = to_half(1);
        c->UpdateSubresource(t, 0, NULL, pixels.data(), d.Width * 8, 0);
    }
}

// The texture's bytes, rows packed.
inline std::vector<unsigned char> read_bytes(ID3D11Device *dev, ID3D11DeviceContext *c, ID3D11Texture2D *t) {
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    UINT pixel = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    ID3D11Texture2D *staging = texture(dev, d.Width, d.Height, d.Format, 0, D3D11_USAGE_STAGING);
    c->CopyResource(staging, t);
    D3D11_MAPPED_SUBRESOURCE m;
    c->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    std::vector<unsigned char> bytes((size_t)d.Width * d.Height * pixel);
    for (UINT y = 0; y < d.Height; y++)
        memcpy(&bytes[(size_t)y * d.Width * pixel], (const char *)m.pData + (size_t)y * m.RowPitch, d.Width * pixel);
    c->Unmap(staging, 0);
    staging->Release();
    return bytes;
}

// One channel of every pixel, for R11G11B10_FLOAT, R16G16B16A16_FLOAT and R16G16_FLOAT.
inline std::vector<float> read_channel(ID3D11Device *dev, ID3D11DeviceContext *c, ID3D11Texture2D *t, int channel) {
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    std::vector<unsigned char> bytes = read_bytes(dev, c, t);
    std::vector<float> out((size_t)d.Width * d.Height);
    for (size_t i = 0; i < out.size(); i++) {
        if (d.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
            unsigned v;
            memcpy(&v, &bytes[i * 4], 4);
            out[i] = channel == 2 ? from_small_float(v >> 22, 5) : from_small_float((v >> (11 * channel)) & 0x7ff, 6);
        } else {
            unsigned short h;
            memcpy(&h, &bytes[i * (d.Format == DXGI_FORMAT_R16G16_FLOAT ? 4 : 8) + channel * 2], 2);
            out[i] = from_half(h);
        }
    }
    return out;
}

inline float halton(int i, int base) {
    float f = 1, r = 0;
    for (; i > 0; i /= base) f /= base, r += f * (i % base);
    return r;
}

// The game module's sequence: Halton(2,3) over 32 phases, centred on the pixel.
inline void jitter_of(int frame, float *x, float *y) {
    *x = halton(frame % 32 + 1, 2) - 0.5f;
    *y = halton(frame % 32 + 1, 3) - 0.5f;
}

// As the game module adds the jitter to the engine's projection.
inline void add_jitter(float *m, float jx, float jy, int w, int h) {
    float a = 2 * jx / w, b = -2 * jy / h;
    for (int i = 0; i < 4; i++) m[i] += a * m[12 + i], m[4 + i] += b * m[12 + i];
}

// The engine's layout: row-major M[r][c] for clip = M * v; depth 0 at near, 1 at far. The camera looks down +z, y up.
struct Camera {
    double fov_y, aspect, near_z, far_z;
    double p00() const { return 1 / (aspect * tan(fov_y / 2)); }
    double p11() const { return 1 / tan(fov_y / 2); }
    void view_projection(double x, double y, double z, float *m) const {
        double a = far_z / (far_z - near_z), b = -near_z * far_z / (far_z - near_z);
        double r[16] = {p00(), 0, 0, -p00() * x, 0, p11(), 0, -p11() * y, 0, 0, a, b - a * z, 0, 0, 1, -z};
        for (int i = 0; i < 16; i++) m[i] = (float)r[i];
    }
    double depth(double view_z) const { return far_z / (far_z - near_z) * (1 - near_z / view_z); }
    double view_z(double depth) const { return near_z * far_z / (far_z - depth * (far_z - near_z)); }
};

// A checker of 3-unit cells turned 20 degrees, two levels.
static const float checker_levels[2] = {0.05f, 0.8f};

inline int checker_cell(double u, double v) {
    static const double c = cos(0.349), s = sin(0.349);
    double a = (u * c - v * s) / 3, b = (u * s + v * c) / 3;
    return ((long long)floor(a) + (long long)floor(b)) & 1;
}

inline float checker(double u, double v) {
    return checker_levels[checker_cell(u, v)];
}
