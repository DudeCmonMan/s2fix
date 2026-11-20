// The jitter's sign and the motion vectors' convention through s2fsr.dll, driven as the game module drives it. A plane
// with a turned checker faces a camera that pans 1.5 px a frame right and down; each frame is rendered on the CPU at the
// pixel centres through the view-projection with the jitter added as the module adds it (Halton(2,3), 32 phases), and
// s2fsr_frame gets the unjittered matrices and the jitter. After 96 frames FSR's output is compared with the plane
// supersampled through the unjittered camera. The jitter as passed must clearly beat each axis flipped, and the previous
// camera as passed must clearly beat none (previous = current) and the two swapped. On WARP, then on the machine's GPU.
// Usage: test_fsrjit [s2fsr.dll]
#include "test_fsr.h"

static Fsr fsr;
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static const int width = 1280, height = 640, frames = 96, border = 16;
static const double pan = 1.5, plane_z = 20;
static const Camera camera = {0.8, (double)width / height, 1, 1000};
// Clearly: 10% less error. The runs are deterministic, and a flipped Halton base-3 axis costs FSR little more than that.
static const double margin = 0.9;

static const double pixel_on_plane = 2 * plane_z * tan(camera.fov_y / 2) / height;

static void camera_at(int frame, float *m) {
    camera.view_projection(frame * pan * pixel_on_plane, -frame * pan * pixel_on_plane, 0, m);
}

// The checker's cell on the plane where a rasterizer drawing it through m samples at (x, y) in pixels: the plane's point
// p with (row 0 - ndc.x row 3) . p = 0 and (row 1 - ndc.y row 3) . p = 0.
static int sample(const float *m, double x, double y) {
    double ndc[2] = {x / width * 2 - 1, 1 - y / height * 2}, c[2][4];
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < 4; k++) c[r][k] = m[r * 4 + k] - ndc[r] * m[12 + k];
    double e0 = -(c[0][2] * plane_z + c[0][3]), e1 = -(c[1][2] * plane_z + c[1][3]), det = c[0][0] * c[1][1] - c[0][1] * c[1][0];
    double px = (e0 * c[1][1] - c[0][1] * e1) / det, py = (c[0][0] * e1 - e0 * c[1][0]) / det;
    return checker_cell(px / pixel_on_plane, -py / pixel_on_plane);
}

// Every run sees the same frames and is measured against the same supersampled last frame: both are made once.
static std::vector<unsigned char> cells;
static std::vector<float> truth;

static void render_frames() {
    if (!cells.empty()) return;
    cells.resize((size_t)frames * width * height);
    for (int i = 0; i < frames; i++) {
        float m[16], jx, jy;
        camera_at(i, m);
        jitter_of(i, &jx, &jy);
        add_jitter(m, jx, jy, width, height);
        unsigned char *frame = &cells[(size_t)i * width * height];
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) frame[(size_t)y * width + x] = (unsigned char)sample(m, x + 0.5, y + 0.5);
    }
    float last[16];
    camera_at(frames - 1, last);
    truth.resize((size_t)width * height);
    for (int y = border; y < height - border; y++)
        for (int x = border; x < width - border; x++) {
            double sum = 0;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) sum += checker_levels[sample(last, x + (a + 0.5) / 4, y + (b + 0.5) / 4)];
            truth[(size_t)y * width + x] = (float)(sum / 16);
        }
}

enum Motion { GIVEN, NONE, SWAPPED };

// Mean absolute error against the supersampled plane, with the jitter passed times (kx, ky); -1 if a call failed.
static double run(float kx, float ky, Motion motion) {
    ID3D11Texture2D *color = texture(device, width, height, DXGI_FORMAT_R11G11B10_FLOAT,
                                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D *depth = texture(device, width, height, DXGI_FORMAT_R32_TYPELESS,
                                     D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    ID3D11DepthStencilView *dsv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
    dd.Format = DXGI_FORMAT_D32_FLOAT, dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depth, &dd, &dsv);
    context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, (float)camera.depth(plane_z), 0);
    dsv->Release();
    bool ok = fsr.create(color, depth, NULL, NULL, NULL, 0) == 0;
    render_frames();
    std::vector<float> grey((size_t)width * height);
    for (int i = 0; ok && i < frames; i++) {
        S2FsrFrame f = {};
        float now[16], before[16];
        camera_at(i, now);
        camera_at(i ? i - 1 : 0, before);
        jitter_of(i, &f.jitter_x, &f.jitter_y);
        const unsigned char *frame = &cells[(size_t)i * width * height];
        for (size_t p = 0; p < grey.size(); p++) grey[p] = checker_levels[frame[p]];
        upload(context, color, grey);
        memcpy(f.view_projection, motion == SWAPPED ? before : now, sizeof now);
        memcpy(f.previous_view_projection, motion == GIVEN ? before : now, sizeof now);
        f.jitter_x *= kx, f.jitter_y *= ky;
        f.frame_ms = 16.7f, f.camera_near = (float)camera.near_z, f.camera_far = (float)camera.far_z;
        f.fov_y = (float)camera.fov_y, f.reset = i == 0;
        ok = fsr.frame(context, &f) == 0;
    }
    double err = 0;
    if (ok) {
        std::vector<float> got = read_channel(device, context, color, 0);
        for (int y = border; y < height - border; y++)
            for (int x = border; x < width - border; x++) err += fabs(got[(size_t)y * width + x] - truth[(size_t)y * width + x]);
        err /= (double)(width - 2 * border) * (height - 2 * border);
    }
    fsr.release();
    color->Release(), depth->Release();
    return ok ? err : -1;
}

static int checks, failures;

static void compare(const char *right_name, double right, const char *wrong_name, double wrong) {
    bool ok = right >= 0 && wrong >= 0 && right <= margin * wrong;
    checks++, failures += !ok;
    printf("  %s %s %.4f against %s %.4f\n", ok ? "ok  " : "FAIL", right_name, right, wrong_name, wrong);
}

static void test(const char *name) {
    printf("test_fsrjit: %s, %dx%d, %d frames, panning %.1f px a frame, mean error against the plane supersampled:\n",
           name, width, height, frames, pan);
    if (fsr.init(device, log_line)) {
        printf("  FAIL init\n");
        checks++, failures++;
        return;
    }
    double given = run(1, 1, GIVEN), flip_x = run(-1, 1, GIVEN), flip_y = run(1, -1, GIVEN), flip_both = run(-1, -1, GIVEN),
           no_jitter = run(0, 0, GIVEN), no_motion = run(1, 1, NONE), swapped = run(1, 1, SWAPPED);
    printf("  jitter as passed %.4f, x flipped %.4f, y flipped %.4f, both flipped %.4f, none %.4f\n", given, flip_x, flip_y,
           flip_both, no_jitter);
    printf("  previous camera as passed %.4f, none %.4f, swapped %.4f\n", given, no_motion, swapped);
    compare("jitter as passed", given, "x flipped", flip_x);
    compare("jitter as passed", given, "y flipped", flip_y);
    compare("previous camera as passed", given, "none", no_motion);
    compare("previous camera as passed", given, "swapped", swapped);
    int errors = debug_errors(device);
    checks++, failures += errors > 0;
    printf("  %s the debug layer reported %s\n", errors > 0 ? "FAIL" : "ok  ",
           errors < 0 ? "nothing (not installed)" : errors ? "errors" : "no errors");
    fsr.shutdown();
}

int main(int argc, char **argv) {
    const char *dll = argc > 1 ? argv[1] : "s2fsr.dll";
    if (!load_fsr(dll, &fsr)) {
        printf("test_fsrjit: cannot load %s, or it lacks an export\n", dll);
        return 1;
    }
    struct {
        D3D_DRIVER_TYPE type;
        const char *name;
    } drivers[] = {{D3D_DRIVER_TYPE_WARP, "WARP"}, {D3D_DRIVER_TYPE_HARDWARE, "hardware"}};
    for (auto &d : drivers) {
        device = make_device(d.type, D3D_FEATURE_LEVEL_11_1, &context);
        if (!device) {
            printf("test_fsrjit: no %s device at feature level 11_1, skipped\n", d.name);
            continue;
        }
        char gpu[128], name[192];
        device_name(device, gpu, sizeof gpu);
        snprintf(name, sizeof name, "%s (%s)", gpu, d.name);
        test(name);
        context->Release(), device->Release();
    }
    printf("test_fsrjit: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
