// s2fsr.dll on made-up frames, no game, on WARP and then on this PC's GPU when it has Direct3D 11.1: init refuses a
// feature level 11_0 device and create a depth that isn't R32_TYPELESS; frames at a few sizes write FSR's anti-aliased
// output into the colour; every frame leaves the compute and output-merger state as it was, also when FSR's backend
// throws, and the engine's compute samplers don't reach FSR; a known camera move gives the expected motion vectors and a
// still camera none; release and create again work; nothing leaks.
// Usage: test_fsrdll [s2fsr.dll]
#include "test_fsr.h"
#include <stdarg.h>
#include "motion_cs.h"

static Fsr fsr;
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static int checks, failures, state_changes;
static const Camera camera = {0.8, 16.0 / 9, 1, 1000};
static const int ffx_error_backend_api_error = (int)0x8000000d;

static bool expect(bool ok, const char *format, ...) {
    checks++, failures += !ok;
    printf("  %s ", ok ? "ok  " : "FAIL");
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    return ok;
}

// The engine's side of a frame: scene colour bound as its render target, depth as its depth view.
struct Target {
    int w, h;
    ID3D11Texture2D *color, *depth, *side;
    ID3D11RenderTargetView *color_view, *side_view;
    ID3D11DepthStencilView *depth_view;
    void make(int width, int height, DXGI_FORMAT format, UINT color_bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) {
        w = width, h = height;
        color = texture(device, w, h, format, color_bind);
        depth = texture(device, w, h, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
        side = texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
        device->CreateRenderTargetView(color, NULL, &color_view);
        device->CreateRenderTargetView(side, NULL, &side_view);
        D3D11_DEPTH_STENCIL_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_D32_FLOAT, d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth, &d, &depth_view);
        context->ClearDepthStencilView(depth_view, D3D11_CLEAR_DEPTH, (float)camera.depth(20), 0);
    }
    void release() {
        IUnknown *all[] = {color_view, side_view, depth_view, color, depth, side};
        for (IUnknown *u : all) u->Release();
    }
};

// Everything of the compute stage and the output merger s2fsr_frame could touch.
struct State {
    ID3D11ComputeShader *shader;
    ID3D11Buffer *cbs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    ID3D11ShaderResourceView *srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    ID3D11UnorderedAccessView *uavs[D3D11_1_UAV_SLOT_COUNT];
    ID3D11SamplerState *samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
    void read() {
        context->CSGetShader(&shader, NULL, NULL);
        context->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs);
        context->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);
        context->CSGetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, uavs);
        context->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    }
    std::vector<IUnknown *> all() const {
        std::vector<IUnknown *> v = {shader, dsv};
        v.insert(v.end(), cbs, cbs + D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
        v.insert(v.end(), srvs, srvs + D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
        v.insert(v.end(), uavs, uavs + D3D11_1_UAV_SLOT_COUNT);
        v.insert(v.end(), samplers, samplers + D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
        v.insert(v.end(), rtvs, rtvs + D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
        return v;
    }
    void release() {
        for (IUnknown *u : all())
            if (u) u->Release();
    }
};

// Bindings the engine could hold when s2fsr_frame is called, in the first and last slots of each kind, and a point,
// wrapping sampler where FSR's shaders expect linear clamp.
struct EngineState {
    ID3D11ComputeShader *shader;
    ID3D11Buffer *cb;
    ID3D11Texture2D *read_tex, *write_tex[3];
    ID3D11ShaderResourceView *srv;
    ID3D11UnorderedAccessView *uav[3];
    ID3D11SamplerState *sampler;
    void make() {
        device->CreateComputeShader(motion_cs, sizeof motion_cs, NULL, &shader);
        D3D11_BUFFER_DESC b = {256, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        device->CreateBuffer(&b, NULL, &cb);
        read_tex = texture(device, 16, 16, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        device->CreateShaderResourceView(read_tex, NULL, &srv);
        for (int i = 0; i < 3; i++) {
            write_tex[i] = texture(device, 16, 16, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
            device->CreateUnorderedAccessView(write_tex[i], NULL, &uav[i]);
        }
        CD3D11_SAMPLER_DESC s(D3D11_DEFAULT);
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        device->CreateSamplerState(&s, &sampler);
    }
    void bind(const Target &t) {
        context->CSSetShader(shader, NULL, 0);
        context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetConstantBuffers(D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1, 1, &cb);
        UINT srv_slots[] = {0, 5, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1};
        for (UINT s : srv_slots) context->CSSetShaderResources(s, 1, &srv);
        UINT uav_slots[] = {0, 7, D3D11_1_UAV_SLOT_COUNT - 1};
        for (int i = 0; i < 3; i++) context->CSSetUnorderedAccessViews(uav_slots[i], 1, &uav[i], NULL);
        UINT sampler_slots[] = {0, 1, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1};
        for (UINT s : sampler_slots) context->CSSetSamplers(s, 1, &sampler);
        ID3D11RenderTargetView *rtvs[2] = {t.color_view, t.side_view};
        context->OMSetRenderTargets(2, rtvs, t.depth_view);
    }
    void release() {
        IUnknown *all[] = {shader, cb, read_tex, write_tex[0], write_tex[1], write_tex[2], srv, uav[0], uav[1], uav[2], sampler};
        for (IUnknown *u : all) u->Release();
    }
};

static S2FsrFrame still_frame(int i) {
    S2FsrFrame f = {};
    camera.view_projection(0, 0, 0, f.view_projection);
    camera.view_projection(0, 0, 0, f.previous_view_projection);
    jitter_of(i, &f.jitter_x, &f.jitter_y);
    f.frame_ms = 16.7f, f.camera_near = (float)camera.near_z, f.camera_far = (float)camera.far_z;
    f.fov_y = (float)camera.fov_y, f.reset = i == 0;
    return f;
}

// s2fsr_frame with the engine's bindings in place when given; each that differs afterwards counts in state_changes.
static int call_frame(EngineState *engine, const Target &t, const S2FsrFrame &f) {
    if (!engine) return fsr.frame(context, &f);
    engine->bind(t);
    State before = {}, after = {};
    before.read();
    int r = fsr.frame(context, &f);
    after.read();
    std::vector<IUnknown *> a = before.all(), b = after.all();
    for (size_t i = 0; i < a.size(); i++) state_changes += a[i] != b[i];
    before.release(), after.release();
    context->ClearState();
    return r;
}

// A grey level as the colour texture holds it.
static float stored(DXGI_FORMAT format, float v) {
    return format == DXGI_FORMAT_R11G11B10_FLOAT ? from_small_float(to_small_float(v, 6), 6) : from_half(to_half(v));
}

// Frames of a still camera over a checker the jitter moves (pixel (x, y) shows it at (x + 0.5 - jx, y + 0.5 - jy));
// the number that failed.
static int still_frames(EngineState *engine, const Target &t, int frames) {
    int failed = 0;
    std::vector<float> grey((size_t)t.w * t.h);
    for (int i = 0; i < frames; i++) {
        S2FsrFrame f = still_frame(i);
        for (int y = 0; y < t.h; y++)
            for (int x = 0; x < t.w; x++) grey[(size_t)y * t.w + x] = checker(x + 0.5 - f.jitter_x, y + 0.5 - f.jitter_y);
        upload(context, t.color, grey);
        failed += call_frame(engine, t, f) != 0;
    }
    return failed;
}

// FSR's output of the still checker: finite; where a 2x2-pixel square around the pixel lies in one cell, that cell's
// level; and the input's two levels blended at the edges.
static void check_output(const Target &t, DXGI_FORMAT format) {
    std::vector<float> got = read_channel(device, context, t.color, 0);
    float lo = stored(format, 0.05f), hi = stored(format, 0.8f);
    int bad = 0, inside = 0, off = 0, between = 0, n = 0;
    for (int y = 8; y < t.h - 8; y++)
        for (int x = 8; x < t.w - 8; x++, n++) {
            float v = got[(size_t)y * t.w + x], c = checker(x + 0.5, y + 0.5);
            bad += !isfinite(v);
            between += v > lo + 0.05f && v < hi - 0.05f;
            if (checker(x - 0.5, y - 0.5) == c && checker(x + 1.5, y - 0.5) == c && checker(x - 0.5, y + 1.5) == c &&
                checker(x + 1.5, y + 1.5) == c)
                inside++, off += fabsf(v - stored(format, c)) > 0.05f * hi;
        }
    expect(!bad && inside > n / 100 && !off, "output finite, and inside the cells their level (%d of %d pixels off)", off,
           inside);
    expect(between > n / 10, "output anti-aliased: %.1f%% of pixels between the input's two levels (input: none)",
           100.0 * between / n);
}

// The plane at the depth buffer's depth, seen by a camera that moved by (dx, dy, dz) since the last frame: at each
// pixel centre, the jittered sample's point, its unjittered position now and where the previous camera saw it.
static void check_motion(const Target &t, double dx, double dy, double dz, float jx, float jy) {
    S2FsrFrame f = still_frame(1);
    camera.view_projection(dx, dy, dz, f.view_projection);
    f.jitter_x = jx, f.jitter_y = jy;
    expect(fsr.frame(context, &f) == 0, "frame with the camera moved by (%.2f, %.2f, %.2f)", dx, dy, dz);
    void *motion, *output;
    fsr.textures(&motion, &output);
    std::vector<float> mx = read_channel(device, context, (ID3D11Texture2D *)motion, 0);
    std::vector<float> my = read_channel(device, context, (ID3D11Texture2D *)motion, 1);
    double z = camera.view_z((float)camera.depth(20)), a = 2.0 * jx / t.w, b = -2.0 * jy / t.h, worst = 0, largest = 0;
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++) {
            double ux = (x + 0.5) / t.w * 2 - 1 - a, uy = 1 - (y + 0.5) / t.h * 2 - b;
            double px = ux * z / camera.p00() + dx, py = uy * z / camera.p11() + dy, pz = z + dz;
            double ex = (camera.p00() * px / pz - ux) * 0.5, ey = -(camera.p11() * py / pz - uy) * 0.5;
            size_t i = (size_t)y * t.w + x;
            double err = fmax(fabs(mx[i] - ex), fabs(my[i] - ey)) / (2e-5 + 1e-3 * fmax(fabs(ex), fabs(ey)));
            worst = fmax(worst, err), largest = fmax(largest, fmax(fabs(ex), fabs(ey)));
        }
    expect(worst <= 1, "motion vectors for that move: worst error %.2f of the tolerance (largest vector %.4f UV)", worst, largest);
}

// The G-buffer's object motion as the engine's targets hold it: velocity everywhere, the stamp equal to the final depth
// in the left half only (an object is what those pixels show) and -1 in the right (nothing, or covered later). The left
// half must carry the object's velocity, the right the camera's vectors exactly as without objects.
static void check_objects(const Target &t) {
    const float vx = 0.0123f, vy = -0.0456f;
    ID3D11Texture2D *velocity = texture(device, t.w, t.h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D *stamp = texture(device, t.w, t.h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    std::vector<unsigned short> v((size_t)t.w * t.h * 2);
    std::vector<float> s((size_t)t.w * t.h);
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++) {
            size_t i = (size_t)y * t.w + x;
            v[2 * i] = to_half(vx), v[2 * i + 1] = to_half(vy);
            s[i] = x < t.w / 2 ? (float)camera.depth(20) : -1.0f;
        }
    context->UpdateSubresource(velocity, 0, NULL, v.data(), t.w * 4, 0);
    context->UpdateSubresource(stamp, 0, NULL, s.data(), t.w * 4, 0);
    S2FsrFrame f = still_frame(1);
    camera.view_projection(0.3, -0.2, 0, f.view_projection);
    f.jitter_x = 0.37f, f.jitter_y = -0.29f;
    std::vector<float> camera_x, camera_y;
    if (expect(fsr.create(t.color, t.depth, NULL, NULL, NULL, 0) == 0 && fsr.frame(context, &f) == 0, "a frame without objects")) {
        void *motion, *output;
        fsr.textures(&motion, &output);
        camera_x = read_channel(device, context, (ID3D11Texture2D *)motion, 0);
        camera_y = read_channel(device, context, (ID3D11Texture2D *)motion, 1);
    }
    ULONG velocity_refs = refs(velocity), stamp_refs = refs(stamp);
    if (expect(fsr.create(t.color, t.depth, velocity, stamp, NULL, 0) == 0 && fsr.frame(context, &f) == 0, "the same frame with objects") &&
        !camera_x.empty()) {
        void *motion, *output;
        fsr.textures(&motion, &output);
        std::vector<float> mx = read_channel(device, context, (ID3D11Texture2D *)motion, 0), my = read_channel(device, context, (ID3D11Texture2D *)motion, 1);
        int object_off = 0, camera_off = 0;
        for (int y = 0; y < t.h; y++)
            for (int x = 0; x < t.w; x++) {
                size_t i = (size_t)y * t.w + x;
                if (x < t.w / 2) object_off += mx[i] != from_half(to_half(vx)) || my[i] != from_half(to_half(vy));
                else camera_off += mx[i] != camera_x[i] || my[i] != camera_y[i];
            }
        expect(!object_off && !camera_off, "where the stamp is the final depth the object's motion, elsewhere the camera's (%d and %d pixels off)",
               object_off, camera_off);
    }
    fsr.release();
    expect(refs(velocity) == velocity_refs && refs(stamp) == stamp_refs, "release lets go of velocity and the stamp");
    ID3D11Texture2D *wrong = texture(device, t.w, t.h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    expect(fsr.create(t.color, t.depth, wrong, stamp, NULL, 0) != 0 && fsr.create(t.color, t.depth, velocity, NULL, NULL, 0) != 0,
           "create refuses a velocity that isn't R16G16_FLOAT, and velocity without a stamp");
    fsr.release();
    wrong->Release(), velocity->Release(), stamp->Release();
}

// The reactive mask from an opaque copy, and FSR writing a target twice the colour's size.
static void check_reactive_and_upscale(const Target &t) {
    S2FsrFrame f = still_frame(1);
    if (expect(fsr.create(t.color, t.depth, NULL, NULL, NULL, 0) == 0, "create for the reactive mask")) {
        f.reactive = 1;
        fsr.opaque(context);
        expect(fsr.frame(context, &f) == 0, "a frame with the reactive mask from an opaque copy");
    }
    D3D11_TEXTURE2D_DESC c;
    t.color->GetDesc(&c);
    ID3D11Texture2D *big = texture(device, 2 * t.w, 2 * t.h, c.Format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D *half = texture(device, t.w / 2, t.h / 2, c.Format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    f.reactive = 0;
    int failed = 0;
    if (expect(fsr.create(t.color, t.depth, NULL, NULL, big, 0) == 0, "create upscaling %dx%d to %dx%d", t.w, t.h, 2 * t.w, 2 * t.h)) {
        std::vector<float> grey((size_t)t.w * t.h);
        for (int i = 0; i < 8; i++) {
            f = still_frame(i);
            for (int y = 0; y < t.h; y++)
                for (int x = 0; x < t.w; x++) grey[(size_t)y * t.w + x] = checker(x + 0.5 - f.jitter_x, y + 0.5 - f.jitter_y);
            upload(context, t.color, grey);
            failed += fsr.frame(context, &f) != 0;
        }
        // The checker's two levels in equal parts: its mean survives the upscale.
        std::vector<float> r = read_channel(device, context, big, 0);
        double sum = 0;
        int bad = 0;
        for (float v : r) bad += !isfinite(v), sum += v;
        double mean = sum / r.size();
        expect(!failed && !bad && mean > 0.35 && mean < 0.5, "8 upscaled frames, the target finite with the checker's mean (%.3f)", mean);
    }
    expect(fsr.create(t.color, t.depth, NULL, NULL, half, 0) != 0, "create refuses a target smaller than the colour");
    fsr.release();
    big->Release(), half->Release();
}

static void run(const char *name) {
    printf("test_fsrdll: %s\n", name);
    debug_errors(device);
    ULONG base = refs(device);
    if (!expect(fsr.init(device, log_line) == 0, "init")) return;
    EngineState engine;
    engine.make();

    Target refused;
    refused.make(64, 64, DXGI_FORMAT_R11G11B10_FLOAT);
    struct {
        DXGI_FORMAT format;
        UINT bind;
        const char *name;
    } bad_depths[] = {{DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, "R32_FLOAT"},
                      {DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, "R24G8_TYPELESS"}};
    for (auto &bad : bad_depths) {
        ID3D11Texture2D *depth = texture(device, 64, 64, bad.format, bad.bind);
        ULONG device_refs = refs(device), color_refs = refs(refused.color), depth_refs = refs(depth);
        expect(fsr.create(refused.color, depth, NULL, NULL, NULL, 0) != 0, "create refuses an %s depth", bad.name);
        expect(refs(device) == device_refs && refs(refused.color) == color_refs && refs(depth) == depth_refs,
               "a refused create holds nothing");
        depth->Release();
    }
    refused.release();

    struct {
        int w, h;
        DXGI_FORMAT format;
        const char *format_name;
        float sharpness;
    } sizes[] = {{256, 144, DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", 0},
                 {643, 357, DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", 0.5f},
                 {1280, 720, DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", 0},
                 {400, 300, DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 0}};
    const int frames = 16;
    for (auto &s : sizes) {
        Target t;
        t.make(s.w, s.h, s.format);
        ULONG device_refs = refs(device), color_refs = refs(t.color), depth_refs = refs(t.depth);
        if (!expect(fsr.create(t.color, t.depth, NULL, NULL, NULL, s.sharpness) == 0, "create %dx%d %s, sharpness %.1f", s.w, s.h,
                    s.format_name, s.sharpness)) {
            t.release();
            continue;
        }
        state_changes = 0;
        expect(!still_frames(&engine, t, frames), "%d frames of a still camera over a jittered checker", frames);
        expect(!state_changes, "each frame kept the compute and output-merger state (%d bindings changed)", state_changes);
        void *motion, *output;
        fsr.textures(&motion, &output);
        expect(read_bytes(device, context, t.color) == read_bytes(device, context, (ID3D11Texture2D *)output),
               "the colour holds FSR's output");
        check_output(t, s.format);
        std::vector<float> mx = read_channel(device, context, (ID3D11Texture2D *)motion, 0),
                           my = read_channel(device, context, (ID3D11Texture2D *)motion, 1);
        float still = 0;
        for (size_t i = 0; i < mx.size(); i++) still = fmaxf(still, fmaxf(fabsf(mx[i]), fabsf(my[i])));
        expect(still < 1e-6f, "a still camera gives no motion (largest %g UV)", still);
        check_motion(t, 0.3, -0.2, 0, 0.37f, -0.29f);
        // A field that varies across the screen, so a jitter left in the vectors' positions shows.
        check_motion(t, -0.1, 0.25, 5, -0.41f, 0.13f);
        check_objects(t);
        check_reactive_and_upscale(t);
        fsr.release();
        expect(refs(device) == device_refs && refs(t.color) == color_refs && refs(t.depth) == depth_refs,
               "release lets go of everything create and the frames made");
        t.release();
    }

    Target same;
    same.make(256, 144, DXGI_FORMAT_R11G11B10_FLOAT);
    expect(fsr.create(same.color, same.depth, NULL, NULL, NULL, 0) == 0 && !still_frames(NULL, same, frames),
           "create again after release, %d frames without the engine's bindings", frames);
    std::vector<unsigned char> alone = read_bytes(device, context, same.color);
    expect(fsr.create(same.color, same.depth, NULL, NULL, NULL, 0) == 0 && !still_frames(&engine, same, frames) &&
               read_bytes(device, context, same.color) == alone,
           "create over a context, the same frames with the engine's point, wrapping compute samplers bound: the same output");
    int errors = debug_errors(device);
    expect(errors <= 0, "the debug layer reported no errors%s", errors < 0 ? " (not installed)" : "");
    fsr.release();
    same.release();

    Target unreadable;
    unreadable.make(128, 128, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET);
    expect(fsr.create(unreadable.color, unreadable.depth, NULL, NULL, NULL, 0) == 0, "create on a colour without a shader view");
    state_changes = 0;
    expect(call_frame(&engine, unreadable, still_frame(0)) == ffx_error_backend_api_error,
           "FSR's backend throwing comes back as FFX_ERROR_BACKEND_API_ERROR");
    expect(!state_changes, "that frame kept the compute and output-merger state (%d bindings changed)", state_changes);
    debug_errors(device);
    fsr.release();
    unreadable.release();

    engine.release();
    fsr.shutdown();
    expect(refs(device) == base, "shutdown lets go of everything (device references %lu, before init %lu)", refs(device), base);
}

int main(int argc, char **argv) {
    const char *dll = argc > 1 ? argv[1] : "s2fsr.dll";
    if (!load_fsr(dll, &fsr)) {
        printf("test_fsrdll: cannot load %s, or it lacks an export\n", dll);
        return 1;
    }
    device = make_device(D3D_DRIVER_TYPE_WARP, D3D_FEATURE_LEVEL_11_0, &context);
    ULONG base = refs(device);
    printf("test_fsrdll: WARP at feature level 11_0\n");
    expect(fsr.init(device, log_line) != 0, "init refuses a feature level 11_0 device");
    expect(refs(device) == base, "a refused init holds nothing, without a shutdown");
    context->Release(), device->Release();

    struct {
        D3D_DRIVER_TYPE type;
        const char *name;
    } drivers[] = {{D3D_DRIVER_TYPE_WARP, "WARP"}, {D3D_DRIVER_TYPE_HARDWARE, "hardware"}};
    for (auto &d : drivers) {
        device = make_device(d.type, D3D_FEATURE_LEVEL_11_1, &context);
        if (!device) {
            printf("test_fsrdll: no %s device at feature level 11_1, skipped\n", d.name);
            continue;
        }
        char gpu[128], name[192];
        device_name(device, gpu, sizeof gpu);
        snprintf(name, sizeof name, "%s at feature level 11_1 (%s)", gpu, d.name);
        run(name);
        context->Release(), device->Release();
    }
    printf("test_fsrdll: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
