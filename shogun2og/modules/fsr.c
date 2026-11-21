#define COBJMACROS
#include "../s2module.h"
#include "../checks/fsr.h"
#include "../fsr/s2fsr.h"
#include "../fsr/shaders/object_blobs.h"
#include <d3d11.h>
#include <ctype.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

// AMD FSR 3.1 as the game's anti-aliasing. Its parts are separate fixes switched together. What FSR needs from the
// engine (the 11_1 device, wider records and bone rows, our shaders) is in place from start, so the AA menu switches it.
#define ENGINE_GLOBAL_RVA 0x1c9a920
#define RENDER_DEVICE_RVA 0x1c9a93c
#define TEXTURE_MANAGER_RVA 0x1c9ad50
#define DEVICE_INTERFACE_OFFSET 0xb8
#define FEATURE_LEVEL_OFFSET 0xfc
#define IMMEDIATE_CONTEXT_OFFSET 0x108
// Caps +0x8, the shader level the engine runs: 4 for SM5.
#define SHADER_LEVEL_OFFSET 0x1724
// D3D11CreateDevice's level count and list: push 3; lea eax,[esp+0x20]; push eax.
#define LEVEL_LIST_RVA 0x145fe60
// The caps switch knows only 10_0, 10_1 and 11_0 by equality; jge gives 11_1 the 11_0 caps instead of the weakest.
#define CAPS_LEVEL_RVA 0x147bae2
// The active AA setting's only reader: mov eax,[ecx+0x69930]; ret (thiscall engine).
#define AA_GETTER_RVA 0x13f3830
// The graphics options' path for an AA change: every view's settings update, then the engine's resource rebuild (vtbl+0x60).
#define VIEWS_UPDATE_RVA 0x13d4d30
#define REBUILD_SLOT (0x60 / 4)
#define AA_CHANGED_MASK 3
#define AA_CHANGED_REBUILD 2
#define TEXTURE_MANAGER_BUSY_OFFSET 0xa3f
#define FRAME_PRESENT_RVA 0x1401ce0
#define FRAME_COUNTER_OFFSET 0x69ccc
#define RESET_FLAG_OFFSET 0x69872
#define CAMERA_WRITER_RVA 0x141bbb0
// The G-buffer view's pre-pass (vtable slot), the first main-camera load the jitter covers.
#define GBUFFER_PREPASS_SLOT_RVA 0x1760044
// Deferred Alpha's post-pass (vtable slot): the scene colour and depth are complete, nothing drawn after them yet.
#define ALPHA_POSTPASS_SLOT_RVA 0x176081c
// The menu: the AA list's push to the UI (call site in its builder), the start-up AA clamp's call site; the game's AA
// setting holds 5..9 for FSR's modes, the requested copy too, and the device's highest AA level.
#define AA_LIST_CALL_RVA 0x11d3008
#define AA_LIST_PUSH_RVA 0xe7d820
#define START_CLAMP_CALL_RVA 0x1401253
#define START_CLAMP_RVA 0x13f9310
#define AA_SETTING_OFFSET 0x69930
#define AA_REQUESTED_OFFSET 0x698c0
#define AA_MAX_OFFSET 0x699a0
// The setter the saved choice comes in through at start keeps a value above the device's highest only here.
#define AA_ASKED_OFFSET 0x69c54
#define REQUESTED_AA_SLOT_RVA 0x1754660
// The game's shutdown saves its settings to the preferences; gfx_aa from the requested AA's getter.
#define SAVE_AA_CALL_RVA 0x932ed
#define FSR_AA_FIRST 5
#define FSR_MODES 5
#define AA_LIST_CHARS 512
// Upscaling: the deferred lighting view's light accumulation (+0x188, the scene colour) and its resolved copy (+0x18c,
// made only with MSAA, behind the branch at 0x1148549e); the resolve at A; the screen-sized targets made before A.
#define LIGHTING_VIEW_RVA 0x13e2020
#define LIGHT_ACCUMULATION 0x188
#define LIGHT_RESOLVED 0x18c
#define AA_BRANCH_RVA 0x148549e
#define AA_BRANCH_SKIP_RVA 0x1485597
#define RESOLVE_RVA 0x14955e0
#define SETTINGS_GLOBAL_RVA 0x1c9a924
#define SETTINGS_WIDTH 0x8
#define SETTINGS_HEIGHT 0xc
#define SCREEN_SIZE_PARAM 0x2a8
#define INV_SCREEN_SIZE_PARAM 0x350
#define FLOAT2_PARAM_VALUE 0x28
// Deferred Alpha's pre-pass slot: the colour before its translucent draws (trails, rain) and the particles.
#define ALPHA_PREPASS_SLOT_RVA 0x1760818
// Point B: EFFECTS_TO_FRAME's post-pass slot (a bare ret), after the particles are composed into the scene colour.
#define EFFECTS_POSTPASS_SLOT_RVA 0x176b10c
#define SCENE_OFFSET 0x1c
#define VIEW_SCENE_OFFSET 0x38
#define SCENE_CAMERA_OFFSET 0x14
#define SCENE_GBUFFER_VIEW_OFFSET 0x3cc
#define GBUFFER_DEPTH_OFFSET 0x5c
#define CAMERA_VIEW_PROJECTION_SLOT (0xdc / 4)
#define CAMERA_FOV_OFFSET 0x24
#define CAMERA_NEAR_OFFSET 0x34
#define CAMERA_FAR_OFFSET 0x38
// Screen texture +0x10 -> resource, +0x34 -> wrapper, +0x18 -> ID3D11Texture2D.
#define SCREEN_TEXTURE_RESOURCE 0x10
#define RESOURCE_WRAPPER 0x34
#define WRAPPER_TEXTURE 0x18
// camera_VS_PS's matrix parameters: present while +0x14 is set, the value at [+0x60], version at +0x10.
#define PARAM_PRESENT 0x14
#define PARAM_VERSION 0x10
#define PARAM_VALUE 0x60
#define PROJECTION_PARAM 0xbc
#define VIEW_PROJECTION_PARAM 0x120
#define INV_VIEW_PROJECTION_PARAM 0x184
#define INV_PROJECTION_PARAM 0x1e8
// Every sampler wrapper's create (vtbl+8, thiscall): its D3D11_SAMPLER_DESC at +0x8 goes to the device's cached creation.
#define SAMPLER_CREATE_RVA 0x14006e0
#define SAMPLER_DESC_OFFSET 0x8
// AMD's mip bias: log2(render / output) - 1, so -1 at native resolution.
#define MIP_BIAS -1.0f
// Object motion: the shaders' new constants go at the end of the engine's camera and wind blocks, added at the calls that
// finalize them (thiscall block, on every scene creation), so the engine lays them out, uploads them and binds them.
#define CAMERA_FINALIZE_CALL_RVA 0x13adabb
#define WIND_FINALIZE_CALL_RVA 0x13ae5c9
#define FINALIZE_RVA 0x13f8c20
#define ADD_PARAMETER_RVA 0x13cd770
#define TOUCH_RVA 0x1498040
#define FLOAT_PARAM_VTABLE_RVA 0x174942c
#define FLOAT2_PARAM_VTABLE_RVA 0x1749728
#define MATRIX_PARAM_VTABLE_RVA 0x1749700
#define SCENE_SHADERS_OFFSET 0xb18
#define CAMERA_BLOCK_OFFSET 0x62c
#define WIND_BLOCK_OFFSET 0x1004
// wind_VS's wind_direction: the float2 parameter at +0x28, its value pointer at +0x28.
#define WIND_DIRECTION_VALUE 0x50
#define SCENE_TIME_OFFSET 0x328
// The G-buffer view makes its targets in 0x114859b0 (thiscall view, again at an AA change); vtbl+0x70 hands out target i
// and +0x78 their count. With readable depth, indices 3 and 4 are free for the objects' motion and its depth stamp.
#define GBUFFER_TARGETS_RVA 0x14859b0
#define GBUFFER_DTOR_RVA 0x1448210
#define GBUFFER_TARGET_SLOT_RVA 0x176005c
#define GBUFFER_COUNT_SLOT_RVA 0x1760064
#define GBUFFER_COLOR_OFFSET 0x60
#define GBUFFER_LINEAR_DEPTH_OFFSET 0x58
#define GBUFFER_AA_LEVEL_OFFSET 0x204
#define ENGINE_NEW_RVA 0xf7440
#define SCREEN_TEXTURE_SIZE 0x34
#define SCREEN_TEXTURE_CTOR_RVA 0x13b6860
#define RC_GBUFFER 0x10
#define TARGET_BIND 0x84
#define FORMAT_R16G16_FLOAT 0x2c
#define FORMAT_R32_FLOAT 0xd
#define FORMAT_R8_UNORM 2
#define RESOURCE_RTV_OBJECT 0x4c
#define RTV_OBJECT_VIEW 0x20
// Rain and snow draw in Deferred Alpha: its target getter (vtbl+0x70, the light accumulation whatever the index) and its
// count (+0x78, 1); their shaders' constructor (thiscall shader; name, library) at its two calls, whose blend
// description (a D3D11_BLEND_DESC at +0x8 of the blend object at +0x490) becomes a state when the resources are made.
#define ALPHA_TARGET_SLOT_RVA 0x1760830
#define ALPHA_COUNT_SLOT_RVA 0x1760838
#define RAIN_SHADER_CALL_RVA 0x13abea8
#define SNOW_SHADER_CALL_RVA 0x13ac951
#define WEATHER_SHADER_CTOR_RVA 0x13bbaf0
#define WEATHER_BLEND_DESC 0x498
// The shader-cache loader (cdecl): ours are handed over by cache name as if read from the cache.
#define SHADER_LOAD_RVA 0x14050e0
#define DEVICE_NEW_BLOB_SLOT (0x88 / 4)
// Units: the previous frame's bones in each bone row's second half, the previous world after each instance record.
#define BONE_WIDTH_PUSH_RVA 0x14b26db
#define WEIGHTED_RECORD_SIZE_RVA 0x14b2b75
#define WEIGHTED_B_RECORD_SIZE_RVA 0x14b2aef
#define RIGID_RECORD_SIZE_RVA 0x14b2a59
#define RIGID_SHADER_CALL_RVA 0x13acde4
#define VTF_SHADER_CALL_RVA 0x13acdfc
#define RIGID_SHADER_CTOR_RVA 0x13b84d0
#define VTF_SHADER_CTOR_RVA 0x13ba080
#define ADD_ELEMENT_RVA 0x14dc380
#define SHADER_FORMAT 0x738
#define SHADER_STREAM_COUNT 0x4
#define SHADER_STREAMS 0x8
#define STREAM_ENTRY_SIZE 0x18
#define SEMANTIC_POSITION 0
#define SEMANTIC_TEXCOORD 3
#define ELEMENT_FLOAT4 3
#define RIGID_GATHER_SLOT_RVA 0x1769628
#define GATHERER_LIST 0x8
#define GATHERER_RECORD_SIZE (-4)
#define FILL_TEXTURE_RVA 0x14c5470
#define TEXTURE_WRAPPER_RVA 0x13e3500
#define GET_RENDERER_SLOT (0x84 / 4)
#define TEXTURE_ROWS_SLOT (0x64 / 4)
#define TEXTURE_MAP_SLOT (0x20 / 4)
#define TEXTURE_UNMAP_SLOT (0x24 / 4)
#define MAP_WRITE_DISCARD 4
#define DRAW_TRACKER_RVA 0x2cf740
#define BATCH_WORLD_CALL_RVA 0x2b4941
#define WORLD_MATRIX_RVA 0x50b5d0
#define BATCH_RVA 0x14bab30
#define BATCH_WORLD_STRIDE 0x30
#define BATCH_SOLDIERS 4096
#define MAX_CONTEXTS 128
// Imposters: the manager's add (thiscall; slot, ...) takes entry [manager+0x114+8*slot] of 0x2c bytes from +0x80114;
// the quad writer fills the entry's four 64-byte vertices.
#define IMPOSTER_ADD_RVA 0x148bda0
#define IMPOSTER_WRITER_RVA 0x1487340
#define IMPOSTER_CURSORS 0x114
#define IMPOSTER_ENTRIES 0x80114
#define IMPOSTER_ENTRY_SIZE 0x2c
#define IMPOSTER_ENTRY_VERTICES 0x28
#define IMPOSTER_SLOTS 0x10000
#define IMPOSTER_VERTEX_SIZE 0x40
// anim.z, tex.w, shadow_size.y: never read by the imposter shader for the animated imposters the engine makes.
#define IMPOSTER_MOVED_X 0x18
#define IMPOSTER_MOVED_Y 0x2c
#define IMPOSTER_MOVED_Z 0x34
// Banners (texture-blend meshes, through the rigid scene node): records of 0x194 bytes whose instance data is +0x150..
// +0x194; the shaders read only mesh_data.w, so mesh_data.y (+0x184, always 1) carries our slot from append to gather.
#define BANNER_SHADER_CALL_RVA 0x13ac667
#define BANNER_WIND_SHADER_CALL_RVA 0x13ac6be
#define BANNER_SHADER_CTOR_RVA 0x13a7200
#define BANNER_SHADER_FORMAT 0x178
#define BANNER_GATHERER_VTABLE_IMMEDIATE_RVA 0x14f31ed
#define GATHERER_VTABLE_ENTRIES 6
#define BANNER_SUBMIT_SLOT_RVA 0x176d554
#define BANNER_APPEND_SLOT_RVA 0x176aa58
#define BANNER_PARAMS_LAYERS 0x28
#define BANNER_INSTANCE 0x150
#define BANNER_INSTANCE_BYTES 0x44
#define BANNER_SLOT_FIELD 0x184
#define BANNER_SLOT_BIAS 2
#define BANNER_BITS 12
#define BANNER_SLOTS (1 << BANNER_BITS)
// Cloth (standard bearers' flags): the item's CPU vertices (0x20 bytes, position first) are handed to the dynamic node's
// add each frame from the same array, which the mesh's gather copies from mesh+0x8c.
#define CLOTH_SHADER_CALL_RVA 0x13ab4b5
#define CLOTH_SHADER_CTOR_RVA 0x1394de0
#define CLOTH_SHADER_FORMAT 0xa04
#define CLOTH_STRIDE_RVA 0x14e9c3a
#define CLOTH_ADD_SLOT_RVA 0x176c180
#define CLOTH_GATHER_SLOT_RVA 0x176bbe0
#define CLOTH_MESH_VERTICES 0x8c
#define CLOTH_VERTEX_COUNT_SLOT (0x38 / 4)
#define CLOTH_VERTEX 0x20
#define CLOTH_VERTEX_WIDE 0x2c
#define CLOTH_POSITION_BYTES 12
#define ELEMENT_FLOAT3 2
#define CLOTH_BITS 10
#define CLOTH_SLOTS (1 << CLOTH_BITS)
// Projectiles: rigid models (RIGID_MESH_V2, rigidstandard.hlsl, as RIGID_STANDARD's meshes) whose instance data
// (0x40: world, mesh_data) the rigid scene node's list hands out; the projectile display draws each through it.
#define RIGID_LEGACY_SHADER_CALL_RVA 0x13ac342
#define RIGID_LEGACY_SHADER_CTOR_RVA 0x13a5c90
#define RIGID_STANDARD_SHADER_CALL_RVA 0x13ac4b7
#define RIGID_STANDARD_SHADER_CTOR_RVA 0x13a6bf0
#define RIGID_SHADER_FORMAT 0x178
#define RIGID_GATHERER_VTABLE_IMMEDIATE_RVA 0x14ad8cf
#define RIGID_BASE_GATHERER_VTABLE_IMMEDIATE_RVA 0x14adb29
#define RIGID_MESH_VTABLE_RVA 0x176a304
#define RIGID_STANDARD_MESH_VTABLE_RVA 0x176a17c
#define GATHERER_MESH 0x4
#define RIGID_INSTANCE_BYTES 0x40
#define RIGID_APPEND_SLOT_RVA 0x176aa28
#define PROJECTILE_DRAW_CALL_RVA 0x2b908a
#define PROJECTILE_DRAW_RVA 0x2b9120
#define PROJECTILE_BITS 12
#define PROJECTILE_SLOTS (1 << PROJECTILE_BITS)
#define POSE_APPEND_RVA 0x14d9900
#define RIGID_APPEND_CALL_RVA 0x14b9b83
#define SKINNED_APPEND_CALL_RVA 0x14b9cc2
#define SKINNED_B_APPEND_CALL_RVA 0x14b9dc4
#define RIGID_APPEND_RVA 0x14bb480
#define SKINNED_APPEND_RVA 0x14bc670
#define SKINNED_B_APPEND_RVA 0x14bc680
#define NODE_POSE_COUNT 0x180
#define NODE_POSES 0x184
#define PALETTE_BONES 0x4
#define PALETTE_DATA 0x8
// The item's three world copies: the skinned record reads +0x54, the rigid one +0x9c; each kind's other is free.
#define ITEM_SKINNED_WORLD 0x54
#define ITEM_RIGID_WORLD 0x9c
#define ITEM_MESH 0xe8
// The emission's world argument, in its frame just above the item it builds.
#define ITEM_SOURCE_WORLD 0x144
#define WORLD_BYTES 48
#define RIGID_RECORD_BYTES 0x44
#define BONE_BYTES 48
#define PREVIOUS_BONES 4096
#define SOLDIER_BITS 16
#define SOLDIER_SLOTS (1 << SOLDIER_BITS)
#define PALETTE_SLOTS SOLDIER_SLOTS
#define WORLD_SLOTS 0x10000
#define SOLDIER_PARTS 24
#define NO_HISTORY 0xffffffffu
// At native resolution; upscaling takes FSR's own length for its ratio when that is longer.
#define JITTER_PHASES 32
#define MAX_FRAME_MS 250
#define FAILED_MESSAGE L"FSR could not start, so the game's own anti-aliasing is used this session. Details are in s2fix's log in perf-logs."
static const float upscale_ratios[FSR_MODES] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
static const char *const mode_names[FSR_MODES] = {"FSR 3.1 (AA)", "FSR 3.1 (Quality)", "FSR 3.1 (Balanced)", "FSR 3.1 (Performance)",
                                                  "FSR 3.1 (Ultra Performance)"};
static const wchar_t fsr_entries[] = L"|FSR 3.1 (AA)|FSR 3.1 (Quality)|FSR 3.1 (Balanced)|FSR 3.1 (Performance)|FSR 3.1 (Ultra Performance)";
// Return addresses of the screen-texture constructor for the targets drawn before A: the G-buffer's four (and linear
// depth), the light accumulation, Deferred Alpha's depth copy, SSAO's three, the refraction texture.
static const DWORD render_size_sites[] = {0x1485ae0, 0x1485c01, 0x1485c8d, 0x1485d19, 0x1485db7, 0x1485450,
                                          0x1485593, 0x140013c, 0x140022c, 0x140033e, 0x14858c5};

typedef HRESULT(WINAPI *CreateDeviceFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
                                        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef DWORD(__attribute__((thiscall)) *AaGetterFn)(BYTE *engine_object);
typedef void(__attribute__((thiscall)) *ViewsUpdateFn)(BYTE *scene, BYTE *mask, BYTE *flags);
typedef void(__attribute__((thiscall)) *RebuildFn)(BYTE *engine_object, DWORD flags);
typedef BYTE(__attribute__((thiscall)) *FramePresentFn)(BYTE *engine_object);
typedef void(__attribute__((thiscall)) *CameraWriterFn)(BYTE *frame_view, BYTE *camera, BYTE *block);
typedef BYTE(__attribute__((thiscall)) *ViewPassFn)(BYTE *view);
typedef BYTE(__attribute__((thiscall)) *SamplerCreateFn)(BYTE *wrapper);
typedef BYTE *(__attribute__((thiscall)) *ScreenTextureFn)(BYTE *scene);
typedef void(__attribute__((thiscall)) *MatrixFn)(BYTE *camera, float *out);
// The engine's shader parameters: vtable, 0, name, owner block, version, storage, then the type's own fields.
typedef struct {
    void *vtable;
    DWORD zero;
    const char *name;
    BYTE *owner;
    DWORD version;
    void *storage;
} ParamHead;
typedef struct {
    ParamHead head;
    float init;
    DWORD total, size;
    float *value;
} FloatParam;
typedef struct {
    ParamHead head;
    float init[2];
    DWORD total, size;
    float *value;
} Float2Param;
typedef struct {
    ParamHead head;
    float init[16];
    DWORD total, size;
    float *value;
} MatrixParam;
_Static_assert(sizeof(FloatParam) == 0x28 && sizeof(Float2Param) == 0x2c && sizeof(MatrixParam) == 0x64, "the engine's parameter sizes");
typedef struct {
    DWORD length, capacity;
    wchar_t *text;
} EngineWString;
typedef struct {
    float x, y;
    int width, height;
    BYTE follow;
} ScreenScale;
typedef void(__attribute__((thiscall)) *BlockFn)(BYTE *block);
typedef void(__attribute__((thiscall)) *AddParameterFn)(BYTE *block, void *param);
typedef void(__attribute__((thiscall)) *TouchFn)(void *param);
typedef BYTE *(__cdecl *EngineNewFn)(size_t size);
typedef BYTE *(__attribute__((thiscall)) *ScreenTextureCtorFn)(BYTE *texture, DWORD category, const ScreenScale *scale, DWORD bind, DWORD format, DWORD aa);
typedef void(__attribute__((thiscall)) *DeleteFn)(BYTE *object, DWORD flags);
typedef BYTE *(__attribute__((thiscall)) *TargetSlotFn)(BYTE *view, DWORD index);
typedef DWORD(__attribute__((thiscall)) *CountSlotFn)(BYTE *view);
typedef void(__attribute__((thiscall)) *ViewDtorFn)(BYTE *view);
typedef BYTE(__cdecl *ShaderLoadFn)(BYTE *device, void *source, EngineWString *second, EngineWString *first, void *defines, BYTE **out, BYTE check_only);
typedef BYTE *(__attribute__((thiscall)) *NewBlobFn)(BYTE *device, DWORD size);
typedef void *(__attribute__((thiscall)) *BlobDataFn)(BYTE *blob);
typedef BYTE *(__attribute__((thiscall)) *ShaderCtorFn)(BYTE *shader, BYTE *library);
typedef BYTE *(__attribute__((thiscall)) *WeatherShaderCtorFn)(BYTE *shader, void *name, BYTE *library);
typedef void(__attribute__((thiscall)) *AddElementFn)(BYTE *stream, DWORD semantic, DWORD type);
typedef struct {
    DWORD bytes, stride;
    BYTE *data;
} InstanceSpan;
typedef void(__attribute__((thiscall)) *GatherFn)(BYTE *gatherer, InstanceSpan *span, DWORD *ids);
typedef BYTE *(__attribute__((thiscall)) *ListElementFn)(BYTE *list, DWORD id);
typedef void(__attribute__((thiscall)) *FillTextureFn)(BYTE *node, BYTE *texture, DWORD first);
typedef DWORD(__attribute__((thiscall)) *RowsFn)(BYTE *texture);
typedef BYTE *(__attribute__((thiscall)) *WrapperFn)(BYTE *texture);
typedef BYTE *(__attribute__((thiscall)) *GetRendererFn)(BYTE *device);
typedef BYTE(__attribute__((thiscall)) *MapFn)(BYTE *wrapper, BYTE *renderer, DWORD subresource, DWORD type, BYTE **data, DWORD *pitch, DWORD a, DWORD b);
typedef void(__attribute__((thiscall)) *UnmapFn)(BYTE *wrapper, BYTE *renderer, DWORD subresource);
typedef DWORD(__attribute__((thiscall)) *ImposterAddFn)(BYTE *manager, DWORD slot, DWORD a2, const float *position, DWORD a4, DWORD a5, DWORD a6,
                                                        DWORD a7);
typedef void(__attribute__((thiscall)) *ImposterWriterFn)(BYTE *manager, BYTE *entry, DWORD a2, DWORD a3, DWORD a4, DWORD a5);
typedef BYTE *(__attribute__((thiscall)) *BannerShaderCtorFn)(BYTE *shader, DWORD a1, DWORD a2, DWORD a3, DWORD a4);
typedef void(__attribute__((thiscall)) *BannerSubmitFn)(BYTE *mesh, DWORD lod, const float *world, void *cull, BYTE *params, DWORD flags, DWORD parallel);
typedef DWORD(__attribute__((thiscall)) *ListAppendFn)(BYTE *list, BYTE *record, DWORD parallel);
typedef void(__attribute__((thiscall)) *ClothAddFn)(BYTE *node, DWORD index, const BYTE *vertices, DWORD count, void *pair);
typedef void(__attribute__((thiscall)) *ClothGatherFn)(BYTE *mesh, InstanceSpan *mapped);
typedef DWORD(__attribute__((thiscall)) *VertexCountFn)(BYTE *mesh);
typedef void(__attribute__((thiscall)) *ListPushFn)(BYTE *self, EngineWString *list);
typedef void(__attribute__((thiscall)) *EngineFn)(BYTE *engine_object);
typedef BYTE *(__attribute__((thiscall)) *SceneViewFn)(BYTE *scene);
typedef void(__attribute__((thiscall)) *ResolveFn)(BYTE *view);
typedef void(__attribute__((thiscall)) *ProjectileDrawFn)(BYTE *view, DWORD model);
typedef void *(__attribute__((thiscall)) *InstanceDataFn)(BYTE *list, DWORD id);
typedef void(__attribute__((thiscall)) *DrawTrackerFn)(BYTE *display, BYTE *tracker, BYTE *state, DWORD a3, DWORD a4, DWORD a5, DWORD a6);
typedef BYTE *(__attribute__((thiscall)) *WorldMatrixFn)(BYTE *tracker, BYTE *out);
typedef void(__attribute__((thiscall)) *BatchFn)(BYTE *node, const DWORD *handles, DWORD count, BYTE *pose, BYTE *worlds, BYTE *overrides, const DWORD *groups,
                                                 BYTE *masks, const float *values);
typedef DWORD(__attribute__((thiscall)) *PoseAppendFn)(BYTE *list, BYTE **palette, DWORD parallel);
typedef void(__attribute__((thiscall)) *AppendFn)(BYTE *node, BYTE *item, DWORD parallel);
// A frame's soldiers (keyed by tracker), their parts' worlds in emission order and their bones; each frame writes one of
// two and reads the other.
typedef struct {
    float world[12];
    BYTE *mesh;
} PartWorld;
typedef struct Soldier {
    DWORD frame, first, parts;
    struct Soldier *previous;
    BOOL imposter;
    float imposter_position[3];
    BYTE *palettes[2];
    DWORD bones[2], bone_counts[2];
} Soldier;
// A banner's key: its slot's layer list in the flag object, its mesh, and how many came before with both this frame.
typedef struct {
    BYTE *mesh;
    DWORD occurrence;
    float world[12], previous[12];
} Banner;
// A projectile part this frame: keyed by its display object and order there (for the next frame), and by its list and
// index there (for this frame's gather).
typedef struct {
    BYTE *view, *list;
    DWORD occurrence, index;
    float world[12], previous[12];
} Projectile;
typedef struct {
    BYTE *volatile soldier_keys[SOLDIER_SLOTS];
    Soldier soldiers[SOLDIER_SLOTS];
    BYTE *volatile palette_keys[PALETTE_SLOTS];
    Soldier *volatile palette_owners[PALETTE_SLOTS];
    PartWorld worlds[WORLD_SLOTS];
    volatile LONG worlds_used, touched, full;
    float *bones;
    DWORD bones_used, bones_capacity;
    BYTE *banner_keys[BANNER_SLOTS];
    Banner banners[BANNER_SLOTS];
    BOOL banners_touched;
    const BYTE *cloth_keys[CLOTH_SLOTS];
    DWORD cloth_counts[CLOTH_SLOTS], cloth_offsets[CLOTH_SLOTS];
    float *cloth_positions;
    DWORD cloth_used, cloth_capacity;
    BOOL cloth_touched;
    Projectile projectiles[PROJECTILE_SLOTS / 2];
    WORD by_view[PROJECTILE_SLOTS], by_index[PROJECTILE_SLOTS];
    DWORD projectile_count;
} History;
typedef struct {
    BOOL active;
    BYTE *tracker;
    Soldier *soldier;
    LONG soldiers, batched, parts, parts_previous, imposters, imposters_previous;
    BYTE *banner_layers, *banner_mesh, *projectile_view;
    DWORD projectile_parts;
} TrackerContext;
typedef struct {
    float moved[3];
    DWORD frame;
} ImposterMove;
#define SHARED_PALETTE ((Soldier *)1)

static const BYTE level_list_expected[] = {0x6a, 0x03, 0x8d, 0x44, 0x24, 0x20, 0x50};
static const BYTE caps_level_expected[] = {0x74, 0x0f}, caps_level_replacement[] = {0x7d, 0x0f};
static const BYTE aa_getter_prologue[] = {0x8b, 0x81, 0x30, 0x99, 0x06, 0x00};
static const BYTE frame_present_prologue[] = {0xff, 0x81, 0xcc, 0x9c, 0x06, 0x00};
static const BYTE camera_writer_prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0};
static const BYTE sampler_create_prologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x0d, 0x3c, 0xa9, 0xc9, 0x11};
static const BYTE camera_finalize_call[] = {0xe8, 0x60, 0xb1, 0x04, 0x00}, wind_finalize_call[] = {0xe8, 0x52, 0xa6, 0x04, 0x00};
static const BYTE gbuffer_targets_prologue[] = {0x83, 0xec, 0x20, 0x56, 0x8b, 0xf1};
static const BYTE gbuffer_dtor_prologue[] = {0x56, 0x8b, 0xf1, 0x57, 0x8b, 0x4e, 0x4c};
static const BYTE shader_load_prologue[] = {0xff, 0x74, 0x24, 0x1c, 0xff, 0x74, 0x24, 0x1c};
static const BYTE bone_width_push[] = {0x68, 0x00, 0x01, 0x00, 0x00}, bone_width_wide[] = {0x68, 0x00, 0x02, 0x00, 0x00};
static const BYTE record_size[] = {0x50, 0x00, 0x00, 0x00}, record_size_wide[] = {0x80, 0x00, 0x00, 0x00};
static const BYTE rigid_shader_call[] = {0xe8, 0xe7, 0xb6, 0x00, 0x00}, vtf_shader_call[] = {0xe8, 0x7f, 0xd2, 0x00, 0x00};
static const BYTE fill_texture_prologue[] = {0x83, 0xec, 0x0c, 0x80, 0xb9, 0xa0, 0x01, 0x00, 0x00, 0x00};
static const BYTE draw_tracker_prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
static const BYTE batch_world_call[] = {0xe8, 0x8a, 0x6c, 0x25, 0x00};
static const BYTE imposter_add_prologue[] = {0x56, 0x8b, 0x74, 0x24, 0x0c, 0x57};
static const BYTE imposter_writer_prologue[] = {0x83, 0xec, 0x4c, 0x8b, 0xc1};
static const BYTE banner_shader_call[] = {0xe8, 0x94, 0xab, 0xff, 0xff}, banner_wind_shader_call[] = {0xe8, 0x3d, 0xab, 0xff, 0xff};
static const BYTE cloth_shader_call[] = {0xe8, 0x26, 0x99, 0xfe, 0xff};
static const BYTE rigid_legacy_shader_call[] = {0xe8, 0x49, 0x99, 0xff, 0xff}, rigid_standard_shader_call[] = {0xe8, 0x34, 0xa7, 0xff, 0xff};
static const BYTE projectile_draw_call[] = {0xe8, 0x91, 0x00, 0x00, 0x00};
static const BYTE rain_shader_call[] = {0xe8, 0x43, 0xfc, 0x00, 0x00}, snow_shader_call[] = {0xe8, 0x9a, 0xf1, 0x00, 0x00};
static const BYTE aa_list_call[] = {0xe8, 0x13, 0xa8, 0xca, 0xff}, start_clamp_call[] = {0xe8, 0xb8, 0x80, 0xff, 0xff};
static const BYTE save_aa_call[] = {0xff, 0x90, 0xec, 0x00, 0x00, 0x00};
static const BYTE aa_branch[] = {0x0f, 0x84, 0xf3, 0x00, 0x00, 0x00};
static const BYTE screen_texture_prologue[] = {0x81, 0xec, 0xa8, 0x00, 0x00, 0x00};
static const BYTE resolve_prologue[] = {0x56, 0x8b, 0xf1, 0x8b, 0x8e, 0x8c, 0x01, 0x00, 0x00};
static const BYTE cloth_stride[] = {0x20, 0x00, 0x00, 0x00}, cloth_stride_wide[] = {0x2c, 0x00, 0x00, 0x00};
static const BYTE batch_prologue[] = {0x83, 0xec, 0x1c, 0xf3, 0x0f, 0x10, 0x0d, 0x78, 0x27, 0x5c, 0x11};
static const BYTE pose_append_prologue[] = {0x83, 0xec, 0x1c, 0x80, 0x7c, 0x24, 0x24, 0x00};
static const BYTE rigid_append_call[] = {0xe8, 0xf8, 0x18, 0x00, 0x00}, skinned_append_call[] = {0xe8, 0xa9, 0x29, 0x00, 0x00},
                  skinned_b_append_call[] = {0xe8, 0xb7, 0x28, 0x00, 0x00};
static const D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};

static const S2Api *api;
static BYTE *engine;
static BOOL enabled, group_ok, layout_ok, start_failed, failure_shown, failure_rebuild;
static float sharpness;
static D3D_FEATURE_LEVEL logged_level;
static AaGetterFn original_aa_getter;
static FramePresentFn original_frame_present;
static CameraWriterFn original_camera_writer;
static SamplerCreateFn original_sampler_create;
static ViewPassFn original_gbuffer_prepass, original_alpha_postpass, original_effects_postpass;
static S2FsrInit fsr_init;
static S2FsrCreate fsr_create;
static S2FsrFrameFn fsr_frame;
static S2FsrShutdown fsr_shutdown;
static S2FsrOpaque fsr_opaque;

// For diagnostics from another module: a switch per part, a callback around each FSR frame, and counts.
typedef void (*FsrFrameHookFn)(ID3D11DeviceContext *context, ID3D11Texture2D *color, ID3D11Texture2D *depth, const S2FsrFrame *frame, BOOL before);
enum {
    FSR_FRAMES,
    FSR_RESET_CONTEXT,
    FSR_RESET_SKIPPED,
    FSR_RESET_SLOW,
    FSR_RESET_CUT,
    FSR_UNIT_SOLDIERS,
    FSR_UNIT_BATCHED,
    FSR_UNIT_PARTS,
    FSR_UNIT_PARTS_PREVIOUS,
    FSR_UNIT_IMPOSTERS,
    FSR_UNIT_IMPOSTERS_PREVIOUS,
    FSR_BANNERS,
    FSR_BANNERS_PREVIOUS,
    FSR_CLOTHS,
    FSR_CLOTHS_PREVIOUS,
    FSR_AFTER_EFFECTS,
    FSR_REACTIVE,
    FSR_PROJECTILES,
    FSR_PROJECTILES_PREVIOUS,
    FSR_UNIT_ROWS,
    FSR_UNIT_ROWS_PREVIOUS,
    FSR_COUNTS
};
__declspec(dllexport) volatile LONG fsr_jitter = 1;
__declspec(dllexport) volatile LONG fsr_mip_bias = 1;
__declspec(dllexport) volatile LONG fsr_objects = 1;
__declspec(dllexport) volatile LONG fsr_units = 1;
// 0 leaves Deferred Alpha its one target: rain and snow mark nothing for FSR.
__declspec(dllexport) volatile LONG fsr_weather = 1;
// 0 hands the bone fill, the gathers and the soldier hooks back to the engine's own work, to time the wider layout.
__declspec(dllexport) volatile LONG fsr_fill = 1, fsr_gather = 1, fsr_track = 1;
// 0 leaves the engine its own shaders from the next shader load (an AA change).
__declspec(dllexport) volatile LONG fsr_shaders = 1;
__declspec(dllexport) FsrFrameHookFn volatile fsr_frame_hook;
__declspec(dllexport) volatile LONG fsr_counts[FSR_COUNTS];
// What the getter last answered, as the targets are made: FSR, and an upscaling mode (targets before A at the render
// size; read by the AA branch's stub).
static volatile BOOL fsr_on;
volatile LONG fsr_upscaling;
const void *fsr_branch_skip;
static ScreenTextureCtorFn original_screen_texture;
static AaGetterFn original_requested_aa;
static ResolveFn original_resolve;
static ID3D11Texture2D *fsr_target;
// Whether the samplers made since the last rebuild got the mip bias.
static LONG bias_applied = 1;
static LONG samplers_biased, samplers_seen;
// Main thread only, per frame.
static BOOL window_open;
// FSR runs after the particles (point B) when the effects views ran last frame, with a copy of the colour from before
// them for the reactive mask; else at point A.
static BOOL effects_seen, effects_ran, opaque_taken;
static float jitter_x, jitter_y;
static unsigned jitter_index;
static ID3D11Device *fsr_device;
static ID3D11Texture2D *fsr_color, *fsr_depth, *fsr_mask;
static float previous_view_projection[16];
static BOOL have_previous;
static DWORD last_frame;
static LARGE_INTEGER last_time;
// Object motion. The block never frees its parameters (the engine's are members), so ours are static: one scene at a time.
static MatrixParam prev_view_projection_param;
static Float2Param jitter_param, prev_wind_param;
static FloatParam prev_time_param;
static float saved_time, saved_wind[2];
static TargetSlotFn original_gbuffer_target;
static CountSlotFn original_gbuffer_count;
static ViewPassFn original_gbuffer_targets;
static ViewDtorFn original_gbuffer_dtor;
static ShaderLoadFn original_shader_load;
// The G-buffer's targets 3 and 4 (the objects' motion and its depth stamp), and the weather's mask (Deferred Alpha's target 1).
static BYTE *extra_targets[3], *extra_view;
static TargetSlotFn original_alpha_target;
static CountSlotFn original_alpha_count;
static ID3D11Texture2D *fsr_velocity;
static LONG shaders_served;
static History *histories[2];
static volatile LONG history_index;
static DWORD tracker_slot, history_frame = 1;
static TrackerContext *volatile contexts[MAX_CONTEXTS];
static volatile LONG context_count;
static BYTE *batch_trackers[BATCH_SOLDIERS];
static DWORD batch_recorded;
static BYTE *volatile batch_worlds;
static volatile DWORD batch_count;
static BatchFn original_batch;
static ImposterAddFn original_imposter_add;
static ImposterWriterFn original_imposter_writer;
static ImposterMove imposter_moves[IMPOSTER_SLOTS];
static void *banner_gatherer_vtable[GATHERER_VTABLE_ENTRIES];
static void *original_banner_gatherer_vtable;
static BannerSubmitFn original_banner_submit;
static ListAppendFn original_banner_append;
static SRWLOCK banner_lock = SRWLOCK_INIT;
static ClothAddFn original_cloth_add;
static void *rigid_gatherer_vtable[GATHERER_VTABLE_ENTRIES];
static void *original_rigid_gatherer_vtable, *original_rigid_base_gatherer_vtable;
static ListAppendFn original_scene_rigid_append;
static ViewPassFn original_alpha_prepass;
static ClothGatherFn original_cloth_gather;
static SRWLOCK cloth_lock = SRWLOCK_INIT;
static DrawTrackerFn original_draw_tracker;
static PoseAppendFn original_pose_append;
static FillTextureFn original_fill_texture;
static GatherFn original_rigid_gather;
static volatile LONG full_tables;

static BYTE *engine_object(void) {
    return *(BYTE **)(engine + ENGINE_GLOBAL_RVA);
}

static BYTE *render_device(void) {
    BYTE *device = *(BYTE **)(engine + RENDER_DEVICE_RVA);
    return device && *(void **)(device + DEVICE_INTERFACE_OFFSET) ? device : NULL;
}

static BOOL device_supports_fsr(void) {
    BYTE *device = render_device();
    return device && *(D3D_FEATURE_LEVEL *)(device + FEATURE_LEVEL_OFFSET) >= D3D_FEATURE_LEVEL_11_1;
}

static BOOL fsr_usable(void) {
    return group_ok && !start_failed && device_supports_fsr();
}

// The FSR mode the player chose in the AA options, or -1.
static int chosen_mode(void) {
    DWORD aa = *(DWORD *)(engine_object() + AA_SETTING_OFFSET);
    return aa >= FSR_AA_FIRST && aa < FSR_AA_FIRST + FSR_MODES ? (int)(aa - FSR_AA_FIRST) : -1;
}

static BOOL fsr_active(void) {
    return fsr_usable() && chosen_mode() >= 0;
}

static BOOL fsr_drawing(void) {
    return fsr_on && !start_failed;
}

// A history table filled up: what doesn't fit keeps previous = current.
static void table_full(LONG bit, const char *what) {
    if (!(InterlockedOr(&full_tables, bit) & bit)) api->log_line("fsr: the %s history is full; the rest get no motion of their own", what);
}

// The requested AA's getter (engine vtbl+0xec) answers as the active one's does, so nothing reads 5..9 as MSAA.
static DWORD __attribute__((thiscall)) requested_aa_hook(BYTE *object) {
    DWORD aa = original_requested_aa(object);
    return aa < FSR_AA_FIRST ? aa : fsr_active() ? 0 : *(DWORD *)(object + AA_MAX_OFFSET);
}

// What the game saves as gfx_aa: the player's choice, FSR's 5..9 included.
static DWORD __attribute__((thiscall)) saved_aa(BYTE *object) {
    return original_requested_aa(object);
}

static float mip_bias(void) {
    return MIP_BIAS - log2f(upscale_ratios[chosen_mode() > 0 ? chosen_mode() : 0]);
}

// s2prof.ini is two folders up from fixes\fsr.dll.
static void module_folder(wchar_t *path) {
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)module_folder, &self);
    if (!GetModuleFileNameW(self, path, MAX_PATH)) path[0] = 0;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
}

static void read_settings(void) {
    wchar_t path[MAX_PATH], text[32];
    module_folder(path);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
    wcscat(path, L"\\s2prof.ini");
    enabled = GetPrivateProfileIntW(L"fsr", L"enabled", 1, path) != 0;
    GetPrivateProfileStringW(L"fsr", L"sharpness", L"0.5", text, sizeof text / sizeof *text, path);
    sharpness = (float)wcstod(text, NULL);
    if (!(sharpness > 0)) sharpness = 0;
    if (sharpness > 1) sharpness = 1;
}

static void fsr_log(const char *line) {
    api->log_line("fsr: %s", line);
}

static const char *load_library(void) {
    wchar_t path[MAX_PATH];
    module_folder(path);
    wcscat(path, L"\\fsr\\s2fsr.dll");
    HMODULE library = LoadLibraryW(path);
    if (!library) return "fixes\\fsr\\s2fsr.dll could not be loaded";
    fsr_init = (S2FsrInit)(void *)GetProcAddress(library, "s2fsr_init");
    fsr_create = (S2FsrCreate)(void *)GetProcAddress(library, "s2fsr_create");
    fsr_frame = (S2FsrFrameFn)(void *)GetProcAddress(library, "s2fsr_frame");
    fsr_shutdown = (S2FsrShutdown)(void *)GetProcAddress(library, "s2fsr_shutdown");
    fsr_opaque = (S2FsrOpaque)(void *)GetProcAddress(library, "s2fsr_opaque");
    return fsr_init && fsr_create && fsr_frame && fsr_shutdown && fsr_opaque ? NULL : "fixes\\fsr\\s2fsr.dll lacks an export";
}

// A runtime without Direct3D 11.1 fails a list holding 11_1 with E_INVALIDARG, and the engine would retry forever. The
// runtime refuses it before any driver loads, so WARP answers without the GPU driver's start-up time.
static HRESULT probe_11_1(void) {
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    CreateDeviceFn create = d3d11 ? (CreateDeviceFn)(void *)GetProcAddress(d3d11, "D3D11CreateDevice") : NULL;
    if (!create) return E_NOINTERFACE;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1, created;
    HRESULT result = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &level, 1, D3D11_SDK_VERSION, NULL, &created, NULL);
    FreeLibrary(d3d11);
    return result;
}

// The box waits for the frame's end: it pumps messages, and the engine's window procedure would run mid-frame.
static void stop(const char *why) {
    api->log_line("fsr: %s; FSR is off this session and the game's own anti-aliasing returns", why);
    start_failed = TRUE;
    failure_rebuild = TRUE;
}

// A rebuild may also take the device away, and FSR's objects would keep it alive: all of it goes, made again next frame.
static void release_targets(void) {
    if (fsr_device) fsr_shutdown();
    fsr_device = NULL;
    fsr_color = fsr_depth = fsr_velocity = fsr_mask = fsr_target = NULL;
}

static void resources_created(void) {
    BYTE *device = render_device();
    if (!device) return;
    if (samplers_biased) api->log_line("fsr: mip bias %.2f on %ld of %ld samplers made", (double)mip_bias(), samplers_biased, samplers_seen);
    samplers_biased = samplers_seen = 0;
    static BOOL shaders_logged;
    if (shaders_served && !shaders_logged) api->log_line("fsr: %ld of our shaders handed to the engine", shaders_served), shaders_logged = TRUE;
    shaders_served = 0;
    D3D_FEATURE_LEVEL level = *(D3D_FEATURE_LEVEL *)(device + FEATURE_LEVEL_OFFSET);
    if (level == logged_level) return;
    logged_level = level;
    api->log_line("fsr: the device runs at feature level %x (shader level %ld)%s", level, *(LONG *)(device + SHADER_LEVEL_OFFSET),
                  level >= D3D_FEATURE_LEVEL_11_1 ? "" : "; FSR needs 11_1, so it stays off");
}

// FSR's entries are the engine's AA off; one whose FSR failed to start gives the device's highest AA instead.
static DWORD __attribute__((thiscall)) aa_getter_hook(BYTE *object) {
    DWORD aa = original_aa_getter(object);
    fsr_on = fsr_active();
    fsr_upscaling = fsr_on && chosen_mode() > 0;
    return aa < FSR_AA_FIRST ? aa : fsr_on ? 0 : *(DWORD *)(object + AA_MAX_OFFSET);
}

// Material textures: trilinear or anisotropic, wrapping; post-processing, shadow and UI samplers clamp or compare.
static BOOL material_sampler(const D3D11_SAMPLER_DESC *desc) {
    return !(desc->Filter & 0x80) && (desc->Filter & 1) && desc->AddressU == D3D11_TEXTURE_ADDRESS_WRAP && desc->MaxLOD > 0;
}

static BYTE __attribute__((thiscall)) sampler_create_hook(BYTE *wrapper) {
    D3D11_SAMPLER_DESC *desc = (D3D11_SAMPLER_DESC *)(wrapper + SAMPLER_DESC_OFFSET);
    samplers_seen++;
    if (!fsr_active() || !bias_applied || !material_sampler(desc)) return original_sampler_create(wrapper);
    samplers_biased++;
    float bias = desc->MipLODBias;
    desc->MipLODBias = bias + mip_bias();
    BYTE result = original_sampler_create(wrapper);
    desc->MipLODBias = bias;
    return result;
}

// The graphics options' own sequence for an AA change, with the setting unchanged and the getter answering anew.
static void rebuild_for_aa(void) {
    BYTE mask[0x20] = {0}, flags[4] = {0, 0, 0, 1};
    mask[AA_CHANGED_MASK] = 1;
    BYTE *object = engine_object();
    BYTE *scene = *(BYTE **)(object + SCENE_OFFSET);
    BYTE *textures = *(BYTE **)(engine + TEXTURE_MANAGER_RVA);
    if (scene) {
        if (textures) textures[TEXTURE_MANAGER_BUSY_OFFSET] = 0;
        ((ViewsUpdateFn)(engine + VIEWS_UPDATE_RVA))(scene, mask, flags);
        if (textures) textures[TEXTURE_MANAGER_BUSY_OFFSET] = 1;
    }
    ((RebuildFn)(*(void ***)object)[REBUILD_SLOT])(object, AA_CHANGED_REBUILD);
}

static void next_history(void);

static BYTE __attribute__((thiscall)) frame_present_hook(BYTE *object) {
    BYTE result = original_frame_present(object);
    if (start_failed && !failure_shown) {
        failure_shown = TRUE;
        MessageBoxW(NULL, FAILED_MESSAGE, L"Shogun 2 fixes", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }
    window_open = FALSE;
    if (histories[0]) next_history();
    effects_seen = effects_ran, effects_ran = opaque_taken = FALSE;
    // FSR failing to start, or the mip bias switched: not while the device is being made again or the engine's own
    // recovery is pending.
    if (!render_device() || object[RESET_FLAG_OFFSET]) return result;
    BOOL wanted = fsr_active();
    if (!failure_rebuild && wanted == fsr_on && (!wanted || fsr_mip_bias == bias_applied)) return result;
    failure_rebuild = FALSE;
    bias_applied = fsr_mip_bias;
    api->log_line("fsr: %s; the engine rebuilds its targets and samplers",
                  !wanted ? "off, the game's anti-aliasing back" : bias_applied ? "on with the mip bias" : "on without the mip bias");
    have_previous = FALSE;
    fsr_on = wanted;
    rebuild_for_aa();
    return result;
}

static BYTE *main_scene(void) {
    return *(BYTE **)(engine_object() + SCENE_OFFSET);
}

static float halton(unsigned index, unsigned base) {
    float fraction = 1, result = 0;
    for (; index; index /= base) {
        fraction /= (float)base;
        result += fraction * (float)(index % base);
    }
    return result;
}

static ID3D11Texture2D *screen_texture(BYTE *screen) {
    BYTE *resource = screen ? *(BYTE **)(screen + SCREEN_TEXTURE_RESOURCE) : NULL;
    BYTE *wrapper = resource ? *(BYTE **)(resource + RESOURCE_WRAPPER) : NULL;
    return wrapper ? *(ID3D11Texture2D **)(wrapper + WRAPPER_TEXTURE) : NULL;
}

static ID3D11RenderTargetView *screen_target(BYTE *screen) {
    BYTE *resource = *(BYTE **)(screen + SCREEN_TEXTURE_RESOURCE);
    return *(ID3D11RenderTargetView **)(*(BYTE **)(resource + RESOURCE_RTV_OBJECT) + RTV_OBJECT_VIEW);
}

static void head(ParamHead *param, DWORD vtable_rva, const char *name, BYTE *block) {
    *param = (ParamHead){engine + vtable_rva, 0, name, block, 0, NULL};
}

static void __attribute__((thiscall)) camera_finalize(BYTE *block) {
    MatrixParam *m = &prev_view_projection_param;
    head(&m->head, MATRIX_PARAM_VTABLE_RVA, "prev_view_projection", block);
    for (int i = 0; i < 16; i++) m->init[i] = i % 5 == 0;
    m->total = 0, m->size = 0x40, m->value = NULL;
    head(&jitter_param.head, FLOAT2_PARAM_VTABLE_RVA, "g_camera_jitter", block);
    jitter_param.init[0] = jitter_param.init[1] = 0, jitter_param.total = 0, jitter_param.size = 8, jitter_param.value = NULL;
    head(&prev_time_param.head, FLOAT_PARAM_VTABLE_RVA, "prev_time_in_sec", block);
    prev_time_param.init = 0, prev_time_param.total = 0, prev_time_param.size = 4, prev_time_param.value = NULL;
    AddParameterFn add = (AddParameterFn)(engine + ADD_PARAMETER_RVA);
    add(block, m), add(block, &jitter_param), add(block, &prev_time_param);
    ((BlockFn)(engine + FINALIZE_RVA))(block);
}

static void __attribute__((thiscall)) wind_finalize(BYTE *block) {
    head(&prev_wind_param.head, FLOAT2_PARAM_VTABLE_RVA, "g_prev_wind_direction", block);
    prev_wind_param.init[0] = prev_wind_param.init[1] = 0, prev_wind_param.total = 0, prev_wind_param.size = 8, prev_wind_param.value = NULL;
    ((AddParameterFn)(engine + ADD_PARAMETER_RVA))(block, &prev_wind_param);
    ((BlockFn)(engine + FINALIZE_RVA))(block);
}

// The engine's own write: the value where the parameter is bound, then the touch that makes its block upload.
static void write_param(ParamHead *param, const void *value, size_t size) {
    if (!param->storage) return;
    memcpy(param->storage, value, size);
    ((TouchFn)(engine + TOUCH_RVA))(param);
}

static void render_scale(float *x, float *y);

static BYTE *make_target(BYTE *view, DWORD format) {
    ScreenScale scale = {1, 1, -1, -1, 1};
    if (fsr_upscaling) render_scale(&scale.x, &scale.y);
    BYTE *texture = ((EngineNewFn)(engine + ENGINE_NEW_RVA))(SCREEN_TEXTURE_SIZE);
    return ((ScreenTextureCtorFn)(engine + SCREEN_TEXTURE_CTOR_RVA))(texture, RC_GBUFFER, &scale, TARGET_BIND, format,
                                                                     *(DWORD *)(view + GBUFFER_AA_LEVEL_OFFSET));
}

// As the view deletes its own targets: the deleting destructor of the sub-object at +8.
static void drop_extra_targets(void) {
    for (int i = 0; i < 3; i++) {
        if (!extra_targets[i]) continue;
        BYTE *sub = extra_targets[i] + 8;
        ((DeleteFn)(**(void ***)sub))(sub, 1);
        extra_targets[i] = NULL;
    }
    extra_view = NULL;
}

static BYTE __attribute__((thiscall)) gbuffer_targets_hook(BYTE *view) {
    BYTE made = original_gbuffer_targets(view);
    if (extra_view == view) drop_extra_targets();
    // Only where this scene's blocks carry our constants: a scene made before s2fix installed keeps camera motion.
    BYTE *shaders = *(BYTE **)(view + VIEW_SCENE_OFFSET) + SCENE_SHADERS_OFFSET;
    BOOL constants = prev_view_projection_param.head.owner == shaders + CAMERA_BLOCK_OFFSET && prev_wind_param.head.owner == shaders + WIND_BLOCK_OFFSET;
    if (made && constants && fsr_active() && !*(void **)(view + GBUFFER_LINEAR_DEPTH_OFFSET)) {
        extra_targets[0] = make_target(view, FORMAT_R16G16_FLOAT);
        extra_targets[1] = make_target(view, FORMAT_R32_FLOAT);
        extra_targets[2] = make_target(view, FORMAT_R8_UNORM);
        extra_view = view;
        api->log_line("fsr: G-buffer targets 3 and 4 made for the objects' motion, and the weather's mask");
    }
    return made;
}

static void __attribute__((thiscall)) gbuffer_dtor_hook(BYTE *view) {
    if (extra_view == view) drop_extra_targets();
    original_gbuffer_dtor(view);
}

static BYTE *__attribute__((thiscall)) gbuffer_target_slot(BYTE *view, DWORD index) {
    return index >= 3 && view == extra_view ? extra_targets[index - 3] : original_gbuffer_target(view, index);
}

static DWORD __attribute__((thiscall)) gbuffer_count_slot(BYTE *view) {
    DWORD count = original_gbuffer_count(view);
    return view == extra_view && count == 3 ? 5 : count;
}

// While FSR draws the main scene, Deferred Alpha's target 1 is the weather's mask, which our rain and snow shaders write.
static BYTE *weather_mask(BYTE *view) {
    BYTE *scene = *(BYTE **)(view + VIEW_SCENE_OFFSET);
    return fsr_weather && fsr_drawing() && extra_targets[2] && *(BYTE **)(scene + SCENE_GBUFFER_VIEW_OFFSET) == extra_view ? extra_targets[2] : NULL;
}

static BYTE *__attribute__((thiscall)) alpha_target_slot(BYTE *view, DWORD index) {
    BYTE *mask = index == 1 ? weather_mask(view) : NULL;
    return mask ? mask : original_alpha_target(view, index);
}

static DWORD __attribute__((thiscall)) alpha_count_slot(BYTE *view) {
    DWORD count = original_alpha_count(view);
    return count == 1 && weather_mask(view) ? 2 : count;
}

// The mask adds up (ONE, ONE), so a pixel rain or snow covers ends at 1 whatever its alpha; the colour keeps its blend.
// Only where FSR can run: a feature level 10_0 device refuses a blend with independent targets.
static BYTE *__attribute__((thiscall)) weather_shader_ctor(BYTE *shader, void *name, BYTE *library) {
    BYTE *made = ((WeatherShaderCtorFn)(engine + WEATHER_SHADER_CTOR_RVA))(shader, name, library);
    static BOOL logged;
    if (!render_device() && !logged) logged = TRUE, api->log_line("fsr: rain and snow's shaders made before the device; they mark nothing for FSR");
    if (!device_supports_fsr()) return made;
    D3D11_BLEND_DESC *blend = (D3D11_BLEND_DESC *)(shader + WEATHER_BLEND_DESC);
    blend->IndependentBlendEnable = TRUE;
    blend->RenderTarget[1] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                                                              D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
    return made;
}

static const struct object_blob *our_shader(const EngineWString *path) {
    if (!path || !path->text) return NULL;
    DWORD start = path->length;
    while (start && path->text[start - 1] != L'/' && path->text[start - 1] != L'\\') start--;
    DWORD length = path->length - start;
    for (size_t i = 0; i < sizeof object_blobs / sizeof *object_blobs; i++) {
        const char *name = object_blobs[i].name;
        DWORD c = 0;
        while (c < length && name[c] && towlower(path->text[start + c]) == (wchar_t)tolower((unsigned char)name[c])) c++;
        if (c == length && !name[c]) return &object_blobs[i];
    }
    return NULL;
}

// A skinned unit vertex shader not ours would read the widened bone rows as 256 wide.
static void check_bone_reader(const EngineWString *path) {
    char name[MAX_PATH];
    DWORD n = 0;
    for (DWORD i = 0; path && path->text && i < path->length && n + 1 < sizeof name; i++) name[n++] = (char)towlower(path->text[i]);
    name[n] = 0;
    if (strstr(name, "variant_vtf_supported") && strstr(name, "variantweightedvtf.hlsl_vs_"))
        api->log_line("fsr: the engine asked for %s, a unit shader reading the bone rows that FSR doesn't have; those units will draw wrong", name);
}

// Ours are the shaders that also write their objects' motion or read the widened bone rows, served only with the whole
// layout installed; asked again (check only) the engine keeps what it made.
static BYTE __cdecl shader_load_hook(BYTE *device, void *source, EngineWString *second, EngineWString *first, void *defines, BYTE **out, BYTE check_only) {
    const struct object_blob *ours = layout_ok && fsr_shaders ? our_shader(first) : NULL;
    if (!ours) {
        if (!check_only) check_bone_reader(first);
        return original_shader_load(device, source, second, first, defines, out, check_only);
    }
    if (check_only) return 1;
    BYTE *blob = ((NewBlobFn)(*(void ***)device)[DEVICE_NEW_BLOB_SLOT])(device, ours->size);
    memcpy(((BlobDataFn)(*(void ***)blob)[0])(blob), ours->dxbc, ours->size);
    *out = blob;
    shaders_served++;
    return 1;
}

// Each frame on main before the G-buffer's split pass: its two new targets cleared (no motion, a stamp no depth has) and
// the shaders' new constants written. Without a previous frame, previous = current: no object motion.
static void prepare_objects(BYTE *view, BYTE *scene) {
    BYTE *device = render_device();
    ID3D11DeviceContext *context = *(ID3D11DeviceContext **)(device + IMMEDIATE_CONTEXT_OFFSET);
    static const float zero[4] = {0}, no_depth[4] = {-1, -1, -1, -1};
    ID3D11DeviceContext_ClearRenderTargetView(context, screen_target(extra_targets[0]), zero);
    ID3D11DeviceContext_ClearRenderTargetView(context, screen_target(extra_targets[1]), no_depth);
    ID3D11DeviceContext_ClearRenderTargetView(context, screen_target(extra_targets[2]), zero);
    BYTE *camera = scene + SCENE_CAMERA_OFFSET;
    float view_projection[16];
    ((MatrixFn)(*(void ***)camera)[CAMERA_VIEW_PROJECTION_SLOT])(camera, view_projection);
    D3D11_TEXTURE2D_DESC size;
    ID3D11Texture2D_GetDesc(screen_texture(*(BYTE **)(view + GBUFFER_COLOR_OFFSET)), &size);
    float jitter[2] = {2 * jitter_x / (float)size.Width, -2 * jitter_y / (float)size.Height};
    float now = *(float *)(scene + SCENE_TIME_OFFSET);
    float *wind = *(float **)(scene + SCENE_SHADERS_OFFSET + WIND_BLOCK_OFFSET + WIND_DIRECTION_VALUE);
    if (!have_previous) saved_time = now, saved_wind[0] = wind[0], saved_wind[1] = wind[1];
    write_param(&prev_view_projection_param.head, have_previous ? previous_view_projection : view_projection, sizeof view_projection);
    write_param(&jitter_param.head, jitter, sizeof jitter);
    write_param(&prev_time_param.head, &saved_time, sizeof saved_time);
    write_param(&prev_wind_param.head, saved_wind, sizeof saved_wind);
    saved_time = now, saved_wind[0] = wind[0], saved_wind[1] = wind[1];
}

static BYTE __attribute__((thiscall)) gbuffer_prepass_hook(BYTE *view) {
    BYTE *scene = *(BYTE **)(view + VIEW_SCENE_OFFSET);
    if (fsr_drawing() && scene == main_scene()) {
        float ratio = upscale_ratios[chosen_mode()];
        unsigned phases = (unsigned)ceilf(8 * ratio * ratio);
        unsigned phase = jitter_index++ % (phases > JITTER_PHASES ? phases : JITTER_PHASES) + 1;
        jitter_x = fsr_jitter ? halton(phase, 2) - 0.5f : 0;
        jitter_y = fsr_jitter ? halton(phase, 3) - 0.5f : 0;
        window_open = TRUE;
        if (view == extra_view) prepare_objects(view, scene);
    }
    return original_gbuffer_prepass(view);
}

static float *param_value(BYTE *block, DWORD param) {
    return *(DWORD *)(block + param + PARAM_PRESENT) ? *(float **)(block + param + PARAM_VALUE) : NULL;
}

// clip = M * v with M row-major: the jitter adds a * row 3 to row 0 and b * row 3 to row 1; an inverse gets the inverse
// of that on its right, column 3 less a * column 0 and b * column 1.
static void jitter_matrix(float *m, float a, float b) {
    if (!m) return;
    for (int c = 0; c < 4; c++) {
        m[c] += a * m[12 + c];
        m[4 + c] += b * m[12 + c];
    }
}

static void jitter_inverse(float *m, float a, float b) {
    if (!m) return;
    for (int r = 0; r < 4; r++) m[4 * r + 3] -= a * m[4 * r] + b * m[4 * r + 1];
}

static void __attribute__((thiscall)) camera_writer_hook(BYTE *frame_view, BYTE *camera, BYTE *block) {
    DWORD inverse_versions[2] = {*(DWORD *)(block + INV_PROJECTION_PARAM + PARAM_VERSION), *(DWORD *)(block + INV_VIEW_PROJECTION_PARAM + PARAM_VERSION)};
    original_camera_writer(frame_view, camera, block);
    if (!window_open || camera != main_scene() + SCENE_CAMERA_OFFSET) return;
    ID3D11Texture2D *color = screen_texture(*(BYTE **)(((SceneViewFn)(engine + LIGHTING_VIEW_RVA))(main_scene()) + LIGHT_ACCUMULATION));
    D3D11_TEXTURE2D_DESC size;
    if (!color) return;
    ID3D11Texture2D_GetDesc(color, &size);
    float a = 2 * jitter_x / (float)size.Width, b = -2 * jitter_y / (float)size.Height;
    jitter_matrix(param_value(block, PROJECTION_PARAM), a, b);
    jitter_matrix(param_value(block, VIEW_PROJECTION_PARAM), a, b);
    // The engine skips both inverses for a near-singular matrix; adjust only what it wrote this call.
    if (*(DWORD *)(block + INV_PROJECTION_PARAM + PARAM_VERSION) != inverse_versions[0]) jitter_inverse(param_value(block, INV_PROJECTION_PARAM), a, b);
    if (*(DWORD *)(block + INV_VIEW_PROJECTION_PARAM + PARAM_VERSION) != inverse_versions[1])
        jitter_inverse(param_value(block, INV_VIEW_PROJECTION_PARAM), a, b);
    if (!fsr_upscaling) return;
    // The views before A draw at the render size and take their texture coordinates from these.
    float *screen = *(DWORD *)(block + SCREEN_SIZE_PARAM + PARAM_PRESENT) ? *(float **)(block + SCREEN_SIZE_PARAM + FLOAT2_PARAM_VALUE) : NULL;
    float *inverse = *(DWORD *)(block + INV_SCREEN_SIZE_PARAM + PARAM_PRESENT) ? *(float **)(block + INV_SCREEN_SIZE_PARAM + FLOAT2_PARAM_VALUE) : NULL;
    if (screen) screen[0] = (float)size.Width, screen[1] = (float)size.Height;
    if (inverse) inverse[0] = 1.0f / (float)(size.Width - 1), inverse[1] = 1.0f / (float)(size.Height - 1);
}

static BOOL invert(const float *m, double *out) {
    double a[16], inv[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (fabs(det) < 1e-12) return FALSE;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return TRUE;
}

// wtfix's rule: the point at the screen's centre, half-way in depth, lands more than a quarter screen away in the
// previous frame.
static BOOL camera_cut(const float *view_projection) {
    double inv[16], p[4], q[4], v[4] = {0, 0, 0.5, 1};
    if (!invert(view_projection, inv)) return TRUE;
    for (int r = 0; r < 4; r++) p[r] = inv[4 * r] * v[0] + inv[4 * r + 1] * v[1] + inv[4 * r + 2] * v[2] + inv[4 * r + 3] * v[3];
    for (int r = 0; r < 4; r++)
        q[r] = previous_view_projection[4 * r] * p[0] + previous_view_projection[4 * r + 1] * p[1] + previous_view_projection[4 * r + 2] * p[2] +
               previous_view_projection[4 * r + 3] * p[3];
    return q[3] <= 0 || fabs(q[0] / q[3]) > 0.5 || fabs(q[1] / q[3]) > 0.5;
}

static void run_fsr(BYTE *scene, BOOL reactive) {
    BYTE *device = render_device();
    ID3D11Device *d3d = *(ID3D11Device **)(device + DEVICE_INTERFACE_OFFSET);
    ID3D11DeviceContext *context = *(ID3D11DeviceContext **)(device + IMMEDIATE_CONTEXT_OFFSET);
    if (d3d != fsr_device) {
        release_targets();
        fsr_device = d3d;
        fsr_color = fsr_depth = fsr_velocity = fsr_mask = fsr_target = NULL;
        int error = fsr_init(d3d, fsr_log);
        if (error) {
            char why[64];
            snprintf(why, sizeof why, "starting FSR failed (%d)", error);
            fsr_device = NULL;
            stop(why);
            return;
        }
    }
    BYTE *gbuffer = *(BYTE **)(scene + SCENE_GBUFFER_VIEW_OFFSET), *lighting = ((SceneViewFn)(engine + LIGHTING_VIEW_RVA))(scene);
    ID3D11Texture2D *color = screen_texture(*(BYTE **)(lighting + LIGHT_ACCUMULATION));
    ID3D11Texture2D *target = fsr_upscaling && *(BYTE **)(lighting + LIGHT_RESOLVED) ? screen_texture(*(BYTE **)(lighting + LIGHT_RESOLVED)) : NULL;
    ID3D11Texture2D *depth = gbuffer ? screen_texture(*(BYTE **)(gbuffer + GBUFFER_DEPTH_OFFSET)) : NULL;
    if (!color || !depth) return;
    BOOL objects = fsr_objects && extra_view == gbuffer;
    ID3D11Texture2D *velocity = objects ? screen_texture(extra_targets[0]) : NULL;
    ID3D11Texture2D *stamp = objects ? screen_texture(extra_targets[1]) : NULL;
    ID3D11Texture2D *mask = extra_view == gbuffer ? screen_texture(extra_targets[2]) : NULL;
    BOOL reset = FALSE, made = FALSE;
    if (color != fsr_color || depth != fsr_depth || velocity != fsr_velocity || mask != fsr_mask || target != fsr_target) {
        fsr_color = fsr_depth = fsr_velocity = fsr_mask = NULL;
        int error = fsr_create(color, depth, velocity, stamp, mask, target, sharpness);
        if (error) {
            char why[64];
            snprintf(why, sizeof why, "creating FSR's context failed (%d)", error);
            stop(why);
            return;
        }
        fsr_color = color, fsr_depth = depth, fsr_velocity = velocity, fsr_mask = mask, fsr_target = target;
        reset = made = TRUE;
        fsr_counts[FSR_RESET_CONTEXT]++;
    }
    BYTE *camera = scene + SCENE_CAMERA_OFFSET;
    S2FsrFrame frame = {0};
    ((MatrixFn)(*(void ***)camera)[CAMERA_VIEW_PROJECTION_SLOT])(camera, frame.view_projection);
    DWORD counter = *(DWORD *)(engine_object() + FRAME_COUNTER_OFFSET);
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    double ms = 1000.0 * (double)(now.QuadPart - last_time.QuadPart) / (double)frequency.QuadPart;
    if (!reset && (!have_previous || counter != last_frame + 1)) reset = TRUE, fsr_counts[FSR_RESET_SKIPPED]++;
    if (!reset && (ms <= 0 || ms > MAX_FRAME_MS)) reset = TRUE, fsr_counts[FSR_RESET_SLOW]++;
    if (!reset && camera_cut(frame.view_projection)) reset = TRUE, fsr_counts[FSR_RESET_CUT]++;
    memcpy(frame.previous_view_projection, have_previous ? previous_view_projection : frame.view_projection, sizeof frame.previous_view_projection);
    frame.jitter_x = jitter_x, frame.jitter_y = jitter_y;
    frame.frame_ms = (float)(ms > 0 && ms <= MAX_FRAME_MS ? ms : 16.7);
    frame.camera_near = *(float *)(camera + CAMERA_NEAR_OFFSET);
    frame.camera_far = *(float *)(camera + CAMERA_FAR_OFFSET);
    frame.fov_y = *(float *)(camera + CAMERA_FOV_OFFSET);
    frame.reset = reset;
    // A context made just now has no copy of this frame's colour.
    frame.reactive = reactive && !made;
    fsr_counts[FSR_REACTIVE] += frame.reactive;
    FsrFrameHookFn hook = fsr_frame_hook;
    if (hook) hook(context, color, depth, &frame, TRUE);
    int error = fsr_frame(context, &frame);
    if (hook) hook(context, color, depth, &frame, FALSE);
    fsr_counts[FSR_FRAMES]++;
    if (error) {
        char why[64];
        snprintf(why, sizeof why, "FSR failed on a frame (%d)", error);
        fsr_color = fsr_depth = fsr_velocity = fsr_mask = fsr_target = NULL;
        stop(why);
        return;
    }
    memcpy(previous_view_projection, frame.view_projection, sizeof previous_view_projection);
    have_previous = TRUE;
    last_frame = counter;
    last_time = now;
}

static BYTE __attribute__((thiscall)) alpha_postpass_hook(BYTE *view) {
    BYTE result = original_alpha_postpass(view);
    BYTE *scene = *(BYTE **)(view + VIEW_SCENE_OFFSET);
    if (!window_open || scene != main_scene() || !fsr_drawing()) {
        window_open = FALSE;
        return result;
    }
    // The particles draw with the jittered camera too, so the window stays open until B.
    if (effects_seen) return result;
    run_fsr(scene, opaque_taken);
    window_open = FALSE;
    return result;
}

// Before Deferred Alpha's translucent draws: the colour FSR's reactive mask compares the frame's colour with.
static BYTE __attribute__((thiscall)) alpha_prepass_hook(BYTE *view) {
    BYTE result = original_alpha_prepass(view);
    BYTE *scene = *(BYTE **)(view + VIEW_SCENE_OFFSET);
    if (!window_open || scene != main_scene() || !fsr_drawing() || !fsr_color) return result;
    ID3D11Texture2D *color = screen_texture(*(BYTE **)(((SceneViewFn)(engine + LIGHTING_VIEW_RVA))(scene) + LIGHT_ACCUMULATION));
    opaque_taken = color == fsr_color;
    if (opaque_taken) fsr_opaque(*(ID3D11DeviceContext **)(render_device() + IMMEDIATE_CONTEXT_OFFSET));
    return result;
}

static BYTE __attribute__((thiscall)) effects_postpass_hook(BYTE *view) {
    BYTE result = original_effects_postpass(view);
    BYTE *scene = *(BYTE **)(view + VIEW_SCENE_OFFSET);
    if (scene != main_scene()) return result;
    effects_ran = TRUE;
    if (window_open && fsr_drawing()) run_fsr(scene, opaque_taken), fsr_counts[FSR_AFTER_EFFECTS]++;
    window_open = FALSE;
    return result;
}

static DWORD hash_slot(const void *key) {
    return ((uint32_t)(uintptr_t)key * 2654435761u) >> (32 - SOLDIER_BITS);
}

// Each tracker is drawn by one thread a frame, so a soldier's entry has one writer; the slot is claimed with a CAS.
static Soldier *soldier_entry(History *history, BYTE *tracker, BOOL *inserted) {
    for (DWORD i = hash_slot(tracker), n = 0; n < SOLDIER_SLOTS; i = (i + 1) & (SOLDIER_SLOTS - 1), n++) {
        BYTE *key = history->soldier_keys[i];
        if (!key) {
            if (!inserted) return NULL;
            key = InterlockedCompareExchangePointer((PVOID volatile *)&history->soldier_keys[i], tracker, NULL);
            if (!key) {
                *inserted = TRUE;
                return &history->soldiers[i];
            }
        }
        if (key == tracker) return &history->soldiers[i];
    }
    return NULL;
}

// Per thread; the counts are summed at the frame's end.
static TrackerContext *tracker_context(void) {
    TrackerContext *context = TlsGetValue(tracker_slot);
    if (context) return context;
    context = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *context);
    TlsSetValue(tracker_slot, context);
    LONG index = InterlockedIncrement(&context_count) - 1;
    if (index < MAX_CONTEXTS) contexts[index] = context;
    return context;
}

// The tracker's entry this frame, made at its first palette or part; drawn twice in a frame, it keeps adding to it.
static Soldier *frame_soldier(BYTE *tracker, TrackerContext *context, BOOL batched) {
    History *history = histories[history_index];
    if (history->full) return NULL;
    BOOL inserted = FALSE;
    Soldier *soldier = soldier_entry(history, tracker, &inserted);
    if (!soldier) {
        history->full = 1;
        table_full(1, "soldiers'");
        return NULL;
    }
    if (!inserted) return soldier;
    history->touched = 1;
    soldier->frame = history_frame;
    soldier->parts = 0;
    soldier->first = NO_HISTORY;
    soldier->imposter = FALSE;
    soldier->palettes[0] = soldier->palettes[1] = NULL;
    soldier->bones[0] = soldier->bones[1] = NO_HISTORY;
    Soldier *previous = soldier_entry(histories[history_index ^ 1], tracker, NULL);
    soldier->previous = previous && previous->frame + 1 == history_frame ? previous : NULL;
    context->soldiers++;
    context->batched += batched;
    return soldier;
}

static Soldier *drawn_soldier(TrackerContext *context) {
    if (!context->soldier) context->soldier = frame_soldier(context->tracker, context, FALSE);
    return context->soldier;
}

static void __attribute__((thiscall)) draw_tracker_hook(BYTE *display, BYTE *tracker, BYTE *state, DWORD a3, DWORD a4, DWORD a5, DWORD a6) {
    if (!fsr_track) {
        original_draw_tracker(display, tracker, state, a3, a4, a5, a6);
        return;
    }
    TrackerContext *context = tracker_context();
    BOOL track = fsr_on && fsr_units;
    if (track) context->active = TRUE, context->tracker = tracker, context->soldier = NULL;
    original_draw_tracker(display, tracker, state, a3, a4, a5, a6);
    if (track) context->active = FALSE;
}

// The batch loop takes each soldier's world in order, then hands the batch its worlds; on main.
static BYTE *__attribute__((thiscall)) batch_world_hook(BYTE *tracker, BYTE *out) {
    if (batch_recorded < BATCH_SOLDIERS) batch_trackers[batch_recorded] = tracker;
    batch_recorded++;
    return ((WorldMatrixFn)(engine + WORLD_MATRIX_RVA))(tracker, out);
}

static void __attribute__((thiscall)) batch_hook(BYTE *node, const DWORD *handles, DWORD count, BYTE *pose, BYTE *worlds, BYTE *overrides, const DWORD *groups,
                                                 BYTE *masks, const float *values) {
    if (fsr_on && fsr_units && count == batch_recorded && count <= BATCH_SOLDIERS) batch_count = count, batch_worlds = worlds;
    original_batch(node, handles, count, pose, worlds, overrides, groups, masks, values);
    batch_worlds = NULL;
    batch_recorded = 0;
}

// A batch's part comes from the world its soldier has in the batch's array, which the emission keeps above the item.
static Soldier *batched_soldier(BYTE *item, BYTE *worlds, TrackerContext *context) {
    DWORD offset = (DWORD)(*(BYTE **)(item + ITEM_SOURCE_WORLD) - worlds);
    if (offset >= batch_count * BATCH_WORLD_STRIDE || offset % BATCH_WORLD_STRIDE) return NULL;
    return frame_soldier(batch_trackers[offset / BATCH_WORLD_STRIDE], context, TRUE);
}

// A palette two soldiers share (a shared pose) gets no previous bones.
static DWORD __attribute__((thiscall)) pose_append_hook(BYTE *list, BYTE **palette_ref, DWORD parallel) {
    DWORD key = original_pose_append(list, palette_ref, parallel);
    TrackerContext *context = TlsGetValue(tracker_slot);
    Soldier *soldier = context && context->active ? drawn_soldier(context) : NULL;
    if (!soldier) return key;
    BYTE *palette = *palette_ref;
    if (soldier->palettes[0] != palette && soldier->palettes[1] != palette) {
        if (soldier->palettes[1]) return key;
        soldier->palettes[soldier->palettes[0] != NULL] = palette;
    }
    History *history = histories[history_index];
    for (DWORD i = hash_slot(palette), n = 0; n < PALETTE_SLOTS; i = (i + 1) & (PALETTE_SLOTS - 1), n++) {
        BYTE *found = history->palette_keys[i];
        if (!found) found = InterlockedCompareExchangePointer((PVOID volatile *)&history->palette_keys[i], palette, NULL);
        if (found && found != palette) continue;
        Soldier *owner = InterlockedCompareExchangePointer((PVOID volatile *)&history->palette_owners[i], soldier, NULL);
        if (owner && owner != soldier) history->palette_owners[i] = SHARED_PALETTE;
        break;
    }
    return key;
}

// Parts are matched by their order in the soldier's emission and checked by mesh; anything else keeps previous = current,
// which the engine's copies already hold.
static void note_part(BYTE *item, DWORD world_offset, DWORD previous_offset) {
    if (!fsr_track) return;
    TrackerContext *context = tracker_context();
    BYTE *worlds = batch_worlds;
    Soldier *soldier = context->active ? drawn_soldier(context) : worlds ? batched_soldier(item, worlds, context) : NULL;
    if (!soldier || soldier->parts >= SOLDIER_PARTS) return;
    if (soldier->first == NO_HISTORY) {
        LONG first = InterlockedExchangeAdd(&histories[history_index]->worlds_used, SOLDIER_PARTS);
        if (first + SOLDIER_PARTS > WORLD_SLOTS) {
            table_full(2, "soldiers' parts");
            return;
        }
        soldier->first = first;
    }
    PartWorld *part = &histories[history_index]->worlds[soldier->first + soldier->parts];
    DWORD index = soldier->parts++;
    memcpy(part->world, item + world_offset, WORLD_BYTES);
    part->mesh = *(BYTE **)(item + ITEM_MESH);
    context->parts++;
    Soldier *previous = soldier->previous;
    if (!previous || index >= previous->parts) return;
    PartWorld *old = &histories[history_index ^ 1]->worlds[previous->first + index];
    if (old->mesh != part->mesh) return;
    memcpy(item + previous_offset, old->world, WORLD_BYTES);
    context->parts_previous++;
}

// An imposter's entry is the slot's cursor before the add; its movement since the last frame is kept for the writer.
static DWORD __attribute__((thiscall)) imposter_add_hook(BYTE *manager, DWORD slot, DWORD a2, const float *position, DWORD a4, DWORD a5, DWORD a6,
                                                         DWORD a7) {
    TrackerContext *context = TlsGetValue(tracker_slot);
    DWORD *cursor = (DWORD *)(manager + IMPOSTER_CURSORS + 8 * slot);
    DWORD entry = *cursor;
    DWORD result = original_imposter_add(manager, slot, a2, position, a4, a5, a6, a7);
    Soldier *soldier = context && context->active && *cursor == entry + 1 ? drawn_soldier(context) : NULL;
    if (!soldier) return result;
    ImposterMove *move = &imposter_moves[entry];
    Soldier *previous = soldier->previous;
    for (int i = 0; i < 3; i++) move->moved[i] = previous && previous->imposter ? previous->imposter_position[i] - position[i] : 0;
    move->frame = history_frame;
    memcpy(soldier->imposter_position, position, sizeof soldier->imposter_position);
    soldier->imposter = TRUE;
    context->imposters++;
    context->imposters_previous += previous && previous->imposter;
    return result;
}

// After the engine writes the quad (on its pool threads), the movement into the components our shader reads; 0 without one.
static void __attribute__((thiscall)) imposter_writer_hook(BYTE *manager, BYTE *entry, DWORD a2, DWORD a3, DWORD a4, DWORD a5) {
    original_imposter_writer(manager, entry, a2, a3, a4, a5);
    if (!fsr_track) return;
    DWORD index = (DWORD)(entry - manager - IMPOSTER_ENTRIES) / IMPOSTER_ENTRY_SIZE;
    const ImposterMove *move = imposter_moves[index].frame == history_frame ? &imposter_moves[index] : NULL;
    BYTE *vertex = *(BYTE **)(entry + IMPOSTER_ENTRY_VERTICES);
    for (int v = 0; v < 4; v++, vertex += IMPOSTER_VERTEX_SIZE) {
        *(float *)(vertex + IMPOSTER_MOVED_X) = move ? move->moved[0] : 0;
        *(float *)(vertex + IMPOSTER_MOVED_Y) = move ? move->moved[1] : 0;
        *(float *)(vertex + IMPOSTER_MOVED_Z) = move ? move->moved[2] : 0;
    }
}

static void __attribute__((thiscall)) rigid_append(BYTE *node, BYTE *item, DWORD parallel) {
    note_part(item, ITEM_RIGID_WORLD, ITEM_SKINNED_WORLD);
    ((AppendFn)(engine + RIGID_APPEND_RVA))(node, item, parallel);
}

static void __attribute__((thiscall)) skinned_append(BYTE *node, BYTE *item, DWORD parallel) {
    note_part(item, ITEM_SKINNED_WORLD, ITEM_RIGID_WORLD);
    ((AppendFn)(engine + SKINNED_APPEND_RVA))(node, item, parallel);
}

static void __attribute__((thiscall)) skinned_b_append(BYTE *node, BYTE *item, DWORD parallel) {
    note_part(item, ITEM_SKINNED_WORLD, ITEM_RIGID_WORLD);
    ((AppendFn)(engine + SKINNED_B_APPEND_RVA))(node, item, parallel);
}

// The rigid record as the engine gathers it, then the previous world the free copy holds.
static void __attribute__((thiscall)) rigid_gather(BYTE *gatherer, InstanceSpan *span, DWORD *ids) {
    if (!fsr_gather) {
        original_rigid_gather(gatherer, span, ids);
        return;
    }
    BYTE *list = *(BYTE **)(gatherer + GATHERER_LIST);
    DWORD size = *(DWORD *)(gatherer + GATHERER_RECORD_SIZE), count = span->bytes / span->stride;
    BYTE *out = span->data;
    for (DWORD i = 0; i < count; i++, out += size) {
        BYTE *item = ((ListElementFn)(*(void ***)list)[1])(list, ids[i]);
        memcpy(out, item + ITEM_RIGID_WORLD, RIGID_RECORD_BYTES);
        memcpy(out + RIGID_RECORD_BYTES, item + ITEM_SKINNED_WORLD, WORLD_BYTES);
    }
}

static const BYTE *previous_bones(BYTE *palette, const BYTE *current, DWORD bones) {
    if (!fsr_on || !fsr_units) return current;
    History *history = histories[history_index];
    Soldier *soldier = NULL;
    for (DWORD i = hash_slot(palette), n = 0; n < PALETTE_SLOTS && history->palette_keys[i]; i = (i + 1) & (PALETTE_SLOTS - 1), n++)
        if (history->palette_keys[i] == palette) {
            soldier = history->palette_owners[i];
            break;
        }
    if (!soldier || soldier == SHARED_PALETTE) return current;
    int k = soldier->palettes[1] == palette;
    if (soldier->bones[k] == NO_HISTORY) {
        DWORD floats = bones * BONE_BYTES / sizeof(float);
        if (history->bones_used + floats > history->bones_capacity) {
            DWORD capacity = history->bones_capacity ? history->bones_capacity * 2 : 0x100000;
            while (capacity < history->bones_used + floats) capacity *= 2;
            history->bones = realloc(history->bones, capacity * sizeof(float)), history->bones_capacity = capacity;
        }
        memcpy(history->bones + history->bones_used, current, bones * BONE_BYTES);
        soldier->bones[k] = history->bones_used, soldier->bone_counts[k] = bones;
        history->bones_used += floats;
    }
    Soldier *previous = soldier->previous;
    if (!previous || previous->bones[k] == NO_HISTORY || previous->bone_counts[k] != bones) return current;
    fsr_counts[FSR_UNIT_ROWS_PREVIOUS]++;
    return (const BYTE *)(histories[history_index ^ 1]->bones + previous->bones[k]);
}

// The engine's fill (WRITE_DISCARD, this frame's bones from texel 0) with the previous frame's from texel 256.
static void __attribute__((thiscall)) fill_texture_hook(BYTE *node, BYTE *texture, DWORD first) {
    if (!fsr_fill) {
        original_fill_texture(node, texture, first);
        return;
    }
    DWORD rows = ((RowsFn)(*(void ***)texture)[TEXTURE_ROWS_SLOT])(texture), count = *(DWORD *)(node + NODE_POSE_COUNT);
    if (count - first < rows) rows = count - first;
    BYTE *device = *(BYTE **)(engine + RENDER_DEVICE_RVA);
    BYTE *renderer = ((GetRendererFn)(*(void ***)device)[GET_RENDERER_SLOT])(device);
    BYTE *wrapper = ((WrapperFn)(engine + TEXTURE_WRAPPER_RVA))(texture);
    BYTE *data = NULL;
    DWORD pitch = 0;
    if (!((MapFn)(*(void ***)wrapper)[TEXTURE_MAP_SLOT])(wrapper, renderer, 0, MAP_WRITE_DISCARD, &data, &pitch, 0, 0)) return;
    BYTE **poses = *(BYTE ***)(node + NODE_POSES);
    for (DWORD r = 0; r < rows; r++) {
        BYTE *palette = poses[first + r], *row = data + (size_t)r * pitch;
        DWORD bones = *(DWORD *)(palette + PALETTE_BONES);
        if (bones > PREVIOUS_BONES / BONE_BYTES) bones = PREVIOUS_BONES / BONE_BYTES;
        const BYTE *current = *(BYTE **)(palette + PALETTE_DATA);
        memcpy(row, current, bones * BONE_BYTES);
        memcpy(row + PREVIOUS_BONES, previous_bones(palette, current, bones), bones * BONE_BYTES);
    }
    fsr_counts[FSR_UNIT_ROWS] += rows;
    ((UnmapFn)(*(void ***)wrapper)[TEXTURE_UNMAP_SLOT])(wrapper, renderer, 0);
}

// At a frame's end, on main: the frame just drawn becomes the previous one.
static void next_history(void) {
    LONG count = context_count < MAX_CONTEXTS ? context_count : MAX_CONTEXTS;
    for (LONG i = 0; i < count; i++) {
        TrackerContext *context = contexts[i];
        if (!context) continue;
        fsr_counts[FSR_UNIT_SOLDIERS] += context->soldiers, fsr_counts[FSR_UNIT_BATCHED] += context->batched;
        fsr_counts[FSR_UNIT_PARTS] += context->parts, fsr_counts[FSR_UNIT_PARTS_PREVIOUS] += context->parts_previous;
        fsr_counts[FSR_UNIT_IMPOSTERS] += context->imposters, fsr_counts[FSR_UNIT_IMPOSTERS_PREVIOUS] += context->imposters_previous;
        context->soldiers = context->batched = context->parts = context->parts_previous = context->imposters = context->imposters_previous = 0;
    }
    batch_recorded = 0;
    history_frame++;
    LONG next = history_index ^ 1;
    History *history = histories[next];
    if (history->touched) {
        memset((void *)history->soldier_keys, 0, sizeof history->soldier_keys);
        memset((void *)history->palette_keys, 0, sizeof history->palette_keys);
        memset((void *)history->palette_owners, 0, sizeof history->palette_owners);
        history->touched = 0;
    }
    if (history->banners_touched) memset(history->banner_keys, 0, sizeof history->banner_keys), history->banners_touched = 0;
    if (history->cloth_touched) memset((void *)history->cloth_keys, 0, sizeof history->cloth_keys), history->cloth_touched = 0;
    if (history->projectile_count) memset(history->by_view, 0, sizeof history->by_view), memset(history->by_index, 0, sizeof history->by_index);
    history->projectile_count = 0;
    history->cloth_used = 0;
    history->full = 0;
    history->worlds_used = 0, history->bones_used = 0;
    history_index = next;
}

// The previous world as three more instance elements, after the vanilla ones: at 0x48 for skinned units (the record is
// one copy from the item, so the free rigid copy follows it), at 0x44 for rigid ones (written by our gather).
static void add_previous_world(BYTE *format, DWORD semantic) {
    BYTE *stream = *(BYTE **)(format + SHADER_STREAMS) + (*(DWORD *)(format + SHADER_STREAM_COUNT) - 1) * STREAM_ENTRY_SIZE;
    for (int i = 0; i < 3; i++) ((AddElementFn)(engine + ADD_ELEMENT_RVA))(stream, semantic, ELEMENT_FLOAT4);
}

static BYTE *__attribute__((thiscall)) rigid_shader_ctor(BYTE *shader, BYTE *library) {
    BYTE *result = ((ShaderCtorFn)(engine + RIGID_SHADER_CTOR_RVA))(shader, library);
    add_previous_world(shader + SHADER_FORMAT, SEMANTIC_POSITION);
    return result;
}

static BYTE *__attribute__((thiscall)) vtf_shader_ctor(BYTE *shader, BYTE *library) {
    BYTE *result = ((ShaderCtorFn)(engine + VTF_SHADER_CTOR_RVA))(shader, library);
    add_previous_world(shader + SHADER_FORMAT, SEMANTIC_TEXCOORD);
    return result;
}

// Both banner shaders: the plain one's instance size is every banner mesh's record size, and both draw them.
static BYTE *__attribute__((thiscall)) banner_shader_ctor(BYTE *shader, DWORD a1, DWORD a2, DWORD a3, DWORD a4) {
    BYTE *result = ((BannerShaderCtorFn)(engine + BANNER_SHADER_CTOR_RVA))(shader, a1, a2, a3, a4);
    add_previous_world(shader + BANNER_SHADER_FORMAT, SEMANTIC_POSITION);
    return result;
}

static BYTE *__attribute__((thiscall)) cloth_shader_ctor(BYTE *shader, BYTE *owner) {
    BYTE *result = ((ShaderCtorFn)(engine + CLOTH_SHADER_CTOR_RVA))(shader, owner);
    BYTE *format = shader + CLOTH_SHADER_FORMAT;
    BYTE *stream = *(BYTE **)(format + SHADER_STREAMS) + (*(DWORD *)(format + SHADER_STREAM_COUNT) - 1) * STREAM_ENTRY_SIZE;
    ((AddElementFn)(engine + ADD_ELEMENT_RVA))(stream, SEMANTIC_TEXCOORD, ELEMENT_FLOAT3);
    return result;
}

static DWORD cloth_slot(const History *history, const BYTE *vertices) {
    DWORD i = ((uint32_t)(uintptr_t)vertices * 2654435761u) >> (32 - CLOTH_BITS);
    for (DWORD n = 0; n < CLOTH_SLOTS && history->cloth_keys[i] && history->cloth_keys[i] != vertices; n++) i = (i + 1) & (CLOTH_SLOTS - 1);
    return i;
}

// A cloth's vertex positions this frame, keyed by its array, which the item keeps from frame to frame.
static void __attribute__((thiscall)) cloth_add(BYTE *node, DWORD index, const BYTE *vertices, DWORD count, void *pair) {
    original_cloth_add(node, index, vertices, count, pair);
    if (!fsr_on || !fsr_units || !vertices) return;
    History *history = histories[history_index];
    AcquireSRWLockExclusive(&cloth_lock);
    DWORD slot = cloth_slot(history, vertices), floats = count * 3;
    if (!history->cloth_keys[slot] && history->cloth_used + floats > history->cloth_capacity) {
        DWORD capacity = history->cloth_capacity ? history->cloth_capacity * 2 : 0x40000;
        while (capacity < history->cloth_used + floats) capacity *= 2;
        history->cloth_positions = realloc(history->cloth_positions, capacity * sizeof(float)), history->cloth_capacity = capacity;
    }
    if (!history->cloth_keys[slot]) {
        float *out = history->cloth_positions + history->cloth_used;
        for (DWORD v = 0; v < count; v++) memcpy(out + 3 * v, vertices + v * CLOTH_VERTEX, CLOTH_POSITION_BYTES);
        history->cloth_keys[slot] = vertices, history->cloth_counts[slot] = count, history->cloth_offsets[slot] = history->cloth_used;
        history->cloth_used += floats, history->cloth_touched = TRUE;
        fsr_counts[FSR_CLOTHS]++;
        const History *last = histories[history_index ^ 1];
        DWORD old = cloth_slot(last, vertices);
        fsr_counts[FSR_CLOTHS_PREVIOUS] += last->cloth_keys[old] == vertices && last->cloth_counts[old] == count;
    }
    ReleaseSRWLockExclusive(&cloth_lock);
}

// The engine's vertices, each followed by its position last frame (its position now without one).
static void __attribute__((thiscall)) cloth_gather(BYTE *mesh, InstanceSpan *mapped) {
    DWORD count = ((VertexCountFn)(*(void ***)mesh)[CLOTH_VERTEX_COUNT_SLOT])(mesh);
    const BYTE *vertices = *(const BYTE **)(mesh + CLOTH_MESH_VERTICES);
    const History *last = histories[history_index ^ 1];
    DWORD slot = cloth_slot(last, vertices);
    const float *previous = last->cloth_keys[slot] == vertices && last->cloth_counts[slot] == count ? last->cloth_positions + last->cloth_offsets[slot] : NULL;
    BYTE *out = mapped->data;
    for (DWORD v = 0; v < count; v++, out += CLOTH_VERTEX_WIDE) {
        memcpy(out, vertices + v * CLOTH_VERTEX, CLOTH_VERTEX);
        memcpy(out + CLOTH_VERTEX, previous ? previous + 3 * v : (const void *)(vertices + v * CLOTH_VERTEX), CLOTH_POSITION_BYTES);
    }
}

static BYTE *__attribute__((thiscall)) rigid_legacy_shader_ctor(BYTE *shader, BYTE *library) {
    BYTE *result = ((ShaderCtorFn)(engine + RIGID_LEGACY_SHADER_CTOR_RVA))(shader, library);
    add_previous_world(shader + RIGID_SHADER_FORMAT, SEMANTIC_POSITION);
    return result;
}

// The same file's gbuffer entries: our vertex shader is served for both.
static BYTE *__attribute__((thiscall)) rigid_standard_shader_ctor(BYTE *shader, BYTE *library) {
    BYTE *result = ((ShaderCtorFn)(engine + RIGID_STANDARD_SHADER_CTOR_RVA))(shader, library);
    add_previous_world(shader + RIGID_SHADER_FORMAT, SEMANTIC_POSITION);
    return result;
}

static DWORD projectile_hash(const void *a, DWORD b) {
    return ((uint32_t)(uintptr_t)a * 2654435761u ^ b * 40503u) >> (32 - PROJECTILE_BITS);
}

static const Projectile *projectile_by_view(const History *history, const BYTE *view, DWORD occurrence) {
    for (DWORD i = projectile_hash(view, occurrence), n = 0; n < PROJECTILE_SLOTS && history->by_view[i]; i = (i + 1) & (PROJECTILE_SLOTS - 1), n++) {
        const Projectile *p = &history->projectiles[history->by_view[i] - 1];
        if (p->view == view && p->occurrence == occurrence) return p;
    }
    return NULL;
}

static const Projectile *projectile_by_index(const History *history, const BYTE *list, DWORD index) {
    for (DWORD i = projectile_hash(list, index), n = 0; n < PROJECTILE_SLOTS && history->by_index[i]; i = (i + 1) & (PROJECTILE_SLOTS - 1), n++) {
        const Projectile *p = &history->projectiles[history->by_index[i] - 1];
        if (p->list == list && p->index == index) return p;
    }
    return NULL;
}

static void index_projectile(WORD *table, DWORD slot, WORD entry) {
    while (table[slot]) slot = (slot + 1) & (PROJECTILE_SLOTS - 1);
    table[slot] = entry;
}

// On the display's thread, serially: each projectile's parts in drawing order.
static void __attribute__((thiscall)) projectile_draw(BYTE *view, DWORD model) {
    TrackerContext *context = fsr_on && fsr_units ? tracker_context() : NULL;
    if (context) context->projectile_view = view, context->projectile_parts = 0;
    ((ProjectileDrawFn)(engine + PROJECTILE_DRAW_RVA))(view, model);
    if (context) context->projectile_view = NULL;
}

static DWORD __attribute__((thiscall)) scene_rigid_append(BYTE *list, BYTE *record, DWORD parallel) {
    DWORD index = original_scene_rigid_append(list, record, parallel);
    TrackerContext *context = TlsGetValue(tracker_slot);
    History *history = histories[history_index];
    if (!context || !context->projectile_view || parallel) return index;
    if (history->projectile_count == PROJECTILE_SLOTS / 2) {
        table_full(4, "projectiles'");
        return index;
    }
    Projectile *p = &history->projectiles[history->projectile_count];
    p->view = context->projectile_view, p->occurrence = context->projectile_parts++, p->list = list, p->index = index;
    memcpy(p->world, ((InstanceDataFn)(*(void ***)list)[0])(list, index), WORLD_BYTES);
    // A display object freed and made again at the same address is most often another model.
    const Projectile *old = projectile_by_view(histories[history_index ^ 1], p->view, p->occurrence);
    if (old && old->list != list) old = NULL;
    memcpy(p->previous, old ? old->world : p->world, WORLD_BYTES);
    WORD entry = (WORD)++history->projectile_count;
    index_projectile(history->by_view, projectile_hash(p->view, p->occurrence), entry);
    index_projectile(history->by_index, projectile_hash(list, index), entry);
    fsr_counts[FSR_PROJECTILES]++;
    fsr_counts[FSR_PROJECTILES_PREVIOUS] += old != NULL;
    return index;
}

// Every rigid scene mesh's gather: the engine's copy, and for the two widened classes the previous world after it (a
// projectile's last frame, else this frame's).
static void __attribute__((thiscall)) scene_rigid_gather(BYTE *gatherer, InstanceSpan *span, DWORD *ids) {
    BYTE *list = *(BYTE **)(gatherer + GATHERER_LIST), *mesh = *(BYTE **)(gatherer + GATHERER_MESH);
    DWORD size = *(DWORD *)(gatherer + GATHERER_RECORD_SIZE), count = span->bytes / span->stride;
    DWORD vtable = (DWORD)(*(BYTE **)mesh - engine);
    BOOL wide = size == RIGID_INSTANCE_BYTES + WORLD_BYTES && (vtable == RIGID_MESH_VTABLE_RVA || vtable == RIGID_STANDARD_MESH_VTABLE_RVA);
    if (!wide && !fsr_gather) {
        ((GatherFn)(*(void ***)&original_rigid_gatherer_vtable)[1])(gatherer, span, ids);
        return;
    }
    const History *history = histories[history_index];
    BYTE *out = span->data;
    for (DWORD i = 0; i < count; i++, out += size) {
        BYTE *data = ((InstanceDataFn)(*(void ***)list)[0])(list, ids[i]);
        if (!wide || !fsr_gather) {
            memcpy(out, data, wide ? RIGID_INSTANCE_BYTES : size);
            continue;
        }
        const Projectile *p = history->projectile_count ? projectile_by_index(history, list, ids[i]) : NULL;
        memcpy(out, data, RIGID_INSTANCE_BYTES);
        memcpy(out + RIGID_INSTANCE_BYTES, p ? (const void *)p->previous : data, WORLD_BYTES);
    }
}

static DWORD banner_hash(const BYTE *layers, const BYTE *mesh) {
    return ((uint32_t)(uintptr_t)layers * 2654435761u ^ (uint32_t)(uintptr_t)mesh * 40503u) >> (32 - BANNER_BITS);
}

static const Banner *banner_find(const History *history, const BYTE *layers, const BYTE *mesh, DWORD occurrence) {
    for (DWORD i = banner_hash(layers, mesh), n = 0; n < BANNER_SLOTS && history->banner_keys[i]; i = (i + 1) & (BANNER_SLOTS - 1), n++)
        if (history->banner_keys[i] == layers && history->banners[i].mesh == mesh && history->banners[i].occurrence == occurrence) return &history->banners[i];
    return NULL;
}

// This frame's entry for a banner, with last frame's world as its previous (or this frame's without one).
static DWORD banner_entry(BYTE *layers, BYTE *mesh, const float *world) {
    History *history = histories[history_index];
    AcquireSRWLockExclusive(&banner_lock);
    DWORD occurrence = 0, i = banner_hash(layers, mesh), n = 0;
    for (; n < BANNER_SLOTS && history->banner_keys[i]; i = (i + 1) & (BANNER_SLOTS - 1), n++)
        occurrence += history->banner_keys[i] == layers && history->banners[i].mesh == mesh;
    if (n == BANNER_SLOTS) {
        ReleaseSRWLockExclusive(&banner_lock);
        table_full(8, "banners'");
        return NO_HISTORY;
    }
    Banner *banner = &history->banners[i];
    history->banner_keys[i] = layers, history->banners_touched = TRUE;
    banner->mesh = mesh, banner->occurrence = occurrence;
    memcpy(banner->world, world, WORLD_BYTES);
    const Banner *old = banner_find(histories[history_index ^ 1], layers, mesh, occurrence);
    memcpy(banner->previous, old ? old->world : world, WORLD_BYTES);
    fsr_counts[FSR_BANNERS]++;
    fsr_counts[FSR_BANNERS_PREVIOUS] += old != NULL;
    ReleaseSRWLockExclusive(&banner_lock);
    return i;
}

static void __attribute__((thiscall)) banner_submit(BYTE *mesh, DWORD lod, const float *world, void *cull, BYTE *params, DWORD flags, DWORD parallel) {
    TrackerContext *context = fsr_on && fsr_units ? tracker_context() : NULL;
    if (context) context->banner_layers = *(BYTE **)(params + BANNER_PARAMS_LAYERS), context->banner_mesh = mesh;
    original_banner_submit(mesh, lod, world, cull, params, flags, parallel);
    if (context) context->banner_layers = NULL;
}

static DWORD __attribute__((thiscall)) banner_append(BYTE *list, BYTE *record, DWORD parallel) {
    TrackerContext *context = TlsGetValue(tracker_slot);
    if (context && context->banner_layers) {
        DWORD slot = banner_entry(context->banner_layers, context->banner_mesh, (const float *)(record + BANNER_INSTANCE));
        if (slot != NO_HISTORY) *(float *)(record + BANNER_SLOT_FIELD) = (float)(slot + BANNER_SLOT_BIAS);
    }
    return original_banner_append(list, record, parallel);
}

// The engine's copy of the instance data with mesh_data.y back to 1, then the previous world.
static void __attribute__((thiscall)) banner_gather(BYTE *gatherer, InstanceSpan *span, DWORD *ids) {
    BYTE *list = *(BYTE **)(gatherer + GATHERER_LIST);
    DWORD size = *(DWORD *)(gatherer + GATHERER_RECORD_SIZE), count = span->bytes / span->stride;
    const History *history = histories[history_index];
    BYTE *out = span->data;
    for (DWORD i = 0; i < count; i++, out += size) {
        BYTE *record = ((ListElementFn)(*(void ***)list)[1])(list, ids[i]);
        float tag = *(float *)(record + BANNER_SLOT_FIELD);
        DWORD slot = tag >= BANNER_SLOT_BIAS ? (DWORD)tag - BANNER_SLOT_BIAS : NO_HISTORY;
        const void *previous = slot < BANNER_SLOTS && history->banner_keys[slot] ? history->banners[slot].previous : (const void *)(record + BANNER_INSTANCE);
        memcpy(out, record + BANNER_INSTANCE, BANNER_INSTANCE_BYTES);
        *(float *)(out + BANNER_SLOT_FIELD - BANNER_INSTANCE) = 1.0f;
        memcpy(out + BANNER_INSTANCE_BYTES, previous, WORLD_BYTES);
    }
}

// The AA list with FSR's modes after the game's own, as values 5..9.
static void __attribute__((thiscall)) aa_list_push(BYTE *self, EngineWString *list) {
    static wchar_t text[AA_LIST_CHARS];
    size_t extra = wcslen(fsr_entries);
    if (!fsr_usable() || !list->text || list->length + extra >= AA_LIST_CHARS) {
        ((ListPushFn)(engine + AA_LIST_PUSH_RVA))(self, list);
        return;
    }
    memcpy(text, list->text, list->length * sizeof *text);
    wcscpy(text + list->length, fsr_entries);
    EngineWString ours = {list->length + (DWORD)extra, list->length + (DWORD)extra, text};
    ((ListPushFn)(engine + AA_LIST_PUSH_RVA))(self, &ours);
}

// The engine clamps the AA to the device's highest at start; a saved FSR mode, which its setter kept only as asked,
// becomes the setting.
static void __attribute__((thiscall)) start_clamp(BYTE *object) {
    ((EngineFn)(engine + START_CLAMP_RVA))(object);
    DWORD asked = *(DWORD *)(object + AA_ASKED_OFFSET);
    if (asked < FSR_AA_FIRST || asked >= FSR_AA_FIRST + FSR_MODES || !fsr_usable()) return;
    *(DWORD *)(object + AA_SETTING_OFFSET) = *(DWORD *)(object + AA_REQUESTED_OFFSET) = asked;
    api->log_line("fsr: the saved anti-aliasing choice %s kept", mode_names[asked - FSR_AA_FIRST]);
}

// x' = x (W_r + 0.25) / W: both of the engine's roundings (at creation, at a device reset) give the render size W_r.
static void render_scale(float *x, float *y) {
    BYTE *settings = *(BYTE **)(engine + SETTINGS_GLOBAL_RVA);
    float ratio = upscale_ratios[chosen_mode()];
    DWORD width = *(DWORD *)(settings + SETTINGS_WIDTH), height = *(DWORD *)(settings + SETTINGS_HEIGHT);
    *x *= ((float)(DWORD)((float)width / ratio + 0.5f) + 0.25f) / (float)width;
    *y *= ((float)(DWORD)((float)height / ratio + 0.5f) + 0.25f) / (float)height;
}

static BYTE *__attribute__((thiscall)) screen_texture_hook(BYTE *texture, DWORD category, const ScreenScale *scale, DWORD bind, DWORD format, DWORD aa) {
    fsr_upscaling = fsr_active() && chosen_mode() > 0;
    DWORD site = (DWORD)((BYTE *)__builtin_return_address(0) - engine);
    BOOL before_a = FALSE;
    for (size_t i = 0; i < sizeof render_size_sites / sizeof *render_size_sites; i++) before_a |= site == render_size_sites[i];
    if (!fsr_upscaling || !before_a) return original_screen_texture(texture, category, scale, bind, format, aa);
    ScreenScale scaled = *scale;
    render_scale(&scaled.x, &scaled.y);
    return original_screen_texture(texture, category, &scaled, bind, format, aa);
}

// Upscaling: FSR takes the resolve's place, from the render-size colour into the output-size copy the views after A read.
static void __attribute__((thiscall)) resolve_hook(BYTE *view) {
    BYTE *scene = *(BYTE **)(view + VIEW_SCENE_OFFSET);
    if (!fsr_upscaling || !fsr_drawing() || !window_open || scene != main_scene()) {
        original_resolve(view);
        return;
    }
    run_fsr(scene, opaque_taken);
    window_open = FALSE;
}

// At the AA branch (test ebx, ebx; je): the resolved copy is made with MSAA or while upscaling.
void fsr_branch_stub(void);
__asm__(".text\n.intel_syntax noprefix\n"
        ".globl _fsr_branch_stub\n"
        "_fsr_branch_stub:\n"
        "  test ebx, ebx\n"
        "  jnz 1f\n"
        "  cmp dword ptr [_fsr_upscaling], 0\n"
        "  jnz 1f\n"
        "  add esp, 4\n"
        "  jmp dword ptr [_fsr_branch_skip]\n"
        "1:\n"
        "  ret\n"
        ".att_syntax\n");

static const char *patch_slot(DWORD rva, void *replacement, void **original) {
    void **slot = (void **)(engine + rva);
    BYTE file_bytes[4], wanted[4];
    uint32_t file_value = (uint32_t)(uintptr_t)*slot - (uint32_t)(uintptr_t)engine + 0x10000000u;
    memcpy(file_bytes, &file_value, 4);
    memcpy(wanted, &replacement, 4);
    *original = *slot;
    return api->patch((BYTE *)slot, file_bytes, wanted, 4);
}

static const char *install_device(void) {
    HRESULT probe = probe_11_1();
    if (probe == E_INVALIDARG || probe == E_NOINTERFACE)
        return probe == E_INVALIDARG ? "this Windows has no Direct3D 11.1, which FSR needs" : "no D3D11CreateDevice to probe for Direct3D 11.1";
    BYTE level_list[7] = {0x6a, 0x04, 0x68};
    const D3D_FEATURE_LEVEL *list = feature_levels;
    memcpy(level_list + 3, &list, 4);
    const char *error = api->patch(engine + LEVEL_LIST_RVA, level_list_expected, level_list, sizeof level_list);
    if (!error) error = api->patch(engine + CAPS_LEVEL_RVA, caps_level_expected, caps_level_replacement, sizeof caps_level_replacement);
    if (!error) api->on_graphics_reset(NULL, resources_created);
    return error;
}

static const char *install_aaoff(void) {
    const char *error = api->hook(engine + AA_GETTER_RVA, aa_getter_prologue, sizeof aa_getter_prologue, (const void *)aa_getter_hook, (void **)&original_aa_getter);
    if (!error)
        error = api->hook(engine + FRAME_PRESENT_RVA, frame_present_prologue, sizeof frame_present_prologue, (const void *)frame_present_hook,
                          (void **)&original_frame_present);
    return error;
}

static const char *install_jitter(void) {
    const char *error = api->hook(engine + CAMERA_WRITER_RVA, camera_writer_prologue, sizeof camera_writer_prologue, (const void *)camera_writer_hook,
                                  (void **)&original_camera_writer);
    if (!error) error = patch_slot(GBUFFER_PREPASS_SLOT_RVA, (void *)gbuffer_prepass_hook, (void **)&original_gbuffer_prepass);
    return error;
}

static const char *patch_call(DWORD rva, const BYTE *expected, void *target) {
    BYTE call[5] = {0xe8};
    int32_t offset = (int32_t)((BYTE *)target - (engine + rva + 5));
    memcpy(call + 1, &offset, 4);
    return api->patch(engine + rva, expected, call, sizeof call);
}

static const char *install_objects(void) {
    const char *error = patch_call(CAMERA_FINALIZE_CALL_RVA, camera_finalize_call, (void *)camera_finalize);
    if (!error) error = patch_call(WIND_FINALIZE_CALL_RVA, wind_finalize_call, (void *)wind_finalize);
    if (!error)
        error = api->hook(engine + GBUFFER_TARGETS_RVA, gbuffer_targets_prologue, sizeof gbuffer_targets_prologue, (const void *)gbuffer_targets_hook,
                          (void **)&original_gbuffer_targets);
    if (!error)
        error = api->hook(engine + GBUFFER_DTOR_RVA, gbuffer_dtor_prologue, sizeof gbuffer_dtor_prologue, (const void *)gbuffer_dtor_hook,
                          (void **)&original_gbuffer_dtor);
    if (!error) error = patch_slot(GBUFFER_TARGET_SLOT_RVA, (void *)gbuffer_target_slot, (void **)&original_gbuffer_target);
    if (!error) error = patch_slot(GBUFFER_COUNT_SLOT_RVA, (void *)gbuffer_count_slot, (void **)&original_gbuffer_count);
    if (!error)
        error = api->hook(engine + SHADER_LOAD_RVA, shader_load_prologue, sizeof shader_load_prologue, (const void *)shader_load_hook,
                          (void **)&original_shader_load);
    return error;
}

static const char *install_units(void) {
    for (int i = 0; i < 2; i++)
        if (!(histories[i] = VirtualAlloc(NULL, sizeof(History), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) return "no memory for the units' history";
    if ((tracker_slot = TlsAlloc()) == TLS_OUT_OF_INDEXES) return "no thread-local slot for the units' history";
    const char *error = api->patch(engine + BONE_WIDTH_PUSH_RVA, bone_width_push, bone_width_wide, sizeof bone_width_wide);
    if (!error) error = api->patch(engine + WEIGHTED_RECORD_SIZE_RVA, record_size, record_size_wide, sizeof record_size_wide);
    if (!error) error = api->patch(engine + WEIGHTED_B_RECORD_SIZE_RVA, record_size, record_size_wide, sizeof record_size_wide);
    if (!error) error = api->patch(engine + RIGID_RECORD_SIZE_RVA, record_size, record_size_wide, sizeof record_size_wide);
    if (!error) error = patch_call(RIGID_SHADER_CALL_RVA, rigid_shader_call, (void *)rigid_shader_ctor);
    if (!error) error = patch_call(VTF_SHADER_CALL_RVA, vtf_shader_call, (void *)vtf_shader_ctor);
    if (!error) error = patch_slot(RIGID_GATHER_SLOT_RVA, (void *)rigid_gather, (void **)&original_rigid_gather);
    if (!error)
        error = api->hook(engine + FILL_TEXTURE_RVA, fill_texture_prologue, sizeof fill_texture_prologue, (const void *)fill_texture_hook,
                          (void **)&original_fill_texture);
    if (!error)
        error = api->hook(engine + DRAW_TRACKER_RVA, draw_tracker_prologue, sizeof draw_tracker_prologue, (const void *)draw_tracker_hook,
                          (void **)&original_draw_tracker);
    if (!error)
        error = api->hook(engine + POSE_APPEND_RVA, pose_append_prologue, sizeof pose_append_prologue, (const void *)pose_append_hook,
                          (void **)&original_pose_append);
    if (!error) error = patch_call(RIGID_APPEND_CALL_RVA, rigid_append_call, (void *)rigid_append);
    if (!error) error = patch_call(SKINNED_APPEND_CALL_RVA, skinned_append_call, (void *)skinned_append);
    if (!error) error = patch_call(SKINNED_B_APPEND_CALL_RVA, skinned_b_append_call, (void *)skinned_b_append);
    if (!error) error = patch_call(BATCH_WORLD_CALL_RVA, batch_world_call, (void *)batch_world_hook);
    if (!error) error = api->hook(engine + BATCH_RVA, batch_prologue, sizeof batch_prologue, (const void *)batch_hook, (void **)&original_batch);
    if (!error)
        error = api->hook(engine + IMPOSTER_ADD_RVA, imposter_add_prologue, sizeof imposter_add_prologue, (const void *)imposter_add_hook,
                          (void **)&original_imposter_add);
    if (!error)
        error = api->hook(engine + IMPOSTER_WRITER_RVA, imposter_writer_prologue, sizeof imposter_writer_prologue, (const void *)imposter_writer_hook,
                          (void **)&original_imposter_writer);
    if (!error) error = patch_call(BANNER_SHADER_CALL_RVA, banner_shader_call, (void *)banner_shader_ctor);
    if (!error) error = patch_call(BANNER_WIND_SHADER_CALL_RVA, banner_wind_shader_call, (void *)banner_shader_ctor);
    // The banner meshes' gatherer: the engine's vtable but for its gather.
    if (!error) {
        memcpy(banner_gatherer_vtable, *(void ***)(engine + BANNER_GATHERER_VTABLE_IMMEDIATE_RVA), sizeof banner_gatherer_vtable);
        banner_gatherer_vtable[1] = (void *)banner_gather;
        error = patch_slot(BANNER_GATHERER_VTABLE_IMMEDIATE_RVA, banner_gatherer_vtable, &original_banner_gatherer_vtable);
    }
    if (!error) error = patch_slot(BANNER_SUBMIT_SLOT_RVA, (void *)banner_submit, (void **)&original_banner_submit);
    if (!error) error = patch_slot(BANNER_APPEND_SLOT_RVA, (void *)banner_append, (void **)&original_banner_append);
    if (!error) error = patch_call(CLOTH_SHADER_CALL_RVA, cloth_shader_call, (void *)cloth_shader_ctor);
    if (!error) error = api->patch(engine + CLOTH_STRIDE_RVA, cloth_stride, cloth_stride_wide, sizeof cloth_stride_wide);
    if (!error) error = patch_slot(CLOTH_ADD_SLOT_RVA, (void *)cloth_add, (void **)&original_cloth_add);
    if (!error) error = patch_slot(CLOTH_GATHER_SLOT_RVA, (void *)cloth_gather, (void **)&original_cloth_gather);
    if (!error) error = patch_call(RIGID_LEGACY_SHADER_CALL_RVA, rigid_legacy_shader_call, (void *)rigid_legacy_shader_ctor);
    if (!error) error = patch_call(RIGID_STANDARD_SHADER_CALL_RVA, rigid_standard_shader_call, (void *)rigid_standard_shader_ctor);
    if (!error) {
        memcpy(rigid_gatherer_vtable, *(void ***)(engine + RIGID_GATHERER_VTABLE_IMMEDIATE_RVA), sizeof rigid_gatherer_vtable);
        rigid_gatherer_vtable[1] = (void *)scene_rigid_gather;
        error = patch_slot(RIGID_GATHERER_VTABLE_IMMEDIATE_RVA, rigid_gatherer_vtable, &original_rigid_gatherer_vtable);
    }
    if (!error) error = patch_slot(RIGID_BASE_GATHERER_VTABLE_IMMEDIATE_RVA, rigid_gatherer_vtable, &original_rigid_base_gatherer_vtable);
    if (!error) error = patch_slot(RIGID_APPEND_SLOT_RVA, (void *)scene_rigid_append, (void **)&original_scene_rigid_append);
    if (!error) error = patch_call(PROJECTILE_DRAW_CALL_RVA, projectile_draw_call, (void *)projectile_draw);
    layout_ok = !error;
    return error;
}

static const char *install_weather(void) {
    const char *error = patch_call(RAIN_SHADER_CALL_RVA, rain_shader_call, (void *)weather_shader_ctor);
    if (!error) error = patch_call(SNOW_SHADER_CALL_RVA, snow_shader_call, (void *)weather_shader_ctor);
    if (!error) error = patch_slot(ALPHA_TARGET_SLOT_RVA, (void *)alpha_target_slot, (void **)&original_alpha_target);
    if (!error) error = patch_slot(ALPHA_COUNT_SLOT_RVA, (void *)alpha_count_slot, (void **)&original_alpha_count);
    return error;
}

static const char *install_modes(void) {
    fsr_branch_skip = engine + AA_BRANCH_SKIP_RVA;
    BYTE branch[6] = {0xe8, 0, 0, 0, 0, 0x90}, save[6] = {0xe8, 0, 0, 0, 0, 0x90};
    int32_t offset = (int32_t)((BYTE *)fsr_branch_stub - (engine + AA_BRANCH_RVA + 5));
    memcpy(branch + 1, &offset, 4);
    offset = (int32_t)((BYTE *)saved_aa - (engine + SAVE_AA_CALL_RVA + 5));
    memcpy(save + 1, &offset, 4);
    const char *error = patch_call(AA_LIST_CALL_RVA, aa_list_call, (void *)aa_list_push);
    if (!error) error = patch_call(START_CLAMP_CALL_RVA, start_clamp_call, (void *)start_clamp);
    if (!error) error = api->patch(engine + AA_BRANCH_RVA, aa_branch, branch, sizeof branch);
    if (!error)
        error = api->hook(engine + SCREEN_TEXTURE_CTOR_RVA, screen_texture_prologue, sizeof screen_texture_prologue, (const void *)screen_texture_hook,
                          (void **)&original_screen_texture);
    if (!error) error = api->hook(engine + RESOLVE_RVA, resolve_prologue, sizeof resolve_prologue, (const void *)resolve_hook, (void **)&original_resolve);
    if (!error) error = patch_slot(REQUESTED_AA_SLOT_RVA, (void *)requested_aa_hook, (void **)&original_requested_aa);
    if (!error) error = api->patch(engine + SAVE_AA_CALL_RVA, save_aa_call, save, sizeof save);
    return error;
}

static const char *install_mipbias(void) {
    return api->hook(engine + SAMPLER_CREATE_RVA, sampler_create_prologue, sizeof sampler_create_prologue, (const void *)sampler_create_hook,
                     (void **)&original_sampler_create);
}

static const char *install_pass(void) {
    const char *error = patch_slot(ALPHA_POSTPASS_SLOT_RVA, (void *)alpha_postpass_hook, (void **)&original_alpha_postpass);
    if (!error) error = patch_slot(EFFECTS_POSTPASS_SLOT_RVA, (void *)effects_postpass_hook, (void **)&original_effects_postpass);
    if (!error) error = patch_slot(ALPHA_PREPASS_SLOT_RVA, (void *)alpha_prepass_hook, (void **)&original_alpha_prepass);
    if (!error) api->on_graphics_reset(release_targets, NULL);
    return error;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    engine = host->engine;
    read_settings();
    if (!enabled) return "off ([fsr] enabled=0 in s2prof.ini)";
    // Every part's engine code is checked before any is installed.
    const char *mismatch = S2_CHECK_ENGINE(host, fsr_checks);
    if (mismatch) return mismatch;
    if (render_device()) return "the graphics device already exists; FSR needs it made at feature level 11_1, so this takes effect from the next start";
    const char *missing = load_library();
    if (missing) return missing;
    api->log_line("fsr: sharpness %.2f", sharpness);
    // A part that fails to install leaves the parts before it in place but idle: FSR is offered only with every part,
    // our shaders only with the whole layout.
    const char *error = install_device();
    if (error) return error;
    api->begin_fix("fsraaoff");
    if ((error = install_aaoff())) return error;
    api->begin_fix("fsrjitter");
    if ((error = install_jitter())) return error;
    api->begin_fix("fsrmipbias");
    if ((error = install_mipbias())) return error;
    api->begin_fix("fsrobjects");
    if ((error = install_objects())) return error;
    api->begin_fix("fsrunits");
    if ((error = install_units())) return error;
    api->begin_fix("fsrweather");
    if ((error = install_weather())) return error;
    api->begin_fix("fsrmodes");
    if ((error = install_modes())) return error;
    api->begin_fix("fsrpass");
    if ((error = install_pass())) return error;
    group_ok = TRUE;
    return NULL;
}
