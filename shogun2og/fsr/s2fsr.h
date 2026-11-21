// s2fsr.dll: AMD FSR 3.1's upscaler and the camera motion-vector pass on D3D11 behind a C interface, built with MSVC
// (32-bit, static runtime) and called from the GCC-built fsr module. Every export catches the backend's exceptions.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Plain data only, so a 64-bit helper can take the same block later; resources are given at create.
typedef struct S2FsrFrame {
    // Unjittered, row-major for clip = M * v (the engine's layout).
    float view_projection[16], previous_view_projection[16];
    // Render pixels, the image's shift, y down; the projection got +2x/width, -2y/height.
    float jitter_x, jitter_y;
    float frame_ms, camera_near, camera_far, fov_y;
    int reset;
    // 1 when s2fsr_opaque copied this frame's colour before its transparent effects: FSR then makes a reactive mask.
    int reactive;
} S2FsrFrame;

typedef void (*S2FsrLog)(const char *line);

// The device must be feature level 11_1 (FSR's luma pyramid binds more UAVs than 11_0 allows).
typedef int (*S2FsrInit)(void *device, S2FsrLog log);
// color: the HDR scene colour at render size; depth: R32_TYPELESS. velocity (R16G16_FLOAT) and stamp (R32_FLOAT):
// the G-buffer's object motion and the depth each object wrote it at, both or neither; where the stamp equals the
// final depth the object's motion is used, elsewhere the camera's. mask (R8_UNORM) or NULL: pixels the engine's own
// transparent draws marked (rain, snow), reactive and FSR's transparency mask at its value. All ID3D11Texture2D*, the
// render size. target: where FSR's output goes, at the output size in color's format; NULL writes it back into color
// (no upscaling).
typedef int (*S2FsrCreate)(void *color, void *depth, void *velocity, void *stamp, void *mask, void *target, float sharpness);
// On the immediate context: copies the colour as it is before the transparent effects, for this frame's reactive mask.
typedef void (*S2FsrOpaque)(void *context);
// On the immediate context: the motion vectors, the reactive mask when asked, FSR, the copy into the target; the
// compute and output-merger state is as before.
typedef int (*S2FsrFrameFn)(void *context, const S2FsrFrame *frame);
typedef void (*S2FsrRelease)(void);
typedef void (*S2FsrShutdown)(void);
// For diagnostics: the motion vectors (R16G16_FLOAT, UV, previous minus current, y down), FSR's output and the reactive
// mask the last frame gave FSR (R8_UNORM; NULL if none).
typedef void (*S2FsrTextures)(void **motion, void **output, void **reactive);

#ifdef __cplusplus
}
#endif
