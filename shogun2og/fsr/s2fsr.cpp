// s2fsr.dll: AMD FSR 3.1's upscaler and the camera motion-vector pass on D3D11 behind s2fsr.h's interface.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>
#include <utility>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_resources.h>
#include "s2fsr.h"
#include "motion_cs.h"
#include "mask_cs.h"

#define EXPORT extern "C" __declspec(dllexport)

struct MotionConstants {
    float previous_from_jittered[16];
    float size[2], jitter[2];
    UINT objects, pad[3];
};

struct CombineConstants {
    UINT size[2], generated, pad;
};

struct Failure {
    FfxErrorCode code;
};

static S2FsrLog log_sink;
static ID3D11Device *device;
static void *scratch;
static FfxInterface backend;
static ID3D11ComputeShader *motion_shader, *mask_shader;
static ID3D11Buffer *motion_constants, *mask_constants;
static ID3D11SamplerState *linear_clamp;
static FfxFsr3UpscalerContext fsr;
static bool created;
static ID3D11Texture2D *color, *depth, *velocity, *stamp, *mask, *target, *output, *motion, *opaque, *reactive, *combined;
// What the last frame gave FSR as its reactive mask, for diagnostics.
static ID3D11Texture2D *given_reactive;
// FSR 3.1 leaves these three to its caller to make.
static ID3D11Texture2D *shared[3];
static ID3D11ShaderResourceView *depth_view, *velocity_view, *stamp_view, *reactive_view, *mask_view;
static ID3D11UnorderedAccessView *motion_view, *combined_view;
static UINT width, height, output_width, output_height;
static float sharpening;

static void say_list(const char *format, va_list args) {
    if (!log_sink) return;
    char line[1024];
    vsnprintf(line, sizeof line, format, args);
    log_sink(line);
}

static void say(const char *format, ...) {
    va_list args;
    va_start(args, format);
    say_list(format, args);
    va_end(args);
}

[[noreturn]] static void fail(FfxErrorCode code, const char *format, ...) {
    va_list args;
    va_start(args, format);
    say_list(format, args);
    va_end(args);
    throw Failure{code};
}

static void check(HRESULT hr, const char *what) {
    if (FAILED(hr)) fail(FFX_ERROR_BACKEND_API_ERROR, "s2fsr: %s failed (0x%08lx)", what, hr);
}

static void check_ffx(FfxErrorCode e, const char *what) {
    if (e != FFX_OK) fail(e, "s2fsr: %s failed (0x%08x)", what, e);
}

// The backend throws on a failed D3D call (a lost device, no memory), and nothing may cross into the GCC caller.
template <typename Body> static int guarded(const char *what, Body body) {
    try {
        body();
        return FFX_OK;
    } catch (const Failure &f) {
        return f.code;
    } catch (...) {
        say("s2fsr: %s: a Direct3D call in FSR's backend failed", what);
        return FFX_ERROR_BACKEND_API_ERROR;
    }
}

template <typename T> static void drop(T *&p) {
    if (p) p->Release(), p = NULL;
}

EXPORT void s2fsr_release(void) {
    guarded("release", [] {
        if (created) ffxFsr3UpscalerContextDestroy(&fsr);
        created = false;
        for (ID3D11Texture2D *&t : shared) drop(t);
        drop(depth_view), drop(velocity_view), drop(stamp_view), drop(motion_view), drop(motion), drop(output);
        drop(reactive_view), drop(mask_view), drop(combined_view), drop(combined), drop(mask);
        drop(opaque), drop(reactive), drop(target), drop(stamp), drop(velocity), drop(depth), drop(color);
        given_reactive = NULL;
    });
}

EXPORT void s2fsr_shutdown(void) {
    s2fsr_release();
    drop(motion_shader), drop(motion_constants), drop(mask_shader), drop(mask_constants), drop(linear_clamp);
    free(scratch);
    scratch = NULL, device = NULL;
}

EXPORT int s2fsr_init(void *d3d_device, S2FsrLog log) {
    s2fsr_shutdown();
    log_sink = log;
    device = (ID3D11Device *)d3d_device;
    int e = guarded("init", [] {
        // The luma pyramid pass binds more UAVs than feature level 11_0 allows.
        if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1)
            fail(FFX_ERROR_BACKEND_API_ERROR, "s2fsr: FSR needs a Direct3D 11.1 device; this one is level %x",
                 device->GetFeatureLevel());
        size_t size = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
        scratch = calloc(1, size);
        check_ffx(ffxGetInterfaceDX11(&backend, ffxGetDeviceDX11(device), scratch, size, FFX_FSR3UPSCALER_CONTEXT_COUNT),
                  "FSR's backend");
        check(device->CreateComputeShader(motion_cs, sizeof motion_cs, NULL, &motion_shader), "the motion-vector shader");
        D3D11_BUFFER_DESC b = {sizeof(MotionConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        check(device->CreateBuffer(&b, NULL, &motion_constants), "the motion-vector constants");
        check(device->CreateComputeShader(mask_cs, sizeof mask_cs, NULL, &mask_shader), "the mask shader");
        b.ByteWidth = sizeof(CombineConstants);
        check(device->CreateBuffer(&b, NULL, &mask_constants), "the mask constants");
        CD3D11_SAMPLER_DESC s(D3D11_DEFAULT);
        check(device->CreateSamplerState(&s, &linear_clamp), "FSR's sampler");
    });
    if (e) s2fsr_shutdown();
    return e;
}

// Defined by the backend, not declared in its header.
DXGI_FORMAT ffxGetDX11FormatFromSurfaceFormat(FfxSurfaceFormat surfaceFormat);

static ID3D11Texture2D *texture(D3D11_TEXTURE2D_DESC d, const char *what) {
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D *t = NULL;
    check(device->CreateTexture2D(&d, NULL, &t), what);
    return t;
}

static ID3D11Texture2D *shared_texture(const FfxCreateResourceDescription &r) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = r.resourceDescription.width, d.Height = r.resourceDescription.height;
    d.MipLevels = 1, d.ArraySize = 1, d.SampleDesc.Count = 1;
    d.Format = ffxGetDX11FormatFromSurfaceFormat(r.resourceDescription.format);
    return texture(d, "an FSR shared texture");
}

EXPORT int s2fsr_create(void *color_texture, void *depth_texture, void *velocity_texture, void *stamp_texture, void *mask_texture,
                        void *target_texture, float sharpness) {
    s2fsr_release();
    color = (ID3D11Texture2D *)color_texture, depth = (ID3D11Texture2D *)depth_texture;
    velocity = (ID3D11Texture2D *)velocity_texture, stamp = (ID3D11Texture2D *)stamp_texture, mask = (ID3D11Texture2D *)mask_texture;
    target = target_texture ? (ID3D11Texture2D *)target_texture : color;
    color->AddRef(), depth->AddRef(), target->AddRef();
    if (velocity) velocity->AddRef();
    if (stamp) stamp->AddRef();
    if (mask) mask->AddRef();
    int e = guarded("create", [=] {
        D3D11_TEXTURE2D_DESC c, d;
        color->GetDesc(&c), depth->GetDesc(&d);
        if (!velocity != !stamp) fail(FFX_ERROR_INVALID_ARGUMENT, "s2fsr: velocity and stamp come together or not at all");
        if (velocity) {
            D3D11_TEXTURE2D_DESC v, s;
            velocity->GetDesc(&v), stamp->GetDesc(&s);
            if (v.Format != DXGI_FORMAT_R16G16_FLOAT || s.Format != DXGI_FORMAT_R32_FLOAT || v.Width != c.Width || v.Height != c.Height ||
                s.Width != c.Width || s.Height != c.Height)
                fail(FFX_ERROR_INVALID_ARGUMENT, "s2fsr: velocity (format %d, %ux%u) and stamp (format %d, %ux%u) must be R16G16_FLOAT and R32_FLOAT at %ux%u",
                     v.Format, v.Width, v.Height, s.Format, s.Width, s.Height, c.Width, c.Height);
            check(device->CreateShaderResourceView(velocity, NULL, &velocity_view), "velocity's view");
            check(device->CreateShaderResourceView(stamp, NULL, &stamp_view), "the stamp's view");
        }
        if (mask) {
            D3D11_TEXTURE2D_DESC m;
            mask->GetDesc(&m);
            if (m.Format != DXGI_FORMAT_R8_UNORM || m.Width != c.Width || m.Height != c.Height)
                fail(FFX_ERROR_INVALID_ARGUMENT, "s2fsr: the mask (format %d, %ux%u) must be R8_UNORM at %ux%u", m.Format, m.Width, m.Height,
                     c.Width, c.Height);
            check(device->CreateShaderResourceView(mask, NULL, &mask_view), "the mask's view");
        }
        if (c.Format != DXGI_FORMAT_R11G11B10_FLOAT && c.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
            fail(FFX_ERROR_INVALID_ARGUMENT, "s2fsr: the scene colour is DXGI format %d, not R11G11B10_FLOAT or R16G16B16A16_FLOAT",
                 c.Format);
        if (d.Format != DXGI_FORMAT_R32_TYPELESS)
            fail(FFX_ERROR_INVALID_ARGUMENT, "s2fsr: depth is DXGI format %d, not R32_TYPELESS (the engine's readable depth)",
                 d.Format);
        if (d.Width != c.Width || d.Height != c.Height || c.SampleDesc.Count != 1 || d.SampleDesc.Count != 1)
            fail(FFX_ERROR_INVALID_ARGUMENT, "s2fsr: colour %ux%u (%u samples) and depth %ux%u (%u samples) must match, unsampled",
                 c.Width, c.Height, c.SampleDesc.Count, d.Width, d.Height, d.SampleDesc.Count);
        D3D11_TEXTURE2D_DESC t;
        target->GetDesc(&t);
        if (t.Format != c.Format || t.Width < c.Width || t.Height < c.Height || t.SampleDesc.Count != 1)
            fail(FFX_ERROR_INVALID_ARGUMENT, "s2fsr: the target (format %d, %ux%u, %u samples) must be the colour's format, unsampled, no smaller than %ux%u",
                 t.Format, t.Width, t.Height, t.SampleDesc.Count, c.Width, c.Height);
        width = c.Width, height = c.Height, output_width = t.Width, output_height = t.Height, sharpening = sharpness;
        FfxFsr3UpscalerContextDescription x = {};
        // Our vectors are at render size and jitter-free; depth is standard and finite; exposure is a fixed 1.
        x.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
        x.maxRenderSize = {width, height};
        x.maxUpscaleSize = {output_width, output_height};
        x.backendInterface = backend;
        check_ffx(ffxFsr3UpscalerContextCreate(&fsr, &x), "FSR's context");
        created = true;
        FfxFsr3UpscalerSharedResourceDescriptions s;
        check_ffx(ffxFsr3UpscalerGetSharedResourceDescriptions(&fsr, &s), "FSR's shared resources");
        shared[0] = shared_texture(s.dilatedDepth);
        shared[1] = shared_texture(s.dilatedMotionVectors);
        shared[2] = shared_texture(s.reconstructedPrevNearestDepth);
        opaque = texture(c, "the opaque colour");
        output = texture(t, "FSR's output");
        c.Format = DXGI_FORMAT_R8_UNORM;
        reactive = texture(c, "the reactive mask");
        if (mask) {
            combined = texture(c, "the combined reactive mask");
            check(device->CreateShaderResourceView(reactive, NULL, &reactive_view), "the reactive mask's view");
            check(device->CreateUnorderedAccessView(combined, NULL, &combined_view), "the combined reactive mask's view");
        }
        c.Format = DXGI_FORMAT_R16G16_FLOAT;
        motion = texture(c, "the motion vectors");
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = DXGI_FORMAT_R32_FLOAT, v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = 1;
        check(device->CreateShaderResourceView(depth, &v, &depth_view), "depth's view");
        check(device->CreateUnorderedAccessView(motion, NULL, &motion_view), "the motion vectors' view");
    });
    if (e) s2fsr_release();
    return e;
}

static void invert(const double *m, double *out) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : c - 4 == r ? 1.0 : 0.0;
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        for (int r = col + 1; r < 4; r++)
            if (fabs(a[r][col]) > fabs(a[pivot][col])) pivot = r;
        std::swap(a[col], a[pivot]);
        double p = a[col][col];
        for (int c = 0; c < 8; c++) a[col][c] /= p;
        for (int r = 0; r < 4; r++) {
            double k = r == col ? 0 : a[r][col];
            for (int c = 0; c < 8; c++) a[r][c] -= k * a[col][c];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][c + 4];
}

static void motion_pass(ID3D11DeviceContext *c, const S2FsrFrame *f) {
    double a = 2.0 * f->jitter_x / width, b = -2.0 * f->jitter_y / height, jittered[16], inverse[16];
    for (int i = 0; i < 16; i++) jittered[i] = f->view_projection[i];
    for (int i = 0; i < 4; i++) jittered[i] += a * f->view_projection[12 + i], jittered[4 + i] += b * f->view_projection[12 + i];
    invert(jittered, inverse);
    MotionConstants k = {};
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) {
            double sum = 0;
            for (int i = 0; i < 4; i++) sum += f->previous_view_projection[r * 4 + i] * inverse[i * 4 + col];
            k.previous_from_jittered[r * 4 + col] = (float)sum;
        }
    k.size[0] = (float)width, k.size[1] = (float)height, k.jitter[0] = (float)a, k.jitter[1] = (float)b;
    k.objects = velocity_view != NULL;
    c->UpdateSubresource(motion_constants, 0, NULL, &k, 0, 0);
    c->CSSetShader(motion_shader, NULL, 0);
    c->CSSetConstantBuffers(0, 1, &motion_constants);
    ID3D11ShaderResourceView *inputs[3] = {depth_view, velocity_view, stamp_view};
    c->CSSetShaderResources(0, 3, inputs);
    c->CSSetUnorderedAccessViews(0, 1, &motion_view, NULL);
    c->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    // FSR reads the vectors next, and a texture bound as a UAV can't be bound for reading.
    ID3D11UnorderedAccessView *none = NULL;
    c->CSSetUnorderedAccessViews(0, 1, &none, NULL);
}

// The reactive mask with the engine's own: the larger of the two, the generated one only when made this frame.
static void mask_pass(ID3D11DeviceContext *c, bool generated) {
    CombineConstants k = {{width, height}, generated, 0};
    c->UpdateSubresource(mask_constants, 0, NULL, &k, 0, 0);
    c->CSSetShader(mask_shader, NULL, 0);
    c->CSSetConstantBuffers(0, 1, &mask_constants);
    ID3D11ShaderResourceView *inputs[2] = {reactive_view, mask_view};
    c->CSSetShaderResources(0, 2, inputs);
    c->CSSetUnorderedAccessViews(0, 1, &combined_view, NULL);
    c->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11UnorderedAccessView *none = NULL;
    c->CSSetUnorderedAccessViews(0, 1, &none, NULL);
}

// The backend defines this overload (C++, not const); the header's declaration (C, const) has no definition.
FfxResource ffxGetResourceDX11(ID3D11Resource *dx11Resource, FfxResourceDescription ffxResDescription,
                               wchar_t const *ffxResName, FfxResourceStates state);

static FfxResource resource(ID3D11Resource *r, FfxResourceStates state) {
    return ffxGetResourceDX11(r, GetFfxResourceDescriptionDX11(r), NULL, state);
}

// The engine caches its bindings: the compute stage and the output merger are put back as they were.
struct Saved {
    ID3D11ComputeShader *shader;
    ID3D11Buffer *cbs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    ID3D11ShaderResourceView *srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11UnorderedAccessView *uavs[D3D11_1_UAV_SLOT_COUNT];
    ID3D11SamplerState *samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
};

template <typename T> static void release_all(T **p, UINT n) {
    for (UINT i = 0; i < n; i++)
        if (p[i]) p[i]->Release();
}

static void save(ID3D11DeviceContext *c, Saved *s) {
    c->CSGetShader(&s->shader, NULL, NULL);
    c->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, s->cbs);
    c->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, s->srvs);
    c->CSGetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, s->uavs);
    c->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, s->samplers);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtvs, &s->dsv);
}

static void restore(ID3D11DeviceContext *c, Saved *s) {
    c->CSSetShader(s->shader, NULL, 0);
    c->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, s->cbs);
    c->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, s->srvs);
    c->CSSetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, s->uavs, NULL);
    c->CSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, s->samplers);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s->rtvs, s->dsv);
    if (s->shader) s->shader->Release();
    release_all(s->cbs, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
    release_all(s->srvs, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT), release_all(s->uavs, D3D11_1_UAV_SLOT_COUNT);
    release_all(s->samplers, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    release_all(s->rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
    if (s->dsv) s->dsv->Release();
}

EXPORT int s2fsr_frame(void *context, const S2FsrFrame *f) {
    ID3D11DeviceContext *c = (ID3D11DeviceContext *)context;
    Saved s;
    save(c, &s);
    // Colour is the engine's bound render target here, and a bound target can't be read.
    c->OMSetRenderTargets(0, NULL, NULL);
    int e = guarded("frame", [&] {
        motion_pass(c, f);
        FfxFsr3UpscalerDispatchDescription x = {};
        x.commandList = ffxGetCommandListDX11(c);
        if (f->reactive) {
            // AMD's sample values: tone-mapped differences of the largest channel, a cut-off at 0.2, 0.9 above it.
            FfxFsr3UpscalerGenerateReactiveDescription r = {};
            r.commandList = x.commandList;
            r.colorOpaqueOnly = resource(opaque, FFX_RESOURCE_STATE_COMPUTE_READ);
            r.colorPreUpscale = resource(color, FFX_RESOURCE_STATE_COMPUTE_READ);
            r.outReactive = resource(reactive, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            r.renderSize = {width, height};
            r.scale = 1, r.cutoffThreshold = 0.2f, r.binaryValue = 0.9f;
            r.flags = FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_TONEMAP | FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_THRESHOLD |
                      FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
            check_ffx(ffxFsr3UpscalerContextGenerateReactiveMask(&fsr, &r), "FSR's reactive mask");
        }
        if (mask) {
            mask_pass(c, f->reactive != 0);
            x.transparencyAndComposition = resource(mask, FFX_RESOURCE_STATE_COMPUTE_READ);
        }
        given_reactive = mask ? combined : f->reactive ? reactive : NULL;
        if (given_reactive) x.reactive = resource(given_reactive, FFX_RESOURCE_STATE_COMPUTE_READ);
        x.color = resource(color, FFX_RESOURCE_STATE_COMPUTE_READ);
        x.depth = resource(depth, FFX_RESOURCE_STATE_COMPUTE_READ);
        x.motionVectors = resource(motion, FFX_RESOURCE_STATE_COMPUTE_READ);
        x.output = resource(output, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        x.dilatedDepth = resource(shared[0], FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        x.dilatedMotionVectors = resource(shared[1], FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        x.reconstructedPrevNearestDepth = resource(shared[2], FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        x.jitterOffset = {f->jitter_x, f->jitter_y};
        x.motionVectorScale = {(float)width, (float)height};
        x.renderSize = {width, height};
        x.upscaleSize = {output_width, output_height};
        x.frameTimeDelta = f->frame_ms;
        x.reset = f->reset != 0;
        x.cameraNear = f->camera_near, x.cameraFar = f->camera_far, x.cameraFovAngleVertical = f->fov_y;
        x.preExposure = 1;
        x.enableSharpening = sharpening > 0, x.sharpness = sharpening;
        // FSR's shaders sample through slot 1 and its backend binds no sampler; the engine's compute passes leave theirs.
        c->CSSetSamplers(1, 1, &linear_clamp);
        check_ffx(ffxFsr3UpscalerContextDispatch(&fsr, &x), "FSR's dispatch");
        c->CopyResource(target, output);
    });
    restore(c, &s);
    // A dispatch that threw leaves the backend's job list behind; the context goes with it.
    if (e) s2fsr_release();
    return e;
}

EXPORT void s2fsr_opaque(void *context) {
    ((ID3D11DeviceContext *)context)->CopyResource(opaque, color);
}

EXPORT void s2fsr_textures(void **motion_texture, void **output_texture, void **reactive_texture) {
    *motion_texture = motion, *output_texture = output, *reactive_texture = given_reactive;
}

// The GCC module calls through s2fsr.h's types.
static_assert(std::is_same<decltype(&s2fsr_init), S2FsrInit>::value && std::is_same<decltype(&s2fsr_create), S2FsrCreate>::value &&
                  std::is_same<decltype(&s2fsr_frame), S2FsrFrameFn>::value &&
                  std::is_same<decltype(&s2fsr_release), S2FsrRelease>::value &&
                  std::is_same<decltype(&s2fsr_shutdown), S2FsrShutdown>::value &&
                  std::is_same<decltype(&s2fsr_textures), S2FsrTextures>::value &&
                  std::is_same<decltype(&s2fsr_opaque), S2FsrOpaque>::value,
              "an export differs from s2fsr.h");
