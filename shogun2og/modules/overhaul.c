#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#include "../s2module.h"
#include <d3d11.h>
#include <intrin.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

// The render overhaul, ported from the Complete Edition's. A view's execute_pass (0x1140c020) is split into contiguous
// ranges of its recorded groups: main draws the first range on the immediate context; each worker runs the engine's own
// execute over its range into a deferred context, through a private DRAW_CONTEXT whose recorders hand out only that
// range's groups, and main replays the command lists in range order, so the GPU sees the original draw order.
// Everything a range touches that the engine shares gets a per-thread copy: the renderer, each draw item's chunker and
// streams and the rings they allocate from, programs and their bound outputs, texture slots, constant-buffer uploads,
// and the shader library's parameter values. Stage 2 adds the Variant node's instance-texture fill on a worker beside
// its keys and sort (prepare), the whole prepare started on workers as BattleView::update ends (earlyprepare), and the
// next shadow cascade recorded while the current one draws (recordcascades, checked by recordverify); not ported: early
// effects, view record-ahead and overlap. It counts the draws and instances each mode issues, because a version that
// drops draws would look faster.
#define EXECUTE_RVA 0x140c020
#define UPLOAD_CONSTANT_BUFFERS_RVA 0x14520b0
#define ALWAYS_UPDATE_CB_RVA 0x1ca3274
#define OPTIMIZE_CYCLED_LOCKS_RVA 0x1c9bdec
#define CUSTOM_LOOP_RVA 0x1ca871b
#define KICK_CALL_RVA 0x1449f29
#define KICK_STREAM_RVA 0x13e24e0
#define STREAM_IN_RVA 0x13e24f0
#define RENDERER_VTABLE_RVA 0x1766548
#define RENDERER_VTABLE_SLOTS 44
#define STAGES 6
#define GET_RENDERER_SLOT_RVA 0x176677c
#define GET_RENDERER_RVA 0xa0a250
#define RENDER_DEVICE_RVA 0x1c9a93c
#define RENDERER_OFFSET 0x100
#define RENDERER_SIZE 0x141c
#define RENDERER_CONTEXT 0x8
// The target record (render-target count, RTVs, DSV) stays in a worker's renderer; the rest is the state cache.
#define RENDERER_TARGETS_START 0x115c
#define RENDERER_TARGETS_END 0x11a4
#define FIRST_DRAW_SLOT (0x84 / 4)
#define DRAW_SLOTS 4
#define RING_ALLOCATE_RVA 0x13fcc60
#define RING_ALLOCATE_SLOT 0x40
#define INSTANCE_RING_VTABLE_RVA 0x175b040
#define GEOMETRY_RING_VTABLE_RVA 0x175aff4
#define RING_HOLDER_SIZE 0x68
#define VERTEX_BUFFER_SIZE 0x24
#define VERTEX_BUFFER_VTABLE_RVA 0x17665f8
// Every list's first map of a clone renames its whole buffer in the driver; small clones that grow on demand also keep
// the 32-bit address space free (the instance ring is 5 MB).
#define RING_CLONE_BYTES (256 * 1024)
#define TEXTURE_SET_RVA 0x1449ef0
#define BUILD_SRV_LIST_RVA 0x1452220
#define SHADER_TEXTURE_SIZE 0x20
#define TEXTURE_COPIES 1024
#define TOUCH_RVA 0x1498040
#define LIBRARY_GETTER_RVA 0x141ee30
#define NODE_TEXTURE_RVA 0x14e2f50
#define NODE_FLUSH_RVA 0x14e6870
#define GROUP_GATHER_RVA 0x146bfc0
#define GROUP_INSTANCE_MESH_VTABLE_RVA 0x1761cf0
// RIGID_GROUP_INSTANCE_MESH up to the last field its gatherer reads (+0x128); a CRITICAL_SECTION follows.
#define GROUP_MESH_VIEW_SIZE 0x12c
#define SORT_BUCKET_RVA 0x1381d00
#define ENGINE_RVA 0x1c9a920
#define SCENE_OFFSET 0x1c
#define FRAME_COUNTER_OFFSET 0x69ccc
#define FRAME_STAMP_RVA 0x17cc340
#define LIBRARY_OFFSET 0xb18
#define LIBRARY_SIZE 0x6ccb4
#define SINGLE_RECORDER_VTABLE_RVA 0x175b1a0
#define BATCH_RECORDER_VTABLE_RVA 0x175b1c8
#define CHUNKER_VTABLE_RVA 0x1749eb0
#define CONTEXT_SIZE 0x64
#define DRAW_ITEM_SIZE 0x24
#define MAX_CHUNKER_SIZE 0x24
#define STREAMS_SIZE 0x1c
#define INDEX_STREAM_SIZE 0x10
#define STREAM_ENTRY_SIZE 16
#define PROGRAM_SIZE 0x70
#define PROGRAM_OUTPUTS 4
#define PROGRAM_TABLE_INITIAL 4096
#define VARIANT_WEIGHTED_VTABLE_RVA 0x1769440
#define VARIANT_WEIGHTED_B_VTABLE_RVA 0x1769524
#define VARIANT_CACHED_WRITER_RVA 0x14e46f0
#define VARIANT_B_CACHED_WRITER_RVA 0x14e4580
#define CACHED_EFFECT_BYTES 0xc8
#define CACHED_EFFECT_B_BYTES 0xac
// The node texture function reads the node up to +0x1c8.
#define CACHED_NODE_BYTES 0x1c8
#define MAX_WORKERS 8
#define MAX_STREAMS 16
#define MAX_CLONES 16
#define MAX_KICKS 256
#define RECORDER_SLOTS 10
#define WARMUP_FRAMES 60
#define WORKER_SPIN_ROUNDS 20000
#define CB_TABLE_SIZE 1024
#define VIEW_TYPES 64
#define STATS_CASCADES 4
#define VALUE_COPIES 512
#define VALUE_ARENA_BYTES (512 * 1024)
#define MAX_TOUCHED 64
#define WRITER_SLOTS 4
#define WRAPPED_CLASSES 1024
#define LEARNED_BUFFERS 32
#define BINDING_INFOS 4096
#define LIBRARY_BUFFERS 512
#define MAX_LIBRARY_RECORDS 8192
#define MAX_LIBRARY_SLOTS 4096
#define MAX_BUFFER_BINDINGS 4
#define MAX_CALLERS 16
#define PARITY_TOLERANCE 0.02
#define DRAW_CONTEXT_RVA 0x1c9a930
#define PREPARE_QUEUES_RVA 0x13e8aa0
#define PREPARE_RVA 0x14c4780
#define PREPARE_VTABLE_RVA 0x1769a0c
#define COMPUTE_KEYS_RVA 0x14e7d90
#define FILL_TEXTURES_RVA 0x14e78e0
#define LIST_MERGE_RVA 0x14e74d0
#define ITEM_SORT_RVA 0x14aba00
#define BATTLE_VIEW_UPDATE_RVA 0x2afac0
#define RENDER_ITEM_SIZE 0x138
// The Variant node (its prepare interface at +4): the pose list +0x17c, render lists A +0x8c and B +0x114, each a list
// object whose body (+4) holds the count (+4), items (+8), "needs merge" (+0x24) and a critical section (+0x28).
#define POSE_LIST 0x17c
#define POSE_COUNT 0x180
#define POSE_DIRTY 0x1a0
#define LIST_A_BODY 0x90
#define LIST_A_COUNT 0x94
#define LIST_A_ITEMS 0x98
#define LIST_A_MERGE 0xb4
#define LIST_B_BODY 0x118
#define LIST_B_COUNT 0x11c
#define LIST_B_MERGE 0x13c
#define NODE_TEXTURE_COUNT 0x1c0
#define NODE_INSTANCING 0x1c8
#define NODE_ROWS_DIRTY 0x1c9
#define NODE_INSTANCING_OFF 0x1ca
#define RECORD_QUEUES_RVA 0x13ce0b0
#define RESET_BUCKETS_RVA 0x13d7df0
#define BEGIN_BUCKET_RVA 0x14e17a0
#define CASCADE_LOOP_RVA 0x13e4320
#define CAMERA_HOLDER_RVA 0x13ed710
#define SET_CAMERA_RVA 0x1411c60
// The shadow view: cascade index +0x58 (raised after each cascade), count +0x6c, cameras from +0x258.
#define CASCADE_INDEX 0x58
#define CASCADE_COUNT 0x6c
#define CASCADE_CAMERAS 0x258
#define CASCADE_CAMERA_SIZE 0x138
#define PREPARE_MODES 8
#define EARLY_NODES 2
enum { HOLDS, REDO_DISTURBED, REDO_SEQUENCE, REDO_INSTANCING, REDO_POSES_DIRTY, REDO_A_MERGE, REDO_B_MERGE, REDO_POSES,
       REDO_A_COUNT, REDO_B_COUNT, REDO_REASONS };

typedef BYTE(__attribute__((thiscall)) *ExecuteFn)(BYTE *context, BYTE *renderer, BYTE *view);
typedef uint32_t(__attribute__((thiscall)) *SetShaderFn)(BYTE *renderer, BYTE *program);
typedef BYTE *(__attribute__((thiscall)) *UploadConstantBuffersFn)(BYTE *bindings, uint32_t unused, uint32_t *count, uint32_t *first, BYTE **cache);
typedef BYTE(__attribute__((thiscall)) *MapBufferFn)(BYTE *buffer, BYTE *renderer, uint32_t offset, uint32_t bytes, uint32_t type, void **out, uint32_t flags);
typedef void(__attribute__((thiscall)) *UnmapBufferFn)(BYTE *buffer, BYTE *renderer);
typedef void (*SortBucketFn)(BYTE **first, BYTE **last, uint32_t count, BYTE *view);
typedef BYTE(__attribute__((thiscall)) *StreamInFn)(BYTE *asset, BYTE *renderer);
typedef uint32_t(__attribute__((thiscall)) *ViewTypeFn)(BYTE *view);
typedef void(__attribute__((thiscall)) *DrawFn)(BYTE *renderer, UINT vertex_count, UINT start_vertex);
typedef void(__attribute__((thiscall)) *DrawIndexedFn)(BYTE *renderer, UINT index_count, UINT start_index, INT base_vertex, UINT vertex_count);
typedef void(__attribute__((thiscall)) *DrawIndexedInstancedFn)(BYTE *renderer, UINT index_count, UINT instances, UINT start_index, INT base_vertex,
                                                                 UINT vertex_count, UINT start_instance);
typedef void(__attribute__((thiscall)) *DrawInstancedFn)(BYTE *renderer, UINT vertex_count, UINT instances, UINT start_vertex, UINT start_instance);
typedef BYTE *(__attribute__((thiscall)) *RingAllocateFn)(BYTE *holder, BYTE *span, BYTE *renderer, uint32_t bytes, uint32_t unused, uint32_t *offset,
                                                         uint32_t stride);
typedef BYTE *(__attribute__((thiscall)) *TextureSetFn)(BYTE *slot, BYTE *texture);
typedef BYTE *(__attribute__((thiscall)) *BuildSrvListFn)(BYTE *list, uint32_t *count, uint32_t *first, BYTE **cache);
typedef void(__attribute__((thiscall)) *TouchFn)(BYTE *parameter);
typedef BYTE *(__attribute__((thiscall)) *LibraryGetterFn)(BYTE *holder);
typedef void(__attribute__((thiscall)) *NodeTextureFn)(BYTE *node, uint32_t index);
typedef void(__attribute__((thiscall)) *NodeFlushFn)(BYTE *list);
typedef uint32_t(__attribute__((thiscall)) *GatherFn)(BYTE *gatherer, BYTE *mapped, const uint32_t *ids);
typedef void(__attribute__((thiscall)) *PrepareQueuesFn)(BYTE *scene, BYTE *renderer);
typedef void(__attribute__((thiscall)) *PrepareFn)(BYTE *prepare_interface, BYTE *renderer);
typedef void(__attribute__((thiscall)) *NodeFn)(BYTE *node);
typedef void(__attribute__((thiscall)) *ListFn)(BYTE *list);
typedef void (*ItemSortFn)(BYTE *first, BYTE *last, uint32_t count, BYTE *context);
typedef void(__attribute__((thiscall)) *BattleViewUpdateFn)(BYTE *battle_view, void *argument);
typedef void(__attribute__((thiscall)) *RecordQueuesFn)(BYTE *view);
typedef void(__attribute__((thiscall)) *ResetBucketsFn)(BYTE *context);
typedef void(__attribute__((thiscall)) *BeginBucketFn)(BYTE *callbacks, void *argument, BYTE *view);
typedef BYTE *(__attribute__((thiscall)) *CameraHolderFn)(BYTE *owner);
typedef void(__attribute__((thiscall)) *SetCameraFn)(BYTE *holder, BYTE *camera);
typedef void (*TaskFn)(void *argument);
// ID3D11Multithread, which mingw's d3d11.h lacks: QueryInterface, AddRef, Release, Enter, Leave, SetMultithreadProtected.
typedef struct {
    void *slots[5];
    BOOL(STDMETHODCALLTYPE *SetMultithreadProtected)(void *self, BOOL protect);
} MultithreadVtable;
static const GUID multithread_iid = {0x9b7e4e00, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}};
// The four effect writer slots execute calls, each with its own arguments (callee pops).
typedef BYTE(__attribute__((thiscall)) *GroupWriterFn)(BYTE *effect, void *view, const uint32_t *ids, uint32_t count, void *k);
typedef BYTE(__attribute__((thiscall)) *SingleWriterFn)(BYTE *effect, void *view, uint32_t id, void *k);
typedef BYTE(__attribute__((thiscall)) *UnitWriterFn)(BYTE *effect, void *view, const uint32_t *ids, void *k, void *record);
typedef BYTE(__attribute__((thiscall)) *MaterialWriterFn)(BYTE *effect, void *view, void *k);

typedef struct {
    uint64_t draws, instances, elements, frames;
} WorkCounts;

enum { JOB_BATCH, JOB_SINGLE, JOB_PACKET };

// A batch recorder's INSTANCE_GROUP or a single recorder, decoded when the range reaches it, or a Packet copied from one.
typedef struct {
    BYTE *object;
    uint32_t bucket, kind, weight;
    BYTE *recorder;
    // A group-instance job's instance count (mesh+0x124) and pass bit (+0x128) when its view was snapshotted.
    uint32_t group_count, group_bit;
} Job;

// The item comes first, as in an INSTANCE_GROUP.
typedef struct {
    BYTE *item;
    uint32_t count, flags;
    uint32_t ids[];
} Packet;

typedef struct {
    ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
    UINT viewport_count, scissor_count;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
} TargetState;

typedef struct {
    BYTE *original, *copy;
    ID3D11Buffer *buffer;
    uint32_t bytes, grows;
} Clone;

typedef struct {
    BYTE *textures[MAX_KICKS];
    int count;
    uint64_t dropped;
} Kicks;

typedef struct {
    BYTE *original, *copy;
    uint32_t capacity[PROGRAM_OUTPUTS];
} ProgramClone;

// A worker's own copy of a texture slot it set in its current range (texturecopies); generation 0 is free.
typedef struct {
    BYTE *slot;
    uint32_t generation;
    __attribute__((aligned(16))) BYTE copy[SHADER_TEXTURE_SIZE];
} TextureCopy;

// A thread's own values of a constant buffer it wrote in its current range (valuecopies): the device buffer's shadow
// (+0x28, +0x2c bytes) as this thread left it, and the version (binding+0x20) at which the shared shadow last held them.
typedef struct {
    BYTE *buffer, *binding, *data;
    uint32_t generation, version;
    // A library buffer's copy (paramcontexts) is written back only if the range changed it.
    uint32_t changed;
} ValueCopy;

typedef struct {
    uint32_t generation, generations, active_count, touched_count, depth;
    BOOL drawing;
    ValueCopy copies[VALUE_COPIES];
    uint16_t active[VALUE_COPIES / 2];
    BYTE *touched[MAX_TOUCHED];
    BYTE *arena;
    size_t arena_used;
    uint64_t calls, unlocked_calls, restores, seeds, copies_out, unwrapped_touches, full;
    LONGLONG wait_ticks;
    // paramcontexts: the thread's copy of the shader library, whose parameters point into data, its own values of every
    // library buffer (library_copies[b] holds buffer b's); in_context while a writer runs in it.
    BYTE *library, *data;
    ValueCopy *library_copies;
    uint32_t library_epoch;
    BOOL contexts, in_context;
    volatile LONG *writer_lock;
    uint64_t context_calls, leaks, demotions;
    LONGLONG seed_ticks;
} ValueState;

typedef struct {
    BYTE *buffer;
    uint32_t version, list;
} UploadedBuffer;

typedef struct {
    BYTE *buffer;
    ID3D11Buffer *copy;
    uint32_t bytes;
} CbCopy;

typedef struct {
    UploadedBuffer uploaded[CB_TABLE_SIZE];
    uint32_t list_number;
    BYTE *uploaded_this_list[CB_TABLE_SIZE];
    uint32_t uploaded_this_list_count;
    BOOL use_copies;
    CbCopy *copies;
    uint32_t copy_capacity, copy_count;
} UploadTracker;

typedef struct Worker Worker;

typedef struct {
    void *const *vtable;
    Worker *worker;
    const Job *jobs;
    uint32_t next_job, end_job, forced_job, current_index;
    const Job *current;
} RangeRecorder;

typedef struct {
    uint32_t capacity, count;
    RangeRecorder **data;
} RecorderVector;

typedef struct {
    uint32_t first_job, end_job, first_callback, last_callback;
    __attribute__((aligned(16))) BYTE context[CONTEXT_SIZE];
    uint32_t bucket_capacity;
    uint32_t *record_counts;
    void **callbacks;
    RecorderVector *recorders;
    RangeRecorder *range_recorders;
    RangeRecorder **recorder_pointers;
    ID3D11CommandList *list;
    HRESULT finish_result;
    BYTE result;
} Range;

// A split view's groups and ranges, and the main-thread state its workers start from.
typedef struct {
    Job *jobs;
    uint32_t job_count, job_capacity;
    BYTE *packet_arena;
    size_t packet_capacity;
    Range ranges[MAX_WORKERS + 1];
    int range_count;
    double ends[MAX_WORKERS + 1];
    BYTE *view;
    uint32_t view_type;
    TargetState targets;
    unsigned int csr;
    unsigned short x87;
    volatile LONG remaining;
    __attribute__((aligned(16))) BYTE renderer_template[RENDERER_SIZE];
} ViewSlot;

struct Worker {
    __attribute__((aligned(16))) BYTE renderer[RENDERER_SIZE];
    void *vtable[RENDERER_VTABLE_SLOTS];
    DWORD thread_id;
    ProgramClone *programs;
    uint32_t program_capacity, program_count;
    UploadTracker uploads;
    __attribute__((aligned(16))) BYTE item[DRAW_ITEM_SIZE];
    __attribute__((aligned(16))) BYTE chunker[MAX_CHUNKER_SIZE];
    __attribute__((aligned(16))) BYTE streams[STREAMS_SIZE];
    __attribute__((aligned(16))) BYTE index_stream[INDEX_STREAM_SIZE];
    __attribute__((aligned(16))) BYTE stream_entries[MAX_STREAMS * STREAM_ENTRY_SIZE];
    int number;
    LONG started_at_generation;
    ID3D11DeviceContext *context;
    Clone clones[MAX_CLONES];
    int clone_count;
    Kicks kicks;
    ViewSlot *volatile slot;
    Range *volatile range;
    // A task runs as main would (get_renderer gives the engine's renderer), so the worker hides while it runs.
    TaskFn volatile task;
    void *task_argument;
    volatile LONG in_task;
    unsigned int task_csr;
    unsigned short task_x87;
    LONGLONG busy_ticks, range_ticks;
    WorkCounts work[2];
    uint64_t shared_holders;
    // The range's TextureCopy generation, 0 between ranges.
    uint32_t texture_generation, texture_generations, texture_copy_count;
    uint64_t texture_sets;
    TextureCopy texture_copies[TEXTURE_COPIES];
    ValueState values;
    __attribute__((aligned(16))) BYTE group_mesh[GROUP_MESH_VIEW_SIZE];
    // The mesh group_mesh is a view of for the current job, or NULL.
    BYTE *group_mesh_of;
};

typedef struct {
    __attribute__((aligned(16))) BYTE item[DRAW_ITEM_SIZE];
    __attribute__((aligned(16))) BYTE chunker[MAX_CHUNKER_SIZE];
    __attribute__((aligned(16))) BYTE view[GROUP_MESH_VIEW_SIZE];
    BYTE *mesh_of;
    const Job *job;
} MainGroup;

typedef struct {
    uint64_t views, jobs, ranges;
    LONGLONG snapshot, main_range, wait, replay, spread;
    // The snapshot's parts: listing the groups (with the engine's bucket sort), building the range contexts, preparing
    // the values (writers, library snapshot, main's copy), capturing the targets and the renderer template.
    LONGLONG snapshot_jobs, snapshot_contexts, snapshot_values, snapshot_targets;
    uint64_t ahead_records, ahead_hits, ahead_misses, ahead_verified, ahead_mismatched;
    LONGLONG ahead;
} Timers;

// ticks: main's time in the prepare and in joins of the early prepare; the parts are main's and the workers' (keys of
// lists A and B; instance textures; list A's merge and sort).
typedef struct {
    uint64_t frames, calls, nodes, repeats, items, split, updates, early_kicked, early_used, early_fallbacks, redone[REDO_REASONS];
    LONGLONG ticks, join;
    volatile LONGLONG main_keys, main_fill, main_sort, worker_keys, worker_fill, worker_sort;
} PrepareStats;

typedef struct {
    BYTE *node;
    uint32_t poses, list_a, list_b;
} EarlyNode;

// Where a view's last split ended its ranges (fractions of the view's weight) and how long each took.
typedef struct {
    int count;
    double ends[MAX_WORKERS + 1];
    LONGLONG ticks[MAX_WORKERS + 1];
} Balance;

// Main-thread time inside execute per view type (and shadow cascade), split or not: what is left to split.
typedef struct {
    uint64_t calls, groups, split;
    LONGLONG ticks;
} ViewStats;

typedef struct {
    uint32_t offset, buffer, value_offset, value_bytes, typed;
    BYTE *binding, *value;
} LibraryRecord;

typedef struct {
    BYTE *buffer, *shadow, *bindings[MAX_BUFFER_BINDINGS];
    uint32_t bytes, data_offset, binding_count;
} LibraryBuffer;

typedef struct {
    void **vtable;
    void *original[WRITER_SLOTS];
    volatile LONG writes[WRITER_SLOTS];
    BYTE *learned[WRITER_SLOTS][LEARNED_BUFFERS];
    volatile LONG context_locked[WRITER_SLOTS];
} WrappedClass;

typedef struct {
    BYTE *binding, *reference;
    volatile LONG varies;
} BindingInfo;

// On since stage 1 and the G-buffer split ran in game (2026-10-25); a switch, and it follows Apply Fixes. Off, every hook
// passes straight to the engine and only counts.
__declspec(dllexport) volatile LONG overhaul_enabled = 1;
__declspec(dllexport) volatile LONG overhaul_workers = 3;
// View types split (bit = type, from view vtbl+0x30): 1 the shadow map, 24 the G-buffer view (vtable 0x1175ffec; split
// in game 2026-10-25, -0.34 to -0.57 ms in a CPU-bound battle, no visible difference). Others are too small to pay.
__declspec(dllexport) volatile uint64_t overhaul_views = 1ull << 1 | 1ull << 24;
// Shadow cascades split (bit = cascade index).
__declspec(dllexport) volatile LONG overhaul_cascades = 7;
__declspec(dllexport) volatile LONG overhaul_min_jobs = 32;
__declspec(dllexport) volatile LONG overhaul_main_share = 60;
__declspec(dllexport) volatile LONG overhaul_autobalance = 1;
__declspec(dllexport) volatile LONG overhaul_texture_copies = 1;
__declspec(dllexport) volatile LONG overhaul_value_copies = 1;
__declspec(dllexport) volatile LONG overhaul_param_contexts = 1;
__declspec(dllexport) volatile LONG overhaul_cb_copies = 1;
__declspec(dllexport) volatile LONG overhaul_group_gather = 1;
// Every change flag a Variant writer acts on except 0x01 (bone palette, instancing off): a range's first group writes
// all of its own parameters instead of relying on the previous range's groups.
__declspec(dllexport) volatile LONG overhaul_forced_flags = 0x1be;
__declspec(dllexport) volatile LONG overhaul_force_bones = 0;
// The prepare's texture fill on a worker; off until it has run in game.
__declspec(dllexport) volatile LONG overhaul_prepare_split = 0;
// The prepare started on two workers as BattleView::update ends, beside the UI update; off until it has run in game.
__declspec(dllexport) volatile LONG overhaul_early_prepare = 0;
// The next shadow cascade recorded while the current one draws; off until it has run in game.
__declspec(dllexport) volatile LONG overhaul_record_cascades = 0;
// At the cascade's own turn, record it again and compare with what was recorded ahead.
__declspec(dllexport) volatile LONG overhaul_record_verify = 0;

static const BYTE execute_prologue[] = {0x83, 0xec, 0x50, 0x53, 0x55, 0x56};
static const BYTE upload_constant_buffers_prologue[] = {0x51, 0x8b, 0x44, 0x24, 0x0c};
static const BYTE ring_allocate_prologue[] = {0x6a, 0x01, 0xff, 0x74, 0x24, 0x1c};
static const BYTE texture_set_prologue[] = {0x56, 0x8b, 0x74, 0x24, 0x08};
static const BYTE build_srv_list_prologue[] = {0x51, 0x53, 0x56, 0x8b, 0xc1};
static const BYTE touch_prologue[] = {0x8b, 0x41, 0x0c, 0xff, 0x41, 0x10};
static const BYTE library_getter_prologue[] = {0x8b, 0x41, 0x1c, 0x05, 0x18, 0x0b, 0x00, 0x00};
static const BYTE node_texture_prologue[] = {0x53, 0x56, 0x57, 0x8b, 0xf9};
static const BYTE group_gather_prologue[] = {0x83, 0xec, 0x20, 0x8b, 0xc1};
static const BYTE prepare_queues_prologue[] = {0x51, 0x53, 0x8b, 0xd9, 0x56};
static const BYTE prepare_prologue[] = {0x51, 0x56, 0x57, 0x8b, 0xf9};
static const BYTE list_merge_prologue[] = {0x83, 0xec, 0x0c, 0x56, 0x57};
static const BYTE battle_view_update_prologue[] = {0x56, 0x57, 0x8b, 0xf9, 0x8b, 0x4f, 0x14};
static const BYTE record_queues_prologue[] = {0x56, 0x57, 0x8b, 0xf9, 0x8b, 0x77, 0x18};
static const BYTE reset_buckets_prologue[] = {0x51, 0x53, 0x8b, 0xd9, 0x55};
static const BYTE begin_bucket_prologue[] = {0x53, 0x8b, 0x5c, 0x24, 0x0c};
static const uint32_t expected_draw_rvas[DRAW_SLOTS] = {0x144ea20, 0x144eb20, 0x144ec30, 0x144ed50};
// The set_shader slots, VS, GS, HS, DS, PS, CS: thiscall(renderer; program).
static const uint32_t set_shader_slots[STAGES] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
static const uint32_t writer_slots[WRITER_SLOTS] = {0x20, 0x24, 0x28, 0x58};
// SHADER_PARAMETER classes and where each keeps the typed copy of its value pointer (0: +0x14 only), which bind_storage
// writes with +0x14; most writers store through it.
static const struct {
    uint32_t vtable_rva, typed;
} parameter_classes[] = {{0x174942c, 0x24}, {0x1749728, 0x28}, {0x17494a4, 0x2c}, {0x1749714, 0x30}, {0x17504d4, 0x50}, {0x1749700, 0x60},
                         {0x17494b8, 0},    {0x1749850, 0},    {0x1749f9c, 0},    {0x174ae48, 0},    {0x174aee4, 0},    {0x174af9c, 0},
                         {0x174b050, 0},    {0x174cb28, 0},    {0x174cdcc, 0},    {0x174cde0, 0},    {0x174cdf4, 0}};
static const struct {
    uint32_t vtable_rva, size;
} chunker_classes[] = {{CHUNKER_VTABLE_RVA, 0x18}, {0x1767088, 0x24}, {0x1763a94, 0x20}, {0x1763bbc, 0x20}};
// Program output vectors {capacity, count, data} and the binding vectors that size them: CBs, samplers, SRVs, UAVs.
static const struct {
    uint32_t output, bindings;
} program_outputs[PROGRAM_OUTPUTS] = {{0x10, 0x04}, {0x2c, 0x20}, {0x50, 0x38}, {0x5c, 0x44}};

static const S2Api *api;
static BYTE *engine;
static ExecuteFn original_execute;
static SetShaderFn original_set_shaders[STAGES];
static UploadConstantBuffersFn original_upload_constant_buffers;
static RingAllocateFn original_ring_allocate;
static TextureSetFn original_texture_set;
static BuildSrvListFn original_build_srv_list;
static TouchFn original_touch;
static LibraryGetterFn original_library_getter;
static NodeTextureFn original_node_texture;
static GatherFn original_group_gather;
static SortBucketFn sort_bucket;
static StreamInFn stream_in;
static PrepareQueuesFn original_prepare_queues;
static PrepareFn original_prepare;
static RecordQueuesFn original_record_queues;
static ResetBucketsFn original_reset_buckets;
static BeginBucketFn original_begin_bucket;
static ListFn original_list_merge, original_node_flush;
static BattleViewUpdateFn original_battle_view_update;
static void *immediate_lock;
static BOOL immediate_lock_failed, protected_before, protection_logged;
static PrepareStats prepare_stats[PREPARE_MODES];
static uint64_t prepare_stamp;
// BattleView::update calls counted, the call after which this frame's nodes were prepared, and those nodes.
static uint64_t update_sequence, prepared_after;
static BYTE *frame_nodes[EARLY_NODES];
static int frame_node_count;
static struct {
    EarlyNode nodes[EARLY_NODES];
    int count;
    volatile LONG pending;
    volatile LONG disturbed;
    uint64_t sequence;
} early;
static struct {
    BYTE *view;
    uint32_t cascade;
    BOOL skip_reset;
} ahead;
static BYTE *verify_records[2];
static size_t verify_capacity[2];
static void *original_draws[DRAW_SLOTS];
static volatile uint64_t *frame_stamp;
static uint64_t last_stamp, enabled_frames;
static Worker *workers[MAX_WORKERS];
static volatile LONG worker_count;
static const char *worker_error;
static ID3D11Device *device;
static volatile LONG generation;
static ViewSlot slot_storage;
static MainGroup main_group;
static Kicks main_kicks;
static volatile DWORD split_thread;
static BYTE *zero_sort_flags;
static uint32_t zero_sort_capacity;
static WorkCounts work[2];
static Timers timers[2];
static Balance balances[VIEW_TYPES][STATS_CASCADES];
static Balance *current_balance;
static ViewStats view_stats[2][VIEW_TYPES][STATS_CASCADES];
static uint64_t fallback_unknown_recorders, texture_copies_full, value_copies_full, wrapped_classes_full, learned_full, binding_infos_full, foreign_executes;
static volatile LONG values_active, contexts_split, escaped_touches;
// Workers run engine code only while a split view's ranges run (a worker in a task counts as main), so outside them no
// hook needs to look the thread up; the lookup in every draw, upload and texture set cost ~1.5% of main.
static volatile LONG ranges_running;
// Between a bench's start and end report: the stats of an overhaul that is off are collected only then.
static BOOL measuring;
static ValueState main_values;
static SRWLOCK value_lock = SRWLOCK_INIT;
static WrappedClass wrapped_classes[WRAPPED_CLASSES];
static LONG wrapped_class_count;
static BindingInfo binding_infos[BINDING_INFOS];
static LONG binding_info_count;
static void *unwrapped_callers[MAX_CALLERS], *leak_callers[MAX_CALLERS];
static volatile LONG unwrapped_caller_count, leak_caller_count;
static struct {
    BYTE *shared, *snapshot;
    LibraryRecord *records;
    uint32_t record_count, buffer_count, data_bytes, epoch;
    LibraryBuffer buffers[LIBRARY_BUFFERS];
    uint32_t index[2 * LIBRARY_BUFFERS];
    uint64_t checked_stamp;
    uint32_t slot_offsets[MAX_LIBRARY_SLOTS];
    volatile LONG slot_count;
    uint32_t snapshot_slots;
    BYTE *slot_snapshot, *slot_marks;
    BOOL failed;
} contexts;

static void join_early(void);

static int overhaul_on(void) {
    return overhaul_enabled && *api->fix_enabled;
}

static int ab_mode(void) {
    return overhaul_on();
}

// The prepare's stats are kept per combination of the switches that change it, so a bench of any one of them compares.
static int prepare_mode(void) {
    return overhaul_on() | (overhaul_prepare_split != 0) << 1 | (overhaul_early_prepare != 0) << 2;
}

static void *method(void *object, int offset) {
    return (*(void ***)object)[offset / 4];
}

static LONGLONG now_ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static double ticks_ms(LONGLONG ticks) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return 1000.0 * ticks / frequency.QuadPart;
}

static BYTE *library_shared(void) {
    return *(BYTE **)(*(BYTE **)(engine + ENGINE_RVA) + SCENE_OFFSET) + LIBRARY_OFFSET;
}

static uint32_t frame_counter(void) {
    return *(uint32_t *)(*(BYTE **)(engine + ENGINE_RVA) + FRAME_COUNTER_OFFSET);
}

// No fast TLS slot is left in this game, so a thread finds its worker by id.
static Worker *current_worker(void) {
    if (!ranges_running) return NULL;
    DWORD self = __readfsdword(0x24);
    for (int i = 0; i < worker_count; i++)
        if (workers[i]->thread_id == self) return workers[i]->in_task ? NULL : workers[i];
    return NULL;
}

// The value state of a thread drawing a range of a split view, or NULL.
static ValueState *drawing_values(void) {
    if (!values_active) return NULL;
    Worker *worker = current_worker();
    if (worker) return worker->values.drawing ? &worker->values : NULL;
    return main_values.drawing && GetCurrentThreadId() == split_thread ? &main_values : NULL;
}

static void note_caller(void **callers, volatile LONG *count, void *caller) {
    LONG seen = *count;
    for (LONG i = 0; i < seen && i < MAX_CALLERS; i++)
        if (callers[i] == caller) return;
    LONG index = InterlockedIncrement(count) - 1;
    if (index < MAX_CALLERS) callers[index] = caller;
}

// ---- the shader library's per-thread copies (paramcontexts) ----

static BOOL heap_pointer(const BYTE *value) {
    return (uintptr_t)value >= 0x10000 && (uintptr_t)value < 0xffff0000u && !((uintptr_t)value & 3);
}

static BOOL read_memory(const void *address, void *out, size_t bytes) {
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, bytes, &done) && done == bytes;
}

static int library_buffer_index(const BYTE *buffer) {
    uint32_t mask = 2 * LIBRARY_BUFFERS - 1;
    for (uint32_t i = (uint32_t)((uintptr_t)buffer >> 4) * 0x9e3779b9u & mask;; i = (i + 1) & mask) {
        uint32_t entry = contexts.index[i];
        if (!entry) return -1;
        if (contexts.buffers[entry - 1].buffer == buffer) return (int)entry - 1;
    }
}

static int add_library_buffer(BYTE *buffer, BYTE *binding, BYTE *shadow, uint32_t bytes) {
    int b = library_buffer_index(buffer);
    if (b < 0) {
        if (contexts.buffer_count == LIBRARY_BUFFERS) return -1;
        b = (int)contexts.buffer_count++;
        contexts.buffers[b] = (LibraryBuffer){buffer, shadow, {binding}, bytes, 0, 1};
        uint32_t mask = 2 * LIBRARY_BUFFERS - 1, i = (uint32_t)((uintptr_t)buffer >> 4) * 0x9e3779b9u & mask;
        while (contexts.index[i]) i = (i + 1) & mask;
        contexts.index[i] = (uint32_t)b + 1;
        return b;
    }
    LibraryBuffer *entry = &contexts.buffers[b];
    for (uint32_t k = 0; k < entry->binding_count; k++)
        if (entry->bindings[k] == binding) return b;
    if (entry->binding_count == MAX_BUFFER_BINDINGS) return -1;
    entry->bindings[entry->binding_count++] = binding;
    return b;
}

static int parameter_typed_offset(const BYTE *record) {
    BYTE *vtable = *(BYTE *const *)record;
    for (size_t i = 0; i < sizeof parameter_classes / sizeof *parameter_classes; i++)
        if (vtable == engine + parameter_classes[i].vtable_rva) return (int)parameter_classes[i].typed;
    return -1;
}

// A parameter is a record with one of the 17 parameter vtables whose binding (+0xc) leads to a buffer whose shadow
// its value pointer (+0x14, and the typed copy) points into. The pointers are read through ReadProcessMemory.
static BOOL build_library(void) {
    contexts.shared = library_shared();
    if (!contexts.records && !(contexts.records = malloc(MAX_LIBRARY_RECORDS * sizeof *contexts.records))) {
        api->log_line("overhaul: out of memory for the shader library's parameters");
        __builtin_trap();
    }
    contexts.record_count = contexts.buffer_count = contexts.data_bytes = 0;
    memset(contexts.index, 0, sizeof contexts.index);
    for (uint32_t offset = 0; offset + 0x64 <= LIBRARY_SIZE; offset += 4) {
        int typed = parameter_typed_offset(contexts.shared + offset);
        if (typed < 0) continue;
        BYTE *binding = *(BYTE **)(contexts.shared + offset + 0xc), *value = *(BYTE **)(contexts.shared + offset + 0x14), *buffer, *shadow;
        uint32_t bytes;
        if (!heap_pointer(binding) || !value || (typed && *(BYTE **)(contexts.shared + offset + typed) != value) ||
            !read_memory(binding + 0x1c, &buffer, sizeof buffer) || !heap_pointer(buffer) || !read_memory(buffer + 0x28, &shadow, sizeof shadow) ||
            !read_memory(buffer + 0x2c, &bytes, sizeof bytes) || !shadow || !bytes || bytes > 65536 || value < shadow || value >= shadow + bytes)
            continue;
        int b = add_library_buffer(buffer, binding, shadow, bytes);
        if (b < 0 || contexts.record_count == MAX_LIBRARY_RECORDS) {
            api->log_line("overhaul: paramcontexts: the shader library has more parameters or buffers than the tables hold");
            return FALSE;
        }
        contexts.records[contexts.record_count++] = (LibraryRecord){offset, (uint32_t)b, (uint32_t)(value - shadow), 0, (uint32_t)typed, binding, value};
    }
    for (uint32_t r = 0; r < contexts.record_count; r++) {
        LibraryRecord *record = &contexts.records[r];
        uint32_t end = contexts.buffers[record->buffer].bytes;
        for (uint32_t o = 0; o < contexts.record_count; o++)
            if (contexts.records[o].buffer == record->buffer && contexts.records[o].value_offset > record->value_offset && contexts.records[o].value_offset < end)
                end = contexts.records[o].value_offset;
        record->value_bytes = end - record->value_offset;
    }
    for (uint32_t b = 0; b < contexts.buffer_count; b++) {
        contexts.buffers[b].data_offset = contexts.data_bytes;
        contexts.data_bytes += (contexts.buffers[b].bytes + 15) & ~15u;
    }
    free(contexts.snapshot);
    if (!(contexts.snapshot = malloc(contexts.data_bytes ? contexts.data_bytes : 16))) {
        api->log_line("overhaul: out of memory for the shader library's values");
        __builtin_trap();
    }
    contexts.epoch++;
    api->log_line("overhaul: paramcontexts: %u parameters in %u buffers (%u bytes) in the shader library", contexts.record_count, contexts.buffer_count,
                  contexts.data_bytes);
    return contexts.record_count > 0;
}

static BOOL library_holds(void) {
    if (library_shared() != contexts.shared) return FALSE;
    for (uint32_t r = 0; r < contexts.record_count; r++) {
        const LibraryRecord *record = &contexts.records[r];
        BYTE *at = contexts.shared + record->offset;
        if (*(BYTE **)(at + 0xc) != record->binding || *(BYTE **)(at + 0x14) != record->value || (record->typed && *(BYTE **)(at + record->typed) != record->value))
            return FALSE;
    }
    for (uint32_t b = 0; b < contexts.buffer_count; b++)
        if (*(BYTE **)(contexts.buffers[b].buffer + 0x28) != contexts.buffers[b].shadow || *(uint32_t *)(contexts.buffers[b].buffer + 0x2c) != contexts.buffers[b].bytes)
            return FALSE;
    return TRUE;
}

// On main at a split view's fork: the library checked once a frame and rebuilt if its parameters moved, and the values
// and texture slots every range of the view starts from.
static BOOL prepare_contexts(void) {
    if (contexts.failed) return FALSE;
    if (!contexts.record_count || (*frame_stamp != contexts.checked_stamp && !library_holds())) {
        if (contexts.record_count) api->log_line("overhaul: paramcontexts: the shader library's parameters moved; its copies are rebuilt");
        if (!build_library()) {
            contexts.failed = TRUE;
            contexts.record_count = 0;
            api->log_line("overhaul: paramcontexts unavailable; valuecopies' lock covers the writers");
            return FALSE;
        }
    }
    contexts.checked_stamp = *frame_stamp;
    if (!contexts.slot_snapshot && !(contexts.slot_snapshot = malloc(MAX_LIBRARY_SLOTS * SHADER_TEXTURE_SIZE))) {
        api->log_line("overhaul: out of memory for the library's texture slots");
        __builtin_trap();
    }
    contexts.snapshot_slots = contexts.slot_count < MAX_LIBRARY_SLOTS ? (uint32_t)contexts.slot_count : MAX_LIBRARY_SLOTS;
    for (uint32_t i = 0; i < contexts.snapshot_slots; i++)
        memcpy(contexts.slot_snapshot + i * SHADER_TEXTURE_SIZE, contexts.shared + contexts.slot_offsets[i], SHADER_TEXTURE_SIZE);
    for (uint32_t b = 0; b < contexts.buffer_count; b++)
        memcpy(contexts.snapshot + contexts.buffers[b].data_offset, contexts.buffers[b].shadow, contexts.buffers[b].bytes);
    return TRUE;
}

// On the drawing thread, at its first range of a library build: both value pointers of each parameter point into the
// thread's own values.
static void prepare_state_library(ValueState *state) {
    if (state->library && state->library_epoch == contexts.epoch) return;
    free(state->library);
    free(state->data);
    free(state->library_copies);
    state->library = malloc(LIBRARY_SIZE);
    state->data = malloc(contexts.data_bytes ? contexts.data_bytes : 16);
    state->library_copies = calloc(contexts.buffer_count ? contexts.buffer_count : 1, sizeof *state->library_copies);
    if (!state->library || !state->data || !state->library_copies) {
        api->log_line("overhaul: out of memory for a thread's copy of the shader library");
        __builtin_trap();
    }
    memcpy(state->library, contexts.shared, LIBRARY_SIZE);
    for (uint32_t r = 0; r < contexts.record_count; r++) {
        const LibraryRecord *record = &contexts.records[r];
        BYTE *value = state->data + contexts.buffers[record->buffer].data_offset + record->value_offset;
        *(BYTE **)(state->library + record->offset + 0x14) = value;
        if (record->typed) *(BYTE **)(state->library + record->offset + record->typed) = value;
    }
    for (uint32_t b = 0; b < contexts.buffer_count; b++)
        state->library_copies[b] = (ValueCopy){contexts.buffers[b].buffer, contexts.buffers[b].bindings[0], state->data + contexts.buffers[b].data_offset, 0, 0, 0};
    state->library_epoch = contexts.epoch;
}

static void begin_values(ValueState *state) {
    state->generation = ++state->generations ? state->generations : ++state->generations;
    state->active_count = 0;
    state->arena_used = 0;
    state->depth = 0;
    state->drawing = TRUE;
    state->in_context = FALSE;
    state->contexts = contexts_split != 0;
    if (!state->contexts) return;
    LONGLONG started = now_ticks();
    prepare_state_library(state);
    memcpy(state->data, contexts.snapshot, contexts.data_bytes);
    for (uint32_t i = 0; i < contexts.snapshot_slots; i++)
        memcpy(state->library + contexts.slot_offsets[i], contexts.slot_snapshot + i * SHADER_TEXTURE_SIZE, SHADER_TEXTURE_SIZE);
    // ~0: the shared values are not known to be the thread's, so the lock's restore puts them back first.
    for (uint32_t b = 0; b < contexts.buffer_count; b++) {
        state->library_copies[b].generation = state->generation;
        state->library_copies[b].version = ~0u;
        state->library_copies[b].changed = 0;
    }
    state->seed_ticks += now_ticks() - started;
}

static ValueCopy *value_copy(ValueState *state, BYTE *buffer, BOOL create) {
    if (state->contexts) {
        int b = library_buffer_index(buffer);
        if (b >= 0) return &state->library_copies[b];
    }
    uint32_t mask = VALUE_COPIES - 1;
    for (uint32_t i = (uint32_t)((uintptr_t)buffer >> 4) * 0x9e3779b9u & mask;; i = (i + 1) & mask) {
        ValueCopy *copy = &state->copies[i];
        if (copy->generation == state->generation) {
            if (copy->buffer == buffer) return copy;
            continue;
        }
        if (!create) return NULL;
        uint32_t bytes = *(uint32_t *)(buffer + 0x2c);
        if (state->active_count == VALUE_COPIES / 2 || state->arena_used + bytes > VALUE_ARENA_BYTES) {
            state->full++;
            if (!value_copies_full++)
                api->log_line("overhaul: a range wrote more than %d constant buffers or %d KB of them; the rest stay shared", VALUE_COPIES / 2,
                              VALUE_ARENA_BYTES / 1024);
            return NULL;
        }
        if (!state->arena && !(state->arena = malloc(VALUE_ARENA_BYTES))) {
            api->log_line("overhaul: out of memory for a thread's constant buffer values");
            __builtin_trap();
        }
        copy->buffer = buffer;
        copy->data = state->arena + state->arena_used;
        state->arena_used += (bytes + 15) & ~15u;
        copy->generation = state->generation;
        state->active[state->active_count++] = (uint16_t)i;
        return copy;
    }
}

// The values a thread draws with, for an upload: its own where it wrote the buffer in this range.
static const void *value_source(BYTE *buffer) {
    ValueState *state = drawing_values();
    ValueCopy *copy = state ? value_copy(state, buffer, FALSE) : NULL;
    return copy ? copy->data : *(void **)(buffer + 0x28);
}

static const LibraryRecord *find_record(uint32_t offset) {
    uint32_t low = 0, high = contexts.record_count;
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        if (contexts.records[middle].offset < offset) low = middle + 1;
        else high = middle;
    }
    return low < contexts.record_count && contexts.records[low].offset == offset ? &contexts.records[low] : NULL;
}

static const LibraryRecord *find_value_record(uint32_t buffer, uint32_t value_offset) {
    for (uint32_t r = 0; r < contexts.record_count; r++)
        if (contexts.records[r].buffer == buffer && contexts.records[r].value_offset == value_offset) return &contexts.records[r];
    return NULL;
}

// The thread's equivalent of a library or SCENE pointer an object keeps, or NULL.
static BYTE *copy_base(const ValueState *state, BYTE *kept) {
    return kept == contexts.shared || kept == contexts.shared - LIBRARY_OFFSET ? state->library + (kept - contexts.shared) : NULL;
}

static BOOL in_library_copy(const BYTE *address) {
    if (main_values.library && address >= main_values.library && address < main_values.library + LIBRARY_SIZE) return TRUE;
    for (int i = 0; i < worker_count; i++)
        if (workers[i]->values.library && address >= workers[i]->values.library && address < workers[i]->values.library + LIBRARY_SIZE) return TRUE;
    return FALSE;
}

// A touch by a writer running in the thread's library copy. A parameter outside the copy was reached without the getter:
// its store went to the shared values, which the thread's copy takes it from.
static void context_touch(ValueState *state, BYTE *parameter, BYTE *binding, void *caller) {
    BYTE *buffer = *(BYTE **)(binding + 0x1c);
    int b = buffer ? library_buffer_index(buffer) : -1;
    if (parameter >= state->library && parameter < state->library + LIBRARY_SIZE) {
        // The engine's own parameter keeps its version moving, as every writer would have it.
        InterlockedIncrement((volatile LONG *)(contexts.shared + (parameter - state->library) + 0x10));
        if (b >= 0) state->library_copies[b].changed = 1;
        return;
    }
    state->leaks++;
    note_caller(leak_callers, &leak_caller_count, caller);
    const LibraryRecord *record = NULL;
    if (parameter >= contexts.shared && parameter < contexts.shared + LIBRARY_SIZE) record = find_record((uint32_t)(parameter - contexts.shared));
    if (!record && b >= 0) {
        BYTE *value = *(BYTE **)(parameter + 0x14), *shadow = contexts.buffers[b].shadow;
        if (value >= shadow && value < shadow + contexts.buffers[b].bytes) record = find_value_record((uint32_t)b, (uint32_t)(value - shadow));
    }
    if (record && (int)record->buffer == b) {
        ValueCopy *copy = &state->library_copies[b];
        memcpy(copy->data + record->value_offset, contexts.buffers[b].shadow + record->value_offset, record->value_bytes);
        copy->changed = 1;
        return;
    }
    // A buffer the copies do not hold: taken as it stands, and the slot writes under the lock from now on.
    if (b < 0 && buffer) {
        ValueCopy *copy = value_copy(state, buffer, TRUE);
        if (copy) {
            copy->binding = binding;
            memcpy(copy->data, *(void **)(buffer + 0x28), *(uint32_t *)(buffer + 0x2c));
            copy->version = *(volatile uint32_t *)(binding + 0x20);
            copy->changed = 1;
        }
    }
    if (state->writer_lock && !*state->writer_lock) {
        *state->writer_lock = 1;
        state->demotions++;
    }
}

// SHADER_PARAMETER::touch (0x11498040) follows every parameter write: it bumps the parameter's version (+0x10) and its
// buffer's (binding +0x20). The engine's increments are not atomic, and a lost one is a missed upload.
static void __attribute__((thiscall)) touch_hook(BYTE *parameter) {
    BYTE *binding = *(BYTE **)(parameter + 0xc);
    if (!values_active) {
        ++*(volatile uint32_t *)(parameter + 0x10);
        ++*(volatile uint32_t *)(binding + 0x20);
        return;
    }
    InterlockedIncrement((volatile LONG *)(parameter + 0x10));
    InterlockedIncrement((volatile LONG *)(binding + 0x20));
    ValueState *state = drawing_values();
    if (contexts_split && (!state || !state->in_context) && in_library_copy(parameter)) InterlockedIncrement(&escaped_touches);
    if (!state) return;
    if (state->in_context) {
        context_touch(state, parameter, binding, __builtin_return_address(0));
        return;
    }
    if (state->depth) {
        for (uint32_t i = 0; i < state->touched_count; i++)
            if (state->touched[i] == binding) return;
        if (state->touched_count < MAX_TOUCHED) state->touched[state->touched_count++] = binding;
        return;
    }
    state->unwrapped_touches++;
    note_caller(unwrapped_callers, &unwrapped_caller_count, __builtin_return_address(0));
}

static WrappedClass *wrapped_class(void **vtable) {
    uint32_t mask = WRAPPED_CLASSES - 1;
    for (uint32_t i = (uint32_t)((uintptr_t)vtable >> 3) * 0x9e3779b9u & mask;; i = (i + 1) & mask) {
        void **entry = *(void **volatile *)&wrapped_classes[i].vtable;
        if (entry == vtable || !entry) return &wrapped_classes[i];
    }
}

static void lock_values(ValueState *state) {
    if (TryAcquireSRWLockExclusive(&value_lock)) return;
    LONGLONG waiting = now_ticks();
    AcquireSRWLockExclusive(&value_lock);
    state->wait_ticks += now_ticks() - waiting;
}

// Lock-free lookup; entries are added under the value lock and never removed.
static BindingInfo *binding_info(BYTE *binding, BOOL create) {
    uint32_t mask = BINDING_INFOS - 1;
    for (uint32_t i = (uint32_t)((uintptr_t)binding >> 3) * 0x9e3779b9u & mask;; i = (i + 1) & mask) {
        BindingInfo *info = &binding_infos[i];
        BYTE *entry = *(BYTE *volatile *)&info->binding;
        if (entry == binding) return info;
        if (entry) continue;
        if (!create) return NULL;
        BYTE *buffer = *(BYTE **)(binding + 0x1c);
        uint32_t bytes = *(uint32_t *)(buffer + 0x2c);
        if (2 * (binding_info_count + 1) > BINDING_INFOS) {
            if (!binding_infos_full++)
                api->log_line("overhaul: more than %d constant buffers written in split views; the rest count as varying", BINDING_INFOS / 2);
            return NULL;
        }
        if (!(info->reference = malloc(bytes ? bytes : 1))) {
            api->log_line("overhaul: out of memory for a constant buffer's reference values");
            __builtin_trap();
        }
        memcpy(info->reference, *(void **)(buffer + 0x28), bytes);
        info->varies = 0;
        binding_info_count++;
        InterlockedExchangePointer((void *volatile *)&info->binding, binding);
        return info;
    }
}

// The buffers the writer just wrote become the thread's copies, and what the slot is known to write.
static void copy_out_values(ValueState *state, WrappedClass *wrapped, int slot) {
    for (uint32_t i = 0; i < state->touched_count; i++) {
        BYTE *binding = state->touched[i], *buffer = *(BYTE **)(binding + 0x1c);
        if (!buffer) continue;
        BYTE **learned = wrapped->learned[slot];
        int k = 0;
        for (; k < LEARNED_BUFFERS; k++) {
            if (learned[k] == binding) break;
            if (!learned[k]) {
                learned[k] = binding;
                break;
            }
        }
        if (k == LEARNED_BUFFERS && !learned_full++)
            api->log_line("overhaul: a writer slot wrote more than %d constant buffers; the rest are not put back before it", LEARNED_BUFFERS);
        ValueCopy *copy = value_copy(state, buffer, TRUE);
        if (!copy) continue;
        copy->binding = binding;
        memcpy(copy->data, *(void **)(buffer + 0x28), *(uint32_t *)(buffer + 0x2c));
        copy->version = *(volatile uint32_t *)(binding + 0x20);
        copy->changed = 1;
        state->copies_out++;
    }
}

typedef struct {
    void *view, *a, *b, *c;
} WriterArguments;

static BYTE run_writer(int slot, void *original, BYTE *effect, const WriterArguments *args) {
    switch (slot) {
    case 0: return ((GroupWriterFn)original)(effect, args->view, args->a, (uint32_t)(uintptr_t)args->b, args->c);
    case 1: return ((SingleWriterFn)original)(effect, args->view, (uint32_t)(uintptr_t)args->a, args->b);
    case 2: return ((UnitWriterFn)original)(effect, args->view, args->a, args->b, args->c);
    default: return ((MaterialWriterFn)original)(effect, args->view, args->a);
    }
}

static BYTE call_writer(int slot, BYTE *effect, const WriterArguments *args) {
    void **vtable = *(void ***)effect;
    WrappedClass *wrapped = wrapped_class(vtable);
    if (wrapped->vtable != vtable) {
        api->log_line("overhaul: a wrapped writer was called through a vtable it does not know");
        __builtin_trap();
    }
    void *original = wrapped->original[slot];
    ValueState *state = drawing_values();
    // A writer the effect's own writer calls runs inside its lock.
    if (!state || state->depth) return run_writer(slot, original, effect, args);
    if (state->contexts && !wrapped->context_locked[slot]) {
        __attribute__((aligned(16))) BYTE view[CACHED_EFFECT_BYTES];
        BYTE *target = effect;
        BOOL cached = original == (void *)(engine + VARIANT_CACHED_WRITER_RVA) || original == (void *)(engine + VARIANT_B_CACHED_WRITER_RVA);
        BYTE *base = cached ? copy_base(state, *(BYTE **)(effect + 0x74)) : NULL;
        if (base) {
            memcpy(view, effect, original == (void *)(engine + VARIANT_B_CACHED_WRITER_RVA) ? CACHED_EFFECT_B_BYTES : CACHED_EFFECT_BYTES);
            *(BYTE **)(view + 0x74) = base;
            target = view;
        }
        state->depth = 1;
        state->in_context = TRUE;
        state->writer_lock = &wrapped->context_locked[slot];
        BYTE result = run_writer(slot, original, target, args);
        state->in_context = FALSE;
        state->depth = 0;
        state->context_calls++;
        return result;
    }
    if (state->contexts) wrapped->writes[slot] = 1;
    state->touched_count = 0;
    // A slot runs unlocked while every buffer it touches only ever holds the bytes of its first write, the same for every
    // thread; one that varies locks the slot from then on.
    if (!wrapped->writes[slot]) {
        state->depth = 1;
        BYTE result = run_writer(slot, original, effect, args);
        state->depth = 0;
        state->unlocked_calls++;
        if (!state->touched_count) return result;
        BOOL constant = TRUE;
        for (uint32_t i = 0; i < state->touched_count && constant; i++) {
            BYTE *buffer = *(BYTE **)(state->touched[i] + 0x1c);
            BindingInfo *info = buffer ? binding_info(state->touched[i], FALSE) : NULL;
            constant = info && !info->varies && !memcmp(info->reference, *(void **)(buffer + 0x28), *(uint32_t *)(buffer + 0x2c));
        }
        if (constant) return result;
        lock_values(state);
        for (uint32_t i = 0; i < state->touched_count; i++) {
            BYTE *buffer = *(BYTE **)(state->touched[i] + 0x1c);
            BindingInfo *info = buffer ? binding_info(state->touched[i], TRUE) : NULL;
            if (info && memcmp(info->reference, *(void **)(buffer + 0x28), *(uint32_t *)(buffer + 0x2c))) info->varies = 1;
            if (!info || info->varies) wrapped->writes[slot] = 1;
        }
        copy_out_values(state, wrapped, slot);
        ReleaseSRWLockExclusive(&value_lock);
        return result;
    }
    lock_values(state);
    // Each buffer the slot writes holds this thread's values when its writer runs: put back where another thread changed
    // it, or, the first time in the range, taken as it stands.
    BYTE **learned = wrapped->learned[slot];
    for (int k = 0; k < LEARNED_BUFFERS && learned[k]; k++) {
        BYTE *buffer = *(BYTE **)(learned[k] + 0x1c);
        if (!buffer) continue;
        ValueCopy *copy = value_copy(state, buffer, FALSE);
        if (!copy) {
            if (!(copy = value_copy(state, buffer, TRUE))) continue;
            copy->binding = learned[k];
            memcpy(copy->data, *(void **)(buffer + 0x28), *(uint32_t *)(buffer + 0x2c));
            copy->version = *(volatile uint32_t *)(learned[k] + 0x20);
            state->seeds++;
            continue;
        }
        uint32_t version = *(volatile uint32_t *)(copy->binding + 0x20);
        if (version == copy->version) continue;
        memcpy(*(void **)(buffer + 0x28), copy->data, *(uint32_t *)(buffer + 0x2c));
        copy->version = (uint32_t)InterlockedIncrement((volatile LONG *)(copy->binding + 0x20));
        state->restores++;
    }
    state->depth = 1;
    BYTE result = run_writer(slot, original, effect, args);
    state->depth = 0;
    copy_out_values(state, wrapped, slot);
    state->calls++;
    ReleaseSRWLockExclusive(&value_lock);
    return result;
}

static BYTE __attribute__((thiscall)) group_writer_wrapper(BYTE *effect, void *view, const uint32_t *ids, uint32_t count, void *k) {
    WriterArguments args = {view, (void *)ids, (void *)(uintptr_t)count, k};
    return call_writer(0, effect, &args);
}

static BYTE __attribute__((thiscall)) single_writer_wrapper(BYTE *effect, void *view, uint32_t id, void *k) {
    WriterArguments args = {view, (void *)(uintptr_t)id, k, NULL};
    return call_writer(1, effect, &args);
}

static BYTE __attribute__((thiscall)) unit_writer_wrapper(BYTE *effect, void *view, const uint32_t *ids, void *k, void *record) {
    WriterArguments args = {view, (void *)ids, k, record};
    return call_writer(2, effect, &args);
}

static BYTE __attribute__((thiscall)) material_writer_wrapper(BYTE *effect, void *view, void *k) {
    WriterArguments args = {view, k, NULL, NULL};
    return call_writer(3, effect, &args);
}

static void *const writer_wrappers[WRITER_SLOTS] = {(void *)group_writer_wrapper, (void *)single_writer_wrapper, (void *)unit_writer_wrapper,
                                                     (void *)material_writer_wrapper};

static BYTE *job_item(const Job *job);

// On main, before a split view's workers start: the writer slots of each effect class it draws point at the wrappers from
// then on (they pass straight through outside a split view's ranges).
static void wrap_writers(const ViewSlot *slot) {
    void **last = NULL;
    for (uint32_t j = 0; j < slot->job_count; j++) {
        void **vtable = **(void ****)(job_item(&slot->jobs[j]) + 0x8);
        if (vtable == last) continue;
        last = vtable;
        WrappedClass *wrapped = wrapped_class(vtable);
        if (wrapped->vtable) continue;
        if (2 * (wrapped_class_count + 1) > WRAPPED_CLASSES) {
            if (!wrapped_classes_full++)
                api->log_line("overhaul: more than %d effect classes to wrap; the rest write the shared buffers", WRAPPED_CLASSES);
            continue;
        }
        DWORD protection;
        if (!VirtualProtect(vtable, 0x5c, PAGE_READWRITE, &protection)) {
            api->log_line("overhaul: VirtualProtect of an effect vtable failed: error %lu", GetLastError());
            continue;
        }
        for (int s = 0; s < WRITER_SLOTS; s++) wrapped->original[s] = vtable[writer_slots[s] / 4];
        // Published, originals first, before the slots point at the wrappers that look it up.
        InterlockedExchangePointer((void *volatile *)&wrapped->vtable, vtable);
        wrapped_class_count++;
        for (int s = 0; s < WRITER_SLOTS; s++) vtable[writer_slots[s] / 4] = writer_wrappers[s];
        VirtualProtect(vtable, 0x5c, protection, &protection);
    }
}

// At the join: the engine's shadows get the values the view's last writer of each left, ranges in draw order, as if one
// thread had drawn the view.
static void write_back_values(const ValueState *state) {
    for (uint32_t i = 0; i < state->active_count; i++) {
        const ValueCopy *copy = &state->copies[state->active[i]];
        memcpy(*(void **)(copy->buffer + 0x28), copy->data, *(uint32_t *)(copy->buffer + 0x2c));
        InterlockedIncrement((volatile LONG *)(copy->binding + 0x20));
    }
    if (!state->contexts) return;
    for (uint32_t b = 0; b < contexts.buffer_count; b++) {
        if (!state->library_copies[b].changed) continue;
        const LibraryBuffer *buffer = &contexts.buffers[b];
        memcpy(buffer->shadow, state->library_copies[b].data, buffer->bytes);
        for (uint32_t k = 0; k < buffer->binding_count; k++) InterlockedIncrement((volatile LONG *)(buffer->bindings[k] + 0x20));
    }
}

// The node's instance-texture function reads the library through the node's SCENE pointer (+0x74). It first flushes a
// pending list under the node's own lock, so that happens on the real node before the copy.
static void __attribute__((thiscall)) node_texture_hook(BYTE *node, uint32_t index) {
    ValueState *state = contexts_split ? drawing_values() : NULL;
    BYTE *base = state && state->in_context ? copy_base(state, *(BYTE **)(node + 0x74)) : NULL;
    if (!base) {
        original_node_texture(node, index);
        return;
    }
    if (node[0x1a0]) ((NodeFlushFn)(engine + NODE_FLUSH_RVA))(node + 0x17c);
    __attribute__((aligned(16))) BYTE view[CACHED_NODE_BYTES];
    memcpy(view, node, sizeof view);
    *(BYTE **)(view + 0x74) = base;
    original_node_texture(view, index);
}

// The library getter (0x1141ee30): a writer running in a thread's library copy gets the copy.
static BYTE *__attribute__((thiscall)) library_getter_hook(BYTE *holder) {
    BYTE *library = *(BYTE **)(holder + SCENE_OFFSET) + LIBRARY_OFFSET;
    if (!contexts_split) return library;
    ValueState *state = drawing_values();
    return state && state->in_context && library == contexts.shared ? state->library : library;
}

// ---- draws, renderer, kicks, texture slots, uploads ----

static void count_draw(uint64_t instances, uint64_t elements) {
    Worker *worker = current_worker();
    if (worker) {
        WorkCounts *counts = &worker->work[ab_mode()];
        counts->draws++;
        counts->instances += instances;
        counts->elements += elements;
        return;
    }
    WorkCounts *counts = &work[ab_mode()];
    if (*frame_stamp != last_stamp) {
        last_stamp = *frame_stamp;
        counts->frames++;
        if (overhaul_on()) enabled_frames++;
    }
    counts->draws++;
    counts->instances += instances;
    counts->elements += elements;
}

static void __attribute__((thiscall)) draw_hook(BYTE *renderer, UINT vertex_count, UINT start_vertex) {
    count_draw(1, vertex_count);
    ((DrawFn)original_draws[0])(renderer, vertex_count, start_vertex);
}

static void __attribute__((thiscall)) draw_indexed_hook(BYTE *renderer, UINT index_count, UINT start_index, INT base_vertex, UINT vertex_count) {
    count_draw(1, index_count);
    ((DrawIndexedFn)original_draws[1])(renderer, index_count, start_index, base_vertex, vertex_count);
}

static void __attribute__((thiscall)) draw_indexed_instanced_hook(BYTE *renderer, UINT index_count, UINT instances, UINT start_index, INT base_vertex,
                                                                  UINT vertex_count, UINT start_instance) {
    count_draw(instances, (uint64_t)index_count * instances);
    ((DrawIndexedInstancedFn)original_draws[2])(renderer, index_count, instances, start_index, base_vertex, vertex_count, start_instance);
}

static void __attribute__((thiscall)) draw_instanced_hook(BYTE *renderer, UINT vertex_count, UINT instances, UINT start_vertex, UINT start_instance) {
    count_draw(instances, (uint64_t)vertex_count * instances);
    ((DrawInstancedFn)original_draws[3])(renderer, vertex_count, instances, start_vertex, start_instance);
}

// CB uploads and texture streaming reach the renderer through g_render_device->get_renderer(), not execute's argument.
static BYTE *__attribute__((thiscall)) get_renderer_hook(BYTE *render_device) {
    Worker *worker = current_worker();
    return worker ? worker->renderer : render_device + RENDERER_OFFSET;
}

// Streaming a texture in replaces its device texture, releasing the view other threads may be binding, so while a view
// is split nobody streams: main's and the workers' kicks run after the join, and the texture streams one view later.
static BYTE __attribute__((thiscall)) kick_stream_hook(BYTE *texture, BYTE *renderer) {
    Worker *worker = current_worker();
    if (!worker && GetCurrentThreadId() != split_thread) return stream_in(*(BYTE **)(texture + 0x10), renderer);
    Kicks *kicks = worker ? &worker->kicks : &main_kicks;
    if (kicks->count < MAX_KICKS) kicks->textures[kicks->count++] = texture;
    else kicks->dropped++;
    return 1;
}

static void run_kicks(Kicks *kicks, BYTE *renderer) {
    for (int i = 0; i < kicks->count; i++) stream_in(*(BYTE **)(kicks->textures[i] + 0x10), renderer);
    kicks->count = 0;
}

// The worker's copy of a slot in its current range, made from the engine's slot on its first set there; NULL when the
// range has none (the commit then reads the engine's slot: a range's first group is forced to set every slot it needs).
static TextureCopy *texture_copy(Worker *worker, BYTE *slot, BOOL create) {
    uint32_t mask = TEXTURE_COPIES - 1;
    for (uint32_t i = (uint32_t)((uintptr_t)slot >> 4) * 0x9e3779b9u & mask;; i = (i + 1) & mask) {
        TextureCopy *copy = &worker->texture_copies[i];
        if (copy->generation == worker->texture_generation) {
            if (copy->slot == slot) return copy;
            continue;
        }
        if (!create) return NULL;
        if (2 * (worker->texture_copy_count + 1) > TEXTURE_COPIES) {
            if (!texture_copies_full++)
                api->log_line("overhaul: a worker range set more than %d texture slots; the rest are set in the engine's", TEXTURE_COPIES / 2);
            return NULL;
        }
        worker->texture_copy_count++;
        copy->slot = slot;
        copy->generation = worker->texture_generation;
        memcpy(copy->copy, slot, SHADER_TEXTURE_SIZE);
        return copy;
    }
}

// Library slots the engine sets are remembered, so each range's library copy starts from their current values.
static void note_library_slot(const BYTE *slot) {
    uint32_t index = (uint32_t)(slot - contexts.shared) / 4;
    if (contexts.slot_marks[index]) return;
    contexts.slot_marks[index] = 1;
    LONG next = InterlockedIncrement(&contexts.slot_count) - 1;
    if (next < MAX_LIBRARY_SLOTS) contexts.slot_offsets[next] = index * 4;
}

// SHADER_TEXTURE::set (0x11449ef0), the one writer of texture slots (202 call sites): a worker's lands in its copy.
static BYTE *__attribute__((thiscall)) texture_set_hook(BYTE *slot, BYTE *texture) {
    BYTE *passed = slot;
    // The commits read the engine's slots: a set in a thread's library copy is one of the engine's, and the copy's slot
    // follows it for what the thread reads back.
    ValueState *state = contexts_split ? drawing_values() : NULL;
    BOOL in_copy = state && state->library && slot >= state->library && slot + SHADER_TEXTURE_SIZE <= state->library + LIBRARY_SIZE;
    if (in_copy) slot = contexts.shared + (slot - state->library);
    if (contexts.slot_marks && contexts.shared && slot >= contexts.shared && slot + SHADER_TEXTURE_SIZE <= contexts.shared + LIBRARY_SIZE)
        note_library_slot(slot);
    Worker *worker = overhaul_texture_copies ? current_worker() : NULL;
    TextureCopy *copy = worker && worker->texture_generation ? texture_copy(worker, slot, TRUE) : NULL;
    BYTE *set = copy ? copy->copy : slot;
    original_texture_set(set, texture);
    if (copy) worker->texture_sets++;
    if (in_copy) memcpy(passed, set, SHADER_TEXTURE_SIZE);
    return passed;
}

// Device::build_srv_list (0x11452220), behind every stage's SRV commit, as the engine has it (each branch of its switch
// on the resource kind ends in the same store), reading a worker's copy of the slots it set.
static BYTE *__attribute__((thiscall)) build_srv_list_hook(BYTE *list, uint32_t *count, uint32_t *first, BYTE **cache) {
    Worker *worker = current_worker();
    if (!worker || !worker->texture_generation) return original_build_srv_list(list, count, first, cache);
    *count = *first = 0;
    BOOL prefix = TRUE;
    BYTE *entries = *(BYTE **)(list + 0x3c), **views = *(BYTE ***)(list + 0x54);
    for (uint32_t i = 0; i < *(uint32_t *)(list + 0x38); i++) {
        BYTE *slot = *(BYTE **)(entries + 12 * i);
        if (!slot) break;
        TextureCopy *copy = texture_copy(worker, slot, FALSE);
        BYTE *handle = *(BYTE **)((copy ? copy->copy : slot) + 0xc);
        if (prefix && handle == cache[i]) {
            *first = i + 1;
            continue;
        }
        prefix = FALSE;
        cache[i] = handle;
        views[(*count)++] = handle ? *(BYTE **)(handle + 0x24) : NULL;
    }
    return list + 0x4c;
}

// The engine uploads a constant buffer only when its binding's version differs from the one last uploaded, tracked once
// per buffer: right for one timeline, wrong for several. A worker uploads each buffer once per command list at each
// version and leaves the engine's record alone; once another timeline uploaded a buffer, main forgets the engine's record
// of it. Open addressing over entries of the current list; entries of earlier lists count as free.
static UploadedBuffer *list_slot(UploadedBuffer *table, BYTE *key, uint32_t list) {
    UploadedBuffer *slot = &table[((uintptr_t)key >> 4) * 0x9e3779b9u & (CB_TABLE_SIZE - 1)];
    while (slot->buffer && slot->buffer != key && slot->list == list) slot = slot + 1 == table + CB_TABLE_SIZE ? table : slot + 1;
    return slot;
}

// A list that uploaded into copies left the engine's buffers, and so its record, alone.
static void forget_uploads(UploadTracker *uploads) {
    if (!uploads->use_copies)
        for (uint32_t i = 0; i < uploads->uploaded_this_list_count; i++) *(uint32_t *)(uploads->uploaded_this_list[i] + 0x24) = ~0u;
    uploads->uploaded_this_list_count = 0;
}

static CbCopy *copy_slot(CbCopy *table, uint32_t capacity, BYTE *buffer) {
    CbCopy *slot = &table[((uintptr_t)buffer >> 4) * 0x9e3779b9u & (capacity - 1)];
    while (slot->buffer && slot->buffer != buffer) slot = slot + 1 == table + capacity ? table : slot + 1;
    return slot;
}

// The timeline's own buffer for an engine constant buffer. Keyed by the engine's object, whose address a freed buffer can
// hand on to one of another size; a new copy holds nothing yet.
static ID3D11Buffer *cb_copy(UploadTracker *uploads, BYTE *buffer, ID3D11Buffer *engine_buffer, uint32_t bytes, BOOL *created) {
    if (2 * (uploads->copy_count + 1) > uploads->copy_capacity) {
        uint32_t capacity = uploads->copy_capacity ? 2 * uploads->copy_capacity : 4096;
        CbCopy *table = calloc(capacity, sizeof *table);
        if (!table) {
            api->log_line("overhaul: out of memory for %u constant buffer copies", capacity);
            __builtin_trap();
        }
        for (uint32_t i = 0; i < uploads->copy_capacity; i++)
            if (uploads->copies[i].buffer) *copy_slot(table, capacity, uploads->copies[i].buffer) = uploads->copies[i];
        free(uploads->copies);
        uploads->copies = table;
        uploads->copy_capacity = capacity;
    }
    CbCopy *slot = copy_slot(uploads->copies, uploads->copy_capacity, buffer);
    *created = !slot->buffer || slot->bytes != bytes;
    if (!*created) return slot->copy;
    if (slot->buffer) ID3D11Buffer_Release(slot->copy);
    else uploads->copy_count++;
    D3D11_BUFFER_DESC desc;
    ID3D11Buffer_GetDesc(engine_buffer, &desc);
    HRESULT result = ID3D11Device_CreateBuffer(device, &desc, NULL, &slot->copy);
    if (FAILED(result)) {
        api->log_line("overhaul: CreateBuffer for a constant buffer copy failed: 0x%08lx", (unsigned long)result);
        __builtin_trap();
    }
    slot->buffer = buffer;
    slot->bytes = bytes;
    return slot->copy;
}

static BOOL map_buffer(BYTE *buffer, BYTE *renderer, uint32_t bytes, void **destination) {
    return ((MapBufferFn)method(buffer, 0xc))(buffer, renderer, 0, bytes, D3D11_MAP_WRITE_DISCARD, destination, 0) != 0;
}

static void unmap_buffer(BYTE *buffer, BYTE *renderer) {
    ((UnmapBufferFn)method(buffer, 0x10))(buffer, renderer);
}

// The upload loop (0x114520b0) as the engine has it, for main's range of a split view, uploading main's own values where
// it wrote the buffer in its range.
static BYTE *upload_main_values(BYTE *bindings, uint32_t *count, uint32_t *first, BYTE **cache) {
    BYTE *renderer = *(BYTE **)(engine + RENDER_DEVICE_RVA) + RENDERER_OFFSET;
    *count = *first = 0;
    BOOL prefix = TRUE, always = engine[ALWAYS_UPDATE_CB_RVA];
    uint32_t binding_count = *(uint32_t *)(bindings + 4);
    BYTE **binding_list = *(BYTE ***)(bindings + 8);
    for (uint32_t i = 0; i < binding_count; i++) {
        BYTE *binding = binding_list[i];
        if (!binding) break;
        BYTE *buffer = *(BYTE **)(binding + 0x1c);
        uint32_t version = *(uint32_t *)(binding + 0x20), bytes = *(uint32_t *)(buffer + 0x2c);
        if (version != *(uint32_t *)(buffer + 0x24) || always) {
            *(uint32_t *)(buffer + 0x24) = version;
            void *destination;
            if (map_buffer(buffer, renderer, bytes, &destination)) {
                memcpy(destination, value_source(buffer), bytes);
                unmap_buffer(buffer, renderer);
            }
        }
        if (prefix && buffer == cache[i]) {
            *first = i + 1;
            continue;
        }
        prefix = FALSE;
        cache[i] = buffer;
        (*(void ***)(bindings + 0x14))[(*count)++] = *(void **)(buffer + 0x1c);
    }
    return bindings + 0xc;
}

static BYTE *__attribute__((thiscall)) upload_constant_buffers_hook(BYTE *bindings, uint32_t unused, uint32_t *count, uint32_t *first, BYTE **cache) {
    Worker *worker = current_worker();
    if (!worker) {
        if (drawing_values()) return upload_main_values(bindings, count, first, cache);
        return original_upload_constant_buffers(bindings, unused, count, first, cache);
    }
    UploadTracker *uploads = &worker->uploads;
    ID3D11DeviceContext *context = worker->context;
    *count = *first = 0;
    BOOL prefix = TRUE, always = engine[ALWAYS_UPDATE_CB_RVA];
    uint32_t binding_count = *(uint32_t *)(bindings + 4);
    BYTE **binding_list = *(BYTE ***)(bindings + 8);
    for (uint32_t i = 0; i < binding_count; i++) {
        BYTE *binding = binding_list[i];
        if (!binding) break;
        BYTE *buffer = *(BYTE **)(binding + 0x1c);
        uint32_t version = *(uint32_t *)(binding + 0x20);
        // Half full keeps every probe short and guarantees it ends.
        if (uploads->uploaded_this_list_count >= CB_TABLE_SIZE / 2) {
            api->log_line("overhaul: a worker uploaded more than %d constant buffers in one command list", CB_TABLE_SIZE / 2);
            __builtin_trap();
        }
        uint32_t bytes = *(uint32_t *)(buffer + 0x2c);
        ID3D11Buffer *engine_buffer = *(ID3D11Buffer **)(buffer + 0x1c);
        // Without a D3D buffer the engine's map fails and NULL is bound; the copy follows it.
        BOOL created = FALSE;
        ID3D11Buffer *bound = uploads->use_copies && engine_buffer ? cb_copy(uploads, buffer, engine_buffer, bytes, &created) : engine_buffer;
        UploadedBuffer *slot = list_slot(uploads->uploaded, buffer, uploads->list_number);
        if (always || created || slot->buffer != buffer || slot->list != uploads->list_number || slot->version != version) {
            if (bound != engine_buffer) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                HRESULT result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)bound, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (FAILED(result)) {
                    api->log_line("overhaul: Map of a constant buffer copy failed: 0x%08lx", (unsigned long)result);
                    __builtin_trap();
                }
                memcpy(mapped.pData, value_source(buffer), bytes);
                ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)bound, 0);
            } else {
                void *destination;
                if (map_buffer(buffer, worker->renderer, bytes, &destination)) {
                    memcpy(destination, value_source(buffer), bytes);
                    unmap_buffer(buffer, worker->renderer);
                }
            }
            if (slot->buffer != buffer || slot->list != uploads->list_number) uploads->uploaded_this_list[uploads->uploaded_this_list_count++] = buffer;
            *slot = (UploadedBuffer){buffer, version, uploads->list_number};
        }
        if (prefix && buffer == cache[i]) {
            *first = i + 1;
            continue;
        }
        prefix = FALSE;
        cache[i] = buffer;
        (*(void ***)(bindings + 0x14))[(*count)++] = bound;
    }
    return bindings + 0xc;
}

// ---- a worker's private draw items, ring clones, programs and group views ----

static __attribute__((noreturn)) void unexpected_recorder_call(void) {
    api->log_line("overhaul: the engine called a recorder method the range recorder does not implement");
    __builtin_trap();
}

static BYTE *job_item(const Job *job) {
    return job->kind == JOB_SINGLE ? *(BYTE **)(job->object + 0x8) : *(BYTE **)job->object;
}

static const uint32_t *job_ids(const Job *job) {
    if (job->kind == JOB_PACKET) return ((const Packet *)job->object)->ids;
    return job->kind == JOB_SINGLE ? *(const uint32_t **)(job->object + 0x14) : (const uint32_t *)(job->object + 0x4);
}

static uint32_t job_instances(const Job *job) {
    if (job->kind == JOB_PACKET) return ((const Packet *)job->object)->count;
    return *(uint32_t *)(job->object + (job->kind == JOB_SINGLE ? 0x10 : 0x404));
}

static uint32_t job_flags(const Job *job) {
    if (job->kind == JOB_PACKET) return ((const Packet *)job->object)->flags;
    return job->kind == JOB_SINGLE ? 0 : *(uint32_t *)(job->object + 0x408);
}

static size_t packet_size(uint32_t count) {
    return (sizeof(Packet) + count * sizeof(uint32_t) + 7) & ~(size_t)7;
}

// The view's groups copied out of the engine's recorders, so the buckets are free for the next record while they draw.
static void copy_packets(ViewSlot *slot) {
    Job *jobs = slot->jobs;
    size_t bytes = 0;
    for (uint32_t i = 0; i < slot->job_count; i++) bytes += packet_size(job_instances(&jobs[i]));
    if (bytes > slot->packet_capacity) {
        free(slot->packet_arena);
        slot->packet_capacity = bytes * 2;
        slot->packet_arena = malloc(slot->packet_capacity);
        if (!slot->packet_arena) {
            api->log_line("overhaul: out of memory for %u bytes of packets", (unsigned)slot->packet_capacity);
            __builtin_trap();
        }
    }
    BYTE *at = slot->packet_arena;
    for (uint32_t i = 0; i < slot->job_count; i++) {
        Packet *packet = (Packet *)at;
        packet->item = job_item(&jobs[i]);
        packet->count = job_instances(&jobs[i]);
        packet->flags = job_flags(&jobs[i]);
        memcpy(packet->ids, job_ids(&jobs[i]), packet->count * sizeof(uint32_t));
        jobs[i].object = at;
        jobs[i].kind = JOB_PACKET;
        at += packet_size(packet->count);
    }
}

static ID3D11Buffer *create_clone_buffer(D3D11_BUFFER_DESC desc, uint32_t bytes) {
    desc.ByteWidth = bytes;
    ID3D11Buffer *buffer;
    HRESULT result = ID3D11Device_CreateBuffer(device, &desc, NULL, &buffer);
    if (FAILED(result)) {
        api->log_line("overhaul: CreateBuffer of %u bytes for a ring clone failed: 0x%08lx", bytes, (unsigned long)result);
        __builtin_trap();
    }
    return buffer;
}

// The clone's buffer and capacity; a stamp unlike the frame counter makes the first map a DISCARD, as a deferred context
// requires.
static void select_clone_buffer(Clone *clone) {
    BYTE *copy = clone->copy, *buffer_copy = copy + RING_HOLDER_SIZE;
    *(uint32_t *)(copy + 0x14) = *(uint32_t *)(copy + 0x18) = *(uint32_t *)(buffer_copy + 0x8) = clone->bytes;
    *(ID3D11Buffer **)(buffer_copy + 0x18) = clone->buffer;
    *(uint32_t *)(copy + 0x5c) = frame_counter() ^ 1;
}

// A request past a small clone's capacity grows it.
static void grow_clone(Worker *worker, BYTE *holder, uint32_t bytes) {
    for (int i = 0; i < worker->clone_count; i++) {
        Clone *clone = &worker->clones[i];
        if (clone->copy != holder) continue;
        uint32_t size = clone->bytes;
        while (size < bytes) size *= 2;
        D3D11_BUFFER_DESC desc;
        ID3D11Buffer_GetDesc(clone->buffer, &desc);
        ID3D11Buffer_Release(clone->buffer);
        clone->buffer = create_clone_buffer(desc, size);
        clone->bytes = size;
        clone->grows++;
        select_clone_buffer(clone);
        return;
    }
}

static BYTE *clone_holder(Worker *worker, BYTE *holder) {
    void *vtable = *(void **)holder;
    if (vtable != engine + INSTANCE_RING_VTABLE_RVA && vtable != engine + GEOMETRY_RING_VTABLE_RVA) return holder;
    for (int i = 0; i < worker->clone_count; i++)
        if (worker->clones[i].original == holder) return worker->clones[i].copy;
    BYTE *buffer = *(BYTE **)(holder + 0x10);
    ID3D11Buffer *d3d_buffer = buffer ? *(ID3D11Buffer **)(buffer + 0x18) : NULL;
    if (worker->clone_count == MAX_CLONES || !d3d_buffer) {
        worker->shared_holders++;
        return holder;
    }
    D3D11_BUFFER_DESC desc;
    ID3D11Buffer_GetDesc(d3d_buffer, &desc);
    BYTE *copy = _aligned_malloc(RING_HOLDER_SIZE + VERTEX_BUFFER_SIZE, 16), *buffer_copy = copy + RING_HOLDER_SIZE;
    if (!copy) {
        api->log_line("overhaul: out of memory for a ring clone");
        __builtin_trap();
    }
    memcpy(copy, holder, RING_HOLDER_SIZE);
    memcpy(buffer_copy, buffer, VERTEX_BUFFER_SIZE);
    *(void **)buffer_copy = engine + VERTEX_BUFFER_VTABLE_RVA;
    buffer_copy[0x1c] = 0;
    *(BYTE **)(copy + 0x10) = buffer_copy;
    *(uint32_t *)(copy + 0x1c) = 0;
    memset(copy + 0x44, 0, 0x54 - 0x44);
    copy[0x54] = 0;
    Clone *clone = &worker->clones[worker->clone_count++];
    clone->original = holder;
    clone->copy = copy;
    clone->bytes = desc.ByteWidth < RING_CLONE_BYTES ? desc.ByteWidth : RING_CLONE_BYTES;
    clone->buffer = create_clone_buffer(desc, clone->bytes);
    clone->grows = 0;
    select_clone_buffer(clone);
    return copy;
}

static size_t chunker_size(BYTE *chunker) {
    void *vtable = *(void **)chunker;
    for (size_t i = 0; i < sizeof chunker_classes / sizeof *chunker_classes; i++)
        if (vtable == engine + chunker_classes[i].vtable_rva) return chunker_classes[i].size;
    api->log_line("overhaul: a draw item's chunker has vtable %p, a class this module does not know the size of", vtable);
    __builtin_trap();
}

// A group-instance mesh as one view's draws must see it: record writes the view's instance count (+0x124) and pass bit
// (+0x128) into the shared mesh, and the chunker's size and the gatherer read them while the next view may already be
// recording. The view also holds the thread's own chunker (+0x120), and the chunker's mesh (+0x18) points back at it.
static BOOL bind_group_view(BYTE *view, BYTE *chunker, const Job *job) {
    BYTE *mesh = *(BYTE **)(job_item(job) + 0x8);
    if (!overhaul_group_gather || *(BYTE **)mesh != engine + GROUP_INSTANCE_MESH_VTABLE_RVA) return FALSE;
    memcpy(view, mesh, GROUP_MESH_VIEW_SIZE);
    *(BYTE **)(view + 0x120) = chunker;
    *(uint32_t *)(view + 0x124) = job->group_count;
    *(uint32_t *)(view + 0x128) = job->group_bit;
    *(BYTE **)(chunker + 0x18) = view;
    return TRUE;
}

static void prepare_private_item(Worker *worker, const Job *job) {
    BYTE *item = job_item(job);
    memcpy(worker->item, item, DRAW_ITEM_SIZE);
    BYTE *chunker = *(BYTE **)(item + 0xc);
    memcpy(worker->chunker, chunker, chunker_size(chunker));
    *(BYTE **)(worker->item + 0xc) = worker->chunker;
    worker->group_mesh_of = bind_group_view(worker->group_mesh, worker->chunker, job) ? *(BYTE **)(item + 0x8) : NULL;
    BYTE *streams = *(BYTE **)(item + 0x10);
    if (streams) {
        memcpy(worker->streams, streams, STREAMS_SIZE);
        uint32_t count = *(uint32_t *)(streams + 0x4);
        if (count > MAX_STREAMS) {
            api->log_line("overhaul: a draw item has %u vertex streams, more than %d", count, MAX_STREAMS);
            __builtin_trap();
        }
        memcpy(worker->stream_entries, *(BYTE **)streams, count * STREAM_ENTRY_SIZE);
        for (uint32_t i = 0; i < count; i++) {
            BYTE **holder = (BYTE **)(worker->stream_entries + i * STREAM_ENTRY_SIZE);
            if (*holder) *holder = clone_holder(worker, *holder);
        }
        *(BYTE **)worker->streams = worker->stream_entries;
        BYTE **instance_holder = (BYTE **)(worker->streams + 0x8);
        if (*instance_holder) *instance_holder = clone_holder(worker, *instance_holder);
        *(BYTE **)(worker->item + 0x10) = worker->streams;
    }
    BYTE *index_stream = *(BYTE **)(item + 0x14);
    if (index_stream) {
        memcpy(worker->index_stream, index_stream, INDEX_STREAM_SIZE);
        *(BYTE **)(worker->item + 0x14) = worker->index_stream;
    }
}

// Main's range draws the engine's items, except a group-instance job, which gets a private item and chunker.
static void prepare_main_group_item(const Job *job) {
    main_group.mesh_of = NULL;
    BYTE *item = job_item(job), *mesh = *(BYTE **)(item + 0x8), *chunker = *(BYTE **)(item + 0xc);
    if (!overhaul_group_gather || *(BYTE **)mesh != engine + GROUP_INSTANCE_MESH_VTABLE_RVA) return;
    memcpy(main_group.item, item, DRAW_ITEM_SIZE);
    memcpy(main_group.chunker, chunker, chunker_size(chunker));
    *(BYTE **)(main_group.item + 0xc) = main_group.chunker;
    bind_group_view(main_group.view, main_group.chunker, job);
    main_group.mesh_of = mesh;
    main_group.job = job;
}

static BYTE *group_view_for(BYTE *mesh) {
    Worker *worker = current_worker();
    if (worker) return worker->group_mesh_of == mesh ? worker->group_mesh : NULL;
    return main_group.mesh_of == mesh ? main_group.view : NULL;
}

// The gatherer reads only its mesh (+0x4) and, through it, the fields up to GROUP_MESH_VIEW_SIZE; it writes none.
static uint32_t __attribute__((thiscall)) group_gather_hook(BYTE *gatherer, BYTE *mapped, const uint32_t *ids) {
    BYTE *view = split_thread ? group_view_for(*(BYTE **)(gatherer + 4)) : NULL;
    if (!view) return original_group_gather(gatherer, mapped, ids);
    void *fake[2] = {*(void **)gatherer, view};
    return original_group_gather((BYTE *)fake, mapped, ids);
}

// Program objects keep the lists their commits build (CBs, samplers, SRVs, UAVs) in output arrays inside the object, and
// the D3D runtime reads the list it is given twice: two threads committing one program would hand each other's lists to
// the runtime mid-call. A worker's renderer binds private copies instead, each with its own output arrays; the binding
// lists and the D3D shader stay shared.
static uint32_t output_entries(const BYTE *program, int i) {
    const uint32_t *output = (const uint32_t *)(program + program_outputs[i].output), *bindings = (const uint32_t *)(program + program_outputs[i].bindings);
    uint32_t entries = output[0] > output[1] ? output[0] : output[1];
    return bindings[1] > entries ? bindings[1] : entries;
}

// Copies the program into its clone, keeping the clone's own output arrays, grown when the program's lists grew.
static void fill_program_clone(ProgramClone *clone) {
    void *outputs[PROGRAM_OUTPUTS];
    for (int i = 0; i < PROGRAM_OUTPUTS; i++) outputs[i] = *(void **)(clone->copy + program_outputs[i].output + 8);
    memcpy(clone->copy, clone->original, PROGRAM_SIZE);
    for (int i = 0; i < PROGRAM_OUTPUTS; i++) {
        uint32_t entries = output_entries(clone->original, i);
        if (entries > clone->capacity[i]) {
            free(outputs[i]);
            outputs[i] = calloc(entries, sizeof(void *));
            if (!outputs[i]) {
                api->log_line("overhaul: out of memory for a program copy's output array");
                __builtin_trap();
            }
            clone->capacity[i] = entries;
        }
        *(void **)(clone->copy + program_outputs[i].output + 8) = outputs[i];
    }
}

static ProgramClone *insert_program(Worker *worker, ProgramClone clone) {
    uint32_t mask = worker->program_capacity - 1;
    for (uint32_t slot = (uint32_t)((uintptr_t)clone.original >> 4) * 0x9e3779b9u & mask;; slot = (slot + 1) & mask)
        if (!worker->programs[slot].original) {
            worker->programs[slot] = clone;
            worker->program_count++;
            return &worker->programs[slot];
        }
}

static BYTE *program_clone(Worker *worker, BYTE *program) {
    uint32_t mask = worker->program_capacity - 1;
    for (uint32_t slot = (uint32_t)((uintptr_t)program >> 4) * 0x9e3779b9u & mask;; slot = (slot + 1) & mask) {
        if (worker->programs[slot].original == program) return worker->programs[slot].copy;
        if (!worker->programs[slot].original) break;
    }
    if (2 * (worker->program_count + 1) > worker->program_capacity) {
        ProgramClone *old = worker->programs;
        uint32_t old_capacity = worker->program_capacity;
        worker->program_capacity *= 2;
        worker->program_count = 0;
        worker->programs = calloc(worker->program_capacity, sizeof *worker->programs);
        if (!worker->programs) {
            api->log_line("overhaul: out of memory for %u program copies", worker->program_capacity);
            __builtin_trap();
        }
        for (uint32_t i = 0; i < old_capacity; i++)
            if (old[i].original) insert_program(worker, old[i]);
        free(old);
    }
    ProgramClone clone = {program, calloc(1, PROGRAM_SIZE), {0}};
    if (!clone.copy) {
        api->log_line("overhaul: out of memory for a program copy");
        __builtin_trap();
    }
    ProgramClone *inserted = insert_program(worker, clone);
    fill_program_clone(inserted);
    return inserted->copy;
}

#define SET_SHADER_WRAPPER(stage)                                                                                               \
    static uint32_t __attribute__((thiscall)) set_shader_##stage(BYTE *renderer, BYTE *program) {                              \
        return original_set_shaders[stage](renderer, program ? program_clone(current_worker(), program) : program);           \
    }
SET_SHADER_WRAPPER(0)
SET_SHADER_WRAPPER(1)
SET_SHADER_WRAPPER(2)
SET_SHADER_WRAPPER(3)
SET_SHADER_WRAPPER(4)
SET_SHADER_WRAPPER(5)
static void *const set_shader_wrappers[STAGES] = {(void *)set_shader_0, (void *)set_shader_1, (void *)set_shader_2,
                                                  (void *)set_shader_3, (void *)set_shader_4, (void *)set_shader_5};

// Only a Variant node drawing without instancing writes the bone palette; forced on an instanced one, the writer would
// read a pose the item need not have.
static BOOL draws_without_instancing(const Job *job) {
    BYTE *effect = *(BYTE **)(job_item(job) + 0x8);
    void *vtable = *(void **)effect;
    if (vtable == engine + VARIANT_WEIGHTED_B_VTABLE_RVA) effect = *(BYTE **)(effect + 0x94);
    else if (vtable != engine + VARIANT_WEIGHTED_VTABLE_RVA) return FALSE;
    if (!effect) return FALSE;
    BYTE *node = *(BYTE **)(effect + 0x60);
    return node && !(node[0x1c8] && !node[0x1ca] && *(uint32_t *)(node + 0x1c0));
}

// ---- range recorders ----

static BYTE __attribute__((thiscall)) range_next(RangeRecorder *recorder) {
    if (recorder->next_job == recorder->end_job) return 0;
    recorder->current_index = recorder->next_job++;
    recorder->current = &recorder->jobs[recorder->current_index];
    if (recorder->worker) prepare_private_item(recorder->worker, recorder->current);
    else prepare_main_group_item(recorder->current);
    return 1;
}

static BYTE *__attribute__((thiscall)) range_item(RangeRecorder *recorder) {
    if (recorder->worker) return recorder->worker->item;
    return main_group.mesh_of && main_group.job == recorder->current ? main_group.item : job_item(recorder->current);
}

static const uint32_t *__attribute__((thiscall)) range_begin(RangeRecorder *recorder) {
    return job_ids(recorder->current);
}

static const uint32_t *__attribute__((thiscall)) range_end(RangeRecorder *recorder) {
    return job_ids(recorder->current) + job_instances(recorder->current);
}

static uint32_t __attribute__((thiscall)) range_flags(RangeRecorder *recorder) {
    uint32_t flags = job_flags(recorder->current);
    if (recorder->current_index != recorder->forced_job) return flags;
    flags |= (uint32_t)overhaul_forced_flags;
    if (overhaul_force_bones && draws_without_instancing(recorder->current)) flags |= 0x01;
    return flags;
}

static void *const range_recorder_vtable[RECORDER_SLOTS] = {
    (void *)unexpected_recorder_call, (void *)unexpected_recorder_call, (void *)unexpected_recorder_call, (void *)range_next,
    (void *)range_item, (void *)range_begin, (void *)range_end, (void *)range_flags, (void *)unexpected_recorder_call,
    (void *)unexpected_recorder_call};

// ---- the split ----

// Weighing groups by their instances balanced worse in the CE.
static uint32_t job_weight(const Job *job) {
    return job->kind == JOB_SINGLE ? 1 + job_instances(job) / 256 : 1;
}

static void append_job(ViewSlot *slot, BYTE *object, uint32_t bucket, uint32_t kind, BYTE *recorder) {
    if (slot->job_count == slot->job_capacity) {
        slot->job_capacity = slot->job_capacity ? slot->job_capacity * 2 : 4096;
        slot->jobs = realloc(slot->jobs, slot->job_capacity * sizeof *slot->jobs);
        if (!slot->jobs) {
            api->log_line("overhaul: out of memory for %u jobs", slot->job_capacity);
            __builtin_trap();
        }
    }
    Job *job = &slot->jobs[slot->job_count++];
    *job = (Job){object, bucket, kind, 0, recorder, 0, 0};
    job->weight = job_weight(job);
    BYTE *effect = *(BYTE **)(job_item(job) + 0x8);
    if (*(BYTE **)effect == engine + GROUP_INSTANCE_MESH_VTABLE_RVA) {
        job->group_count = *(uint32_t *)(effect + 0x124);
        job->group_bit = *(uint32_t *)(effect + 0x128);
    }
}

// Groups the view would draw, or -1 for a recorder kind this module does not know. Counted before any sort: the program
// sort need not be a strict weak ordering, so a view handed back to the original must not be sorted twice.
static int64_t count_jobs(BYTE *context) {
    uint32_t bucket_count = *(uint32_t *)(context + 0x10);
    BYTE *recorder_vectors = *(BYTE **)(context + 0x2c);
    uint32_t *record_counts = *(uint32_t **)(context + 0x50);
    int64_t count = 0;
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++) {
        if (!record_counts[bucket]) continue;
        BYTE *vector = recorder_vectors + 12 * bucket;
        BYTE **recorders = *(BYTE ***)(vector + 8);
        for (uint32_t i = 0; i < *(uint32_t *)(vector + 4); i++) {
            void *vtable = *(void **)recorders[i];
            if (vtable == engine + SINGLE_RECORDER_VTABLE_RVA) {
                count++;
            } else if (vtable == engine + BATCH_RECORDER_VTABLE_RVA) {
                count += *(uint32_t *)(recorders[i] + 0x14);
            } else {
                if (!fallback_unknown_recorders++) api->log_line("overhaul: unknown recorder vtable %p; such views run unsplit", vtable);
                return -1;
            }
        }
    }
    return count;
}

// Lists the view's groups in execute's order: buckets in order, the bucket's recorders after its program sort, each
// recorder's groups in order.
static void snapshot_jobs(ViewSlot *slot, BYTE *context, BYTE *view) {
    slot->job_count = 0;
    uint32_t bucket_count = *(uint32_t *)(context + 0x10);
    BYTE *sort_flags = *(BYTE **)(context + 0x20), *recorder_vectors = *(BYTE **)(context + 0x2c);
    uint32_t *record_counts = *(uint32_t **)(context + 0x50);
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++) {
        if (!record_counts[bucket]) continue;
        BYTE *vector = recorder_vectors + 12 * bucket;
        uint32_t count = *(uint32_t *)(vector + 4);
        BYTE **recorders = *(BYTE ***)(vector + 8);
        if (sort_flags[bucket]) sort_bucket(recorders, recorders + count, count, view);
        for (uint32_t i = 0; i < count; i++) {
            BYTE *recorder = recorders[i];
            if (*(void **)recorder == engine + SINGLE_RECORDER_VTABLE_RVA) {
                append_job(slot, recorder, bucket, JOB_SINGLE, recorder);
            } else {
                uint32_t used = *(uint32_t *)(recorder + 0x14);
                BYTE **groups = *(BYTE ***)(recorder + 0x10);
                for (uint32_t g = 0; g < used; g++) append_job(slot, groups[g], bucket, JOB_BATCH, recorder);
            }
        }
    }
}

static void ensure_bucket_capacity(Range *range, uint32_t bucket_count) {
    if (range->bucket_capacity >= bucket_count) return;
    free(range->record_counts);
    free(range->callbacks);
    free(range->recorders);
    free(range->range_recorders);
    free(range->recorder_pointers);
    range->bucket_capacity = bucket_count;
    range->record_counts = calloc(bucket_count, sizeof *range->record_counts);
    range->callbacks = calloc(bucket_count, sizeof *range->callbacks);
    range->recorders = calloc(bucket_count, sizeof *range->recorders);
    range->range_recorders = calloc(bucket_count, sizeof *range->range_recorders);
    range->recorder_pointers = calloc(bucket_count, sizeof *range->recorder_pointers);
    if (!range->record_counts || !range->callbacks || !range->recorders || !range->range_recorders || !range->recorder_pointers) {
        api->log_line("overhaul: out of memory for %u buckets", bucket_count);
        __builtin_trap();
    }
    if (zero_sort_capacity < bucket_count) {
        free(zero_sort_flags);
        zero_sort_flags = calloc(bucket_count, 1);
        zero_sort_capacity = bucket_count;
    }
}

// Begin callbacks run for every bucket, empty ones too, before its draws. Each range runs those of the buckets it draws,
// a bucket split between two ranges in both, and the empty buckets before its first draw.
static void build_range_context(ViewSlot *slot, Range *range, BYTE *context, Worker *worker) {
    const Job *jobs = slot->jobs;
    uint32_t bucket_count = *(uint32_t *)(context + 0x10);
    void **callbacks = *(void ***)(context + 0x5c);
    ensure_bucket_capacity(range, bucket_count);
    memset(range->record_counts, 0, bucket_count * sizeof *range->record_counts);
    memset(range->recorders, 0, bucket_count * sizeof *range->recorders);
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++)
        range->callbacks[bucket] = bucket >= range->first_callback && bucket <= range->last_callback ? callbacks[bucket] : NULL;
    for (uint32_t first = range->first_job; first < range->end_job;) {
        uint32_t bucket = jobs[first].bucket, end = first;
        while (end < range->end_job && jobs[end].bucket == bucket) end++;
        RangeRecorder *recorder = &range->range_recorders[bucket];
        *recorder = (RangeRecorder){.vtable = range_recorder_vtable,
                                    .worker = worker,
                                    .jobs = jobs,
                                    .next_job = first,
                                    .end_job = end,
                                    .forced_job = worker || overhaul_value_copies ? range->first_job : UINT32_MAX,
                                    .current_index = UINT32_MAX};
        range->recorder_pointers[bucket] = recorder;
        range->recorders[bucket] = (RecorderVector){1, 1, &range->recorder_pointers[bucket]};
        range->record_counts[bucket] = end - first;
        first = end;
    }
    memcpy(range->context, context, CONTEXT_SIZE);
    *(uint32_t *)(range->context + 0x10) = bucket_count;
    *(BYTE **)(range->context + 0x20) = zero_sort_flags;
    *(RecorderVector **)(range->context + 0x2c) = range->recorders;
    *(uint32_t **)(range->context + 0x50) = range->record_counts;
    *(void ***)(range->context + 0x5c) = range->callbacks;
}

// The first range gets first_share percent of the others' share.
static void share_ends(int range_count, LONG first_share, double *ends) {
    double main_share = first_share < 0 ? 0 : first_share, shares = main_share + 100.0 * (range_count - 1);
    for (int k = 0; k < range_count; k++) ends[k] = shares ? (main_share + 100.0 * k) / shares : 1;
}

// Ends that would have given last frame's ranges equal times, if each range's time is spread evenly over its weight;
// halfway there from last frame's ends, so a frame's noise does not swing them.
static void balanced_ends(const Balance *balance, int range_count, LONG first_share, double *ends) {
    LONGLONG total = 0;
    for (int k = 0; k < balance->count; k++) total += balance->ticks[k];
    if (balance->count != range_count || total <= 0) {
        share_ends(range_count, first_share, ends);
        return;
    }
    int segment = 0;
    LONGLONG before = 0;
    for (int k = 0; k < range_count - 1; k++) {
        double target = (double)total * (k + 1) / range_count;
        while (segment < range_count - 1 && before + balance->ticks[segment] < target) before += balance->ticks[segment++];
        double start = segment ? balance->ends[segment - 1] : 0, width = balance->ends[segment] - start;
        double even = start + (balance->ticks[segment] ? (target - before) / balance->ticks[segment] * width : width);
        ends[k] = (balance->ends[k] + even) / 2;
    }
    ends[range_count - 1] = 1;
}

// Range k ends where the weight reaches ends[k] of the view's total; the slot keeps the ends the split reached.
static int split_ranges(ViewSlot *slot, int range_count, uint32_t bucket_count, const double *ends) {
    const Job *jobs = slot->jobs;
    uint32_t job_count = slot->job_count;
    uint64_t total = 0;
    for (uint32_t i = 0; i < job_count; i++) total += jobs[i].weight;
    uint64_t sum = 0;
    uint32_t start = 0;
    int built = 0;
    for (int k = 0; k < range_count && start < job_count; k++) {
        uint32_t end = start;
        uint64_t target = (uint64_t)(total * ends[k]);
        while (end < job_count && (sum < target || end == start || k == range_count - 1)) sum += jobs[end++].weight;
        Range *range = &slot->ranges[built];
        range->first_job = start;
        range->end_job = end;
        range->first_callback = built == 0 ? 0 : jobs[start].bucket == jobs[start - 1].bucket ? jobs[start].bucket : jobs[start - 1].bucket + 1;
        range->last_callback = end == job_count ? bucket_count - 1 : jobs[end - 1].bucket;
        slot->ends[built] = total ? (double)sum / total : 1;
        built++;
        start = end;
    }
    return built;
}

static void capture_targets(TargetState *state, ID3D11DeviceContext *context) {
    ID3D11DeviceContext_OMGetRenderTargets(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state->rtvs, &state->dsv);
    state->viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetViewports(context, &state->viewport_count, state->viewports);
    state->scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetScissorRects(context, &state->scissor_count, state->scissors);
}

static void release_targets(TargetState *state) {
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
        if (state->rtvs[i]) ID3D11RenderTargetView_Release(state->rtvs[i]);
    if (state->dsv) ID3D11DepthStencilView_Release(state->dsv);
    memset(state, 0, sizeof *state);
}

// Cold, like a fresh renderer, but keeping the target record: every cached state differs from what the engine binds next.
static void make_cold(BYTE *renderer) {
    memset(renderer + 0xc, 0, RENDERER_TARGETS_START - 0xc);
    memset(renderer + RENDERER_TARGETS_END, 0, RENDERER_SIZE - RENDERER_TARGETS_END);
    *(uint32_t *)(renderer + 0x44) = *(uint32_t *)(renderer + 0x48) = ~0u;
    *(uint32_t *)(renderer + 0x50) = *(uint32_t *)(renderer + 0x54) = ~0u;
    *(uint32_t *)(renderer + 0x5c) = *(uint32_t *)(renderer + 0x60) = ~0u;
}

static void bind_targets(ID3D11DeviceContext *context, const TargetState *state) {
    ID3D11DeviceContext_OMSetRenderTargets(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, (ID3D11RenderTargetView *const *)state->rtvs, state->dsv);
    ID3D11DeviceContext_RSSetViewports(context, state->viewport_count, state->viewports);
    ID3D11DeviceContext_RSSetScissorRects(context, state->scissor_count, state->scissors);
}

static void begin_command_list(Worker *worker, ViewSlot *slot) {
    if (values_active) begin_values(&worker->values);
    if (overhaul_texture_copies) {
        worker->texture_generation = ++worker->texture_generations ? worker->texture_generations : ++worker->texture_generations;
        worker->texture_copy_count = 0;
    }
    memcpy(worker->renderer, slot->renderer_template, RENDERER_SIZE);
    *(void ***)worker->renderer = worker->vtable;
    *(ID3D11DeviceContext **)(worker->renderer + RENDERER_CONTEXT) = worker->context;
    make_cold(worker->renderer);
    worker->uploads.list_number++;
    worker->uploads.use_copies = overhaul_cb_copies;
    for (int i = 0; i < worker->clone_count; i++) select_clone_buffer(&worker->clones[i]);
    bind_targets(worker->context, &slot->targets);
}

static void run_range(Worker *worker, ViewSlot *slot, Range *range) {
    LONGLONG started = now_ticks();
    unsigned short x87 = slot->x87;
    _mm_setcsr(slot->csr);
    __asm__ volatile("fldcw %0" : : "m"(x87));
    begin_command_list(worker, slot);
    range->result = original_execute(range->context, worker->renderer, slot->view);
    worker->texture_generation = 0;
    worker->values.drawing = FALSE;
    range->finish_result = ID3D11DeviceContext_FinishCommandList(worker->context, FALSE, &range->list);
    worker->range_ticks = now_ticks() - started;
    worker->busy_ticks += worker->range_ticks;
}

static DWORD WINAPI worker_main(LPVOID parameter) {
    Worker *worker = parameter;
    // Main may kick this worker's first range before the thread runs, so it starts from the generation main saw.
    LONG seen = worker->started_at_generation;
    for (;;) {
        for (int spin = 0; generation == seen && spin < WORKER_SPIN_ROUNDS; spin++) _mm_pause();
        while (generation == seen) WaitOnAddress(&generation, &seen, sizeof seen, INFINITE);
        seen = generation;
        TaskFn task = worker->task;
        if (task) {
            unsigned short x87 = worker->task_x87;
            _mm_setcsr(worker->task_csr);
            __asm__ volatile("fldcw %0" : : "m"(x87));
            worker->in_task = 1;
            task(worker->task_argument);
            worker->in_task = 0;
            worker->task = NULL;
            continue;
        }
        ViewSlot *slot = worker->slot;
        Range *range = worker->range;
        if (!slot) continue;
        worker->slot = NULL;
        run_range(worker, slot, range);
        InterlockedDecrement(&slot->remaining);
    }
    return 0;
}

static BOOL start_workers(int wanted, ID3D11DeviceContext *immediate) {
    if (worker_error) return FALSE;
    if (!device) {
        ID3D11DeviceContext_GetDevice(immediate, &device);
        if (ID3D11Device_GetCreationFlags(device) & D3D11_CREATE_DEVICE_SINGLETHREADED) {
            worker_error = "the device is single-threaded; set multithreaded_device=1 in s2prof.ini";
            api->log_line("overhaul: no workers: %s", worker_error);
            return FALSE;
        }
    }
    while (worker_count < wanted) {
        Worker *worker = _aligned_malloc(sizeof *worker, 64);
        if (!worker) {
            worker_error = "out of memory for a worker";
            api->log_line("overhaul: no more workers: %s", worker_error);
            return FALSE;
        }
        memset(worker, 0, sizeof *worker);
        worker->number = worker_count + 1;
        worker->started_at_generation = generation;
        memcpy(worker->vtable, engine + RENDERER_VTABLE_RVA, sizeof worker->vtable);
        for (int stage = 0; stage < STAGES; stage++) worker->vtable[set_shader_slots[stage] / 4] = set_shader_wrappers[stage];
        worker->program_capacity = PROGRAM_TABLE_INITIAL;
        worker->programs = calloc(worker->program_capacity, sizeof *worker->programs);
        HRESULT result = ID3D11Device_CreateDeferredContext(device, 0, &worker->context);
        if (!worker->programs || FAILED(result)) {
            static char error[160];
            snprintf(error, sizeof error, "CreateDeferredContext failed: 0x%08lx", (unsigned long)result);
            worker_error = worker->programs ? error : "out of memory for a worker's programs";
            api->log_line("overhaul: no more workers: %s", worker_error);
            _aligned_free(worker);
            return FALSE;
        }
        HANDLE thread = CreateThread(NULL, 0, worker_main, worker, CREATE_SUSPENDED, &worker->thread_id);
        if (!thread) {
            worker_error = "CreateThread failed";
            api->log_line("overhaul: no more workers: %s", worker_error);
            return FALSE;
        }
        // Published before the thread runs, so its own lookups find it; the count after the entry, for other threads.
        workers[worker_count] = worker;
        InterlockedIncrement(&worker_count);
        ResumeThread(thread);
        CloseHandle(thread);
    }
    return TRUE;
}

static void wake_workers(void) {
    InterlockedIncrement(&generation);
    WakeByAddressAll((void *)&generation);
}

// A task runs with the kicking thread's rounding, as the engine would have run it there.
static void kick_task(Worker *worker, TaskFn task, void *argument) {
    worker->task_csr = _mm_getcsr();
    __asm__ volatile("fnstcw %0" : "=m"(worker->task_x87));
    worker->task_argument = argument;
    worker->task = task;
    wake_workers();
}

static void wait_task(Worker *worker) {
    while (worker->task) _mm_pause();
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static BYTE execute_lists(ViewSlot *slot, ID3D11DeviceContext *immediate) {
    BYTE result = 1;
    for (int k = 1; k < slot->range_count; k++) {
        Range *range = &slot->ranges[k];
        if (FAILED(range->finish_result)) {
            api->log_line("overhaul: range %d: FinishCommandList failed: 0x%08lx", k, (unsigned long)range->finish_result);
            __builtin_trap();
        }
        ID3D11DeviceContext_ExecuteCommandList(immediate, range->list, FALSE);
        ID3D11CommandList_Release(range->list);
        range->list = NULL;
        result &= range->result;
    }
    return result;
}

static uint32_t main_view_type = VIEW_TYPES, main_view_cascade;

// The cascade loop sets each cascade's camera before its generic pass, then raises the index after it; the record
// ahead sets both for the next cascade and puts the index back.
static void record_cascade_ahead(BYTE *context, BYTE *view, uint32_t cascade) {
    original_reset_buckets(context);
    BYTE *holder = ((CameraHolderFn)(engine + CAMERA_HOLDER_RVA))(*(BYTE **)(view + 0x38));
    ((SetCameraFn)(engine + SET_CAMERA_RVA))(holder, view + CASCADE_CAMERAS + cascade * CASCADE_CAMERA_SIZE);
    *(uint32_t *)(view + CASCADE_INDEX) = cascade;
    original_record_queues(view);
    *(uint32_t *)(view + CASCADE_INDEX) = cascade - 1;
    ahead.view = view;
    ahead.cascade = cascade;
    ahead.skip_reset = TRUE;
}

static BYTE split_execute(BYTE *context, BYTE *renderer, BYTE *view, int64_t groups) {
    ID3D11DeviceContext *immediate = *(ID3D11DeviceContext **)(renderer + RENDERER_CONTEXT);
    LONGLONG started = now_ticks();
    Timers *timer = &timers[ab_mode()];
    if (groups < overhaul_min_jobs) return original_execute(context, renderer, view);
    ViewSlot *slot = &slot_storage;
    snapshot_jobs(slot, context, view);
    // No ranges split from no groups; the engine's empty buckets draw what an empty record would.
    if (!slot->job_count) return original_execute(context, renderer, view);
    LONGLONG listed = now_ticks();
    timer->snapshot_jobs += listed - started;
    int wanted = overhaul_workers < 0 ? 0 : overhaul_workers > MAX_WORKERS ? MAX_WORKERS : overhaul_workers;
    if (!overhaul_on() || enabled_frames < WARMUP_FRAMES || (wanted && !start_workers(wanted, immediate))) wanted = 0;
    uint32_t bucket_count = *(uint32_t *)(context + 0x10);
    double ends[MAX_WORKERS + 1];
    if (overhaul_autobalance) balanced_ends(current_balance, wanted + 1, overhaul_main_share, ends);
    else share_ends(wanted + 1, overhaul_main_share, ends);
    int range_count = slot->range_count = split_ranges(slot, wanted + 1, bucket_count, ends);
    for (int k = 0; k < range_count; k++) build_range_context(slot, &slot->ranges[k], context, k ? workers[k - 1] : NULL);
    LONGLONG built = now_ticks();
    timer->snapshot_contexts += built - listed;
    BOOL values = range_count > 1 && overhaul_value_copies;
    if (values) {
        wrap_writers(slot);
        contexts_split = overhaul_param_contexts && prepare_contexts();
        begin_values(&main_values);
        values_active = 1;
    }
    LONGLONG valued = now_ticks();
    timer->snapshot_values += valued - built;
    // The next cascade records into the buckets while this one draws, so this one draws from copies of its groups. Its
    // camera is written into the shared library, which the ranges, drawing from their own copies, do not read; its record
    // writes each group-instance mesh's count and pass bit, which the ranges read from groupgather's views.
    uint32_t next_cascade = UINT32_MAX;
    if (range_count > 1 && overhaul_record_cascades && contexts_split && overhaul_group_gather && main_view_type == 1 &&
        method(view, 0x64) == engine + RECORD_QUEUES_RVA && method(view, 0xa0) == engine + CASCADE_LOOP_RVA) {
        uint32_t index = *(uint32_t *)(view + CASCADE_INDEX);
        if (index + 1 < *(uint32_t *)(view + CASCADE_COUNT)) next_cascade = index + 1;
    }
    if (next_cascade != UINT32_MAX) copy_packets(slot);
    if (range_count > 1) {
        split_thread = GetCurrentThreadId();
        capture_targets(&slot->targets, immediate);
        memcpy(slot->renderer_template, renderer, RENDERER_SIZE);
        slot->view = view;
        slot->view_type = main_view_type;
        slot->csr = _mm_getcsr();
        unsigned short x87;
        __asm__ volatile("fnstcw %0" : "=m"(x87));
        slot->x87 = x87;
        slot->remaining = range_count - 1;
        for (int k = 1; k < range_count; k++) {
            workers[k - 1]->range = &slot->ranges[k];
            workers[k - 1]->slot = slot;
        }
        ranges_running = 1;
        wake_workers();
    }
    LONGLONG main_started = now_ticks();
    timer->snapshot_targets += main_started - valued;
    timer->snapshot += main_started - started;
    BYTE result = original_execute(slot->ranges[0].context, renderer, view);
    main_values.drawing = FALSE;
    main_group.mesh_of = NULL;
    LONGLONG main_busy = now_ticks();
    timer->main_range += main_busy - main_started;
    if (next_cascade != UINT32_MAX) {
        record_cascade_ahead(context, view, next_cascade);
        timer->ahead_records++;
        timer->ahead += now_ticks() - main_busy;
        main_busy = now_ticks();
    }
    if (range_count > 1) {
        while (slot->remaining) _mm_pause();
        ranges_running = 0;
        if (values) {
            values_active = 0;
            write_back_values(&main_values);
            for (int k = 1; k < range_count; k++) write_back_values(&workers[k - 1]->values);
            contexts_split = 0;
        }
        LONGLONG joined = now_ticks();
        timer->wait += joined - main_busy;
        LONGLONG longest = main_busy - main_started, shortest = longest;
        current_balance->count = range_count;
        current_balance->ends[0] = slot->ends[0];
        current_balance->ticks[0] = main_busy - main_started;
        for (int k = 1; k < range_count; k++) {
            LONGLONG ticks = workers[k - 1]->range_ticks;
            longest = ticks > longest ? ticks : longest;
            shortest = ticks < shortest ? ticks : shortest;
            current_balance->ends[k] = slot->ends[k];
            current_balance->ticks[k] = ticks;
        }
        timer->spread += longest - shortest;
        result &= execute_lists(slot, immediate);
        for (int k = 1; k < range_count; k++) forget_uploads(&workers[k - 1]->uploads);
        // The lists leave the immediate context in its default state, so the engine's cache must start cold and the
        // targets it set before execute must come back.
        make_cold(renderer);
        bind_targets(immediate, &slot->targets);
        split_thread = 0;
        run_kicks(&main_kicks, renderer);
        for (int k = 1; k < range_count; k++) run_kicks(&workers[k - 1]->kicks, renderer);
        release_targets(&slot->targets);
        timer->replay += now_ticks() - joined;
    }
    timer->views++;
    timer->jobs += slot->job_count;
    timer->ranges += range_count;
    return result;
}

static BYTE __attribute__((thiscall)) execute_hook(BYTE *context, BYTE *renderer, BYTE *view) {
    if (current_worker() || GetCurrentThreadId() != *api->main_thread) return original_execute(context, renderer, view);
    join_early();
    if (!overhaul_on() && !measuring && !ahead.view) return original_execute(context, renderer, view);
    uint32_t type = ((ViewTypeFn)method(view, 0x30))(view);
    uint32_t cascade = type == 1 ? *(uint32_t *)(view + 0x58) : 0;
    uint32_t type_index = type < VIEW_TYPES ? type : VIEW_TYPES - 1, cascade_index = cascade < STATS_CASCADES ? cascade : STATS_CASCADES - 1;
    ViewStats *stats = &view_stats[ab_mode()][type_index][cascade_index];
    LONGLONG started = now_ticks();
    int64_t groups = count_jobs(context);
    BOOL splits = !engine[OPTIMIZE_CYCLED_LOCKS_RVA] && !engine[CUSTOM_LOOP_RVA] && groups >= 0 && type < VIEW_TYPES && (overhaul_views >> type & 1) &&
                  (type != 1 || (cascade < 32 && (overhaul_cascades >> cascade & 1)));
    // Another view drawing between two cascades recorded without record_queues, on top of the record ahead; that frame's
    // draws of it are wrong, and the reset after it starts the next cascade clean.
    if (ahead.view && view != ahead.view) {
        ahead.view = NULL;
        if (!foreign_executes++) api->log_line("overhaul: a view of type 0x%x drew between two shadow cascades; recordcascades is wrong there", type);
    }
    BYTE result;
    if (splits && overhaul_on()) {
        main_view_type = type_index;
        main_view_cascade = cascade_index;
        current_balance = &balances[type_index][cascade_index];
        result = split_execute(context, renderer, view, groups);
        main_view_type = VIEW_TYPES;
        current_balance = NULL;
        stats->split++;
    } else {
        result = original_execute(context, renderer, view);
    }
    stats->calls++;
    if (groups > 0) stats->groups += groups;
    stats->ticks += now_ticks() - started;
    return result;
}

static void append_record(int which, size_t *size, const void *data, size_t bytes) {
    if (*size + bytes > verify_capacity[which]) {
        verify_capacity[which] = 2 * (*size + bytes);
        verify_records[which] = realloc(verify_records[which], verify_capacity[which]);
        if (!verify_records[which]) {
            api->log_line("overhaul: out of memory for %u bytes of recorded buckets", (unsigned)verify_capacity[which]);
            __builtin_trap();
        }
    }
    memcpy(verify_records[which] + *size, data, bytes);
    *size += bytes;
}

// What a view's record left in the buckets, in bucket and record order: counts, callbacks, each recorder's groups.
static size_t serialize_records(int which, BYTE *context) {
    size_t size = 0;
    uint32_t bucket_count = *(uint32_t *)(context + 0x10);
    BYTE *recorder_vectors = *(BYTE **)(context + 0x2c);
    uint32_t *record_counts = *(uint32_t **)(context + 0x50);
    void **callbacks = *(void ***)(context + 0x5c);
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++) {
        if (!record_counts[bucket]) continue;
        BYTE *vector = recorder_vectors + 12 * bucket;
        uint32_t count = *(uint32_t *)(vector + 4);
        BYTE **recorders = *(BYTE ***)(vector + 8);
        append_record(which, &size, &bucket, sizeof bucket);
        append_record(which, &size, &record_counts[bucket], sizeof *record_counts);
        append_record(which, &size, &callbacks[bucket], sizeof *callbacks);
        for (uint32_t i = 0; i < count; i++) {
            BYTE *recorder = recorders[i];
            append_record(which, &size, &recorder, sizeof recorder);
            if (*(void **)recorder == engine + SINGLE_RECORDER_VTABLE_RVA) {
                Job job = {recorder, bucket, JOB_SINGLE, 0, recorder, 0, 0};
                uint32_t instances = job_instances(&job);
                append_record(which, &size, &instances, sizeof instances);
                append_record(which, &size, job_ids(&job), instances * sizeof(uint32_t));
                continue;
            }
            uint32_t used = *(uint32_t *)(recorder + 0x14);
            BYTE **groups = *(BYTE ***)(recorder + 0x10);
            append_record(which, &size, &used, sizeof used);
            for (uint32_t g = 0; g < used; g++) {
                Job job = {groups[g], bucket, JOB_BATCH, 0, recorder, 0, 0};
                uint32_t instances = job_instances(&job), flags = job_flags(&job);
                BYTE *item = job_item(&job);
                append_record(which, &size, &item, sizeof item);
                append_record(which, &size, &flags, sizeof flags);
                append_record(which, &size, &instances, sizeof instances);
                append_record(which, &size, job_ids(&job), instances * sizeof(uint32_t));
            }
        }
    }
    return size;
}

// At the cascade's own turn: its record runs again into emptied buckets, and the two records must match.
static void verify_record_ahead(BYTE *view, Timers *timer) {
    BYTE *context = *(BYTE **)(engine + DRAW_CONTEXT_RVA);
    size_t ahead_size = serialize_records(0, context);
    original_reset_buckets(context);
    original_record_queues(view);
    size_t size = serialize_records(1, context);
    timer->ahead_verified++;
    if (size == ahead_size && !memcmp(verify_records[0], verify_records[1], size)) return;
    if (!timer->ahead_mismatched++)
        api->log_line("overhaul: the record ahead of shadow cascade %u differs from its own record (%u vs %u bytes)", *(uint32_t *)(view + CASCADE_INDEX),
                      (unsigned)ahead_size, (unsigned)size);
}

static void cancel_record_ahead(Timers *timer) {
    timer->ahead_misses++;
    ahead.view = NULL;
    original_reset_buckets(*(BYTE **)(engine + DRAW_CONTEXT_RVA));
}

static void __attribute__((thiscall)) record_queues_hook(BYTE *view) {
    if (ahead.view && GetCurrentThreadId() == *api->main_thread) {
        Timers *timer = &timers[ab_mode()];
        if (ahead.skip_reset || view != ahead.view || *(uint32_t *)(view + CASCADE_INDEX) != ahead.cascade) {
            cancel_record_ahead(timer);
        } else {
            ahead.view = NULL;
            timer->ahead_hits++;
            if (overhaul_record_verify) verify_record_ahead(view, timer);
            return;
        }
    }
    original_record_queues(view);
}

// The reset after the cascade that recorded the next one ahead would empty the next one's buckets; a second reset before
// the next cascade's record means the record ahead is gone.
static void __attribute__((thiscall)) reset_buckets_hook(BYTE *context) {
    if (ahead.view && GetCurrentThreadId() == *api->main_thread) {
        if (ahead.skip_reset) {
            ahead.skip_reset = FALSE;
            return;
        }
        ahead.view = NULL;
        timers[ab_mode()].ahead_misses++;
    }
    original_reset_buckets(context);
}

// With the custom loop off, begin_bucket only stores the view type that the view's record already stored (+0x108). A
// worker skips the store: main may be recording the next cascade, whose walks read the field.
static void __attribute__((thiscall)) begin_bucket_hook(BYTE *callbacks, void *argument, BYTE *view) {
    if (current_worker() && !engine[CUSTOM_LOOP_RVA]) return;
    original_begin_bucket(callbacks, argument, view);
}

// ---- the Variant node's prepare ----

static ID3D11DeviceContext *immediate_context(void) {
    return *(ID3D11DeviceContext **)(*(BYTE **)(engine + RENDER_DEVICE_RVA) + RENDERER_OFFSET + RENDERER_CONTEXT);
}

// The immediate context takes no lock of its own unless asked: a worker maps instance textures on it beside main (in the
// CE, unprotected maps from three threads crashed the driver). Off again means as it was before.
static BOOL protect_immediate_context(BOOL on) {
    if (immediate_lock_failed) return FALSE;
    if (!immediate_lock) {
        HRESULT result = ID3D11DeviceContext_QueryInterface(immediate_context(), &multithread_iid, &immediate_lock);
        if (FAILED(result)) {
            api->log_line("overhaul: the immediate context has no ID3D11Multithread: 0x%08lx; the prepare stays on main", (unsigned long)result);
            immediate_lock_failed = TRUE;
            immediate_lock = NULL;
            return FALSE;
        }
    }
    BOOL was = (*(MultithreadVtable **)immediate_lock)->SetMultithreadProtected(immediate_lock, on || protected_before);
    if (on && !protection_logged) {
        protected_before = was;
        protection_logged = TRUE;
        api->log_line("overhaul: the immediate context's multithread protection was %s before the first prepare on a worker", was ? "on" : "off");
    }
    return TRUE;
}

static BOOL instancing_active(const BYTE *node) {
    return node[NODE_INSTANCING] && !node[NODE_INSTANCING_OFF] && *(const uint32_t *)(node + NODE_TEXTURE_COUNT);
}

static void sort_list_a(BYTE *node) {
    if (node[LIST_A_MERGE]) original_list_merge(node + LIST_A_BODY);
    uint32_t count = *(uint32_t *)(node + LIST_A_COUNT);
    BYTE *items = *(BYTE **)(node + LIST_A_ITEMS);
    ((ItemSortFn)(engine + ITEM_SORT_RVA))(items, items + (size_t)count * RENDER_ITEM_SIZE, count, node + 4);
}

// The prepare as the engine has it (keys of lists A and B, each merged first; the instance textures; list A merged again
// if something was added, then sorted, with the prepare's own interface pointer as the sort's fourth argument), timed by
// part. fill FALSE leaves the textures to a worker.
static void timed_prepare(BYTE *node, BOOL fill, volatile LONGLONG *keys, volatile LONGLONG *filled, volatile LONGLONG *sorted) {
    LONGLONG started = now_ticks();
    ((NodeFn)(engine + COMPUTE_KEYS_RVA))(node);
    LONGLONG keyed = now_ticks();
    if (fill) ((NodeFn)(engine + FILL_TEXTURES_RVA))(node);
    LONGLONG textured = now_ticks();
    sort_list_a(node);
    InterlockedAdd64(keys, keyed - started);
    InterlockedAdd64(filled, textured - keyed);
    InterlockedAdd64(sorted, now_ticks() - textured);
}

// The fill reads only the pose list and the textures' sizes, which the keys and the sort also only read; the pose list's
// merge, which either may run first, takes the list's own lock.
static void fill_task(void *node) {
    LONGLONG started = now_ticks();
    ((NodeFn)(engine + FILL_TEXTURES_RVA))(node);
    InterlockedAdd64(&prepare_stats[prepare_mode()].worker_fill, now_ticks() - started);
}

static void early_node_task(void *argument) {
    EarlyNode *slot = argument;
    BYTE *node = slot->node;
    PrepareStats *stats = &prepare_stats[prepare_mode()];
    timed_prepare(node, TRUE, &stats->worker_keys, &stats->worker_fill, &stats->worker_sort);
    slot->poses = *(uint32_t *)(node + POSE_COUNT);
    slot->list_a = *(uint32_t *)(node + LIST_A_COUNT);
    slot->list_b = *(uint32_t *)(node + LIST_B_COUNT);
}

static void wait_early_tasks(void) {
    for (int i = 0; i < early.count; i++) wait_task(workers[i]);
}

static void join_early(void) {
    if (!early.pending) return;
    LONGLONG started = now_ticks();
    wait_early_tasks();
    protect_immediate_context(FALSE);
    early.pending = 0;
    LONGLONG waited = now_ticks() - started;
    prepare_stats[prepare_mode()].join += waited;
    prepare_stats[prepare_mode()].ticks += waited;
}

// HOLDS if nothing was added since the early prepare: no list grew or needs a merge, no pose was added; otherwise the
// first reason it does not. Dirty rows are no reason: the fill's one-texture branch (0x114e7966) returns without clearing
// them, so they stay set, and the fill is redone on main anyway.
static int early_prepare_holds(const EarlyNode *slot) {
    BYTE *node = slot->node;
    if (early.disturbed) return REDO_DISTURBED;
    if (early.sequence != update_sequence) return REDO_SEQUENCE;
    if (!instancing_active(node)) return REDO_INSTANCING;
    if (node[POSE_DIRTY]) return REDO_POSES_DIRTY;
    if (node[LIST_A_MERGE]) return REDO_A_MERGE;
    if (node[LIST_B_MERGE]) return REDO_B_MERGE;
    if (slot->poses != *(uint32_t *)(node + POSE_COUNT)) return REDO_POSES;
    if (slot->list_a != *(uint32_t *)(node + LIST_A_COUNT)) return REDO_A_COUNT;
    if (slot->list_b != *(uint32_t *)(node + LIST_B_COUNT)) return REDO_B_COUNT;
    return HOLDS;
}

static BOOL running_task(void) {
    DWORD self = __readfsdword(0x24);
    for (int i = 0; i < worker_count; i++)
        if (workers[i]->thread_id == self) return workers[i]->in_task != 0;
    return FALSE;
}

// A merge of one of the nodes' lists while the early prepare runs would move what it reads; it waits for the early
// prepare, which then no longer holds.
static BOOL early_list(const BYTE *list) {
    for (int i = 0; i < early.count; i++) {
        const BYTE *node = early.nodes[i].node;
        if (list == node + POSE_LIST || list == node + LIST_A_BODY || list == node + LIST_B_BODY) return TRUE;
    }
    return FALSE;
}

static void __attribute__((thiscall)) list_merge_hook(BYTE *list) {
    if (early.pending && early_list(list) && !running_task()) {
        wait_early_tasks();
        early.disturbed = 1;
    }
    original_list_merge(list);
}

static void __attribute__((thiscall)) node_flush_hook(BYTE *list) {
    if (early.pending && early_list(list) && !running_task()) {
        wait_early_tasks();
        early.disturbed = 1;
    }
    original_node_flush(list);
}

// BattleView::update ends with the frame's render lists and poses complete; the UI update runs next, then the nodes'
// prepares. The nodes are those prepared after the previous update, one worker each.
static void __attribute__((thiscall)) battle_view_update_hook(BYTE *battle_view, void *argument) {
    if (GetCurrentThreadId() != *api->main_thread) {
        original_battle_view_update(battle_view, argument);
        return;
    }
    join_early();
    prepare_stats[prepare_mode()].updates++;
    original_battle_view_update(battle_view, argument);
    BOOL continuing = frame_node_count && prepared_after == update_sequence;
    update_sequence++;
    if (!overhaul_on() || !overhaul_early_prepare || enabled_frames < WARMUP_FRAMES || !continuing) return;
    int count = 0;
    for (int i = 0; i < frame_node_count; i++) {
        BYTE *node = frame_nodes[i];
        if (*(BYTE **)(node + 4) == engine + PREPARE_VTABLE_RVA && instancing_active(node)) early.nodes[count++] = (EarlyNode){node, 0, 0, 0};
    }
    early.count = 0;
    if (!count || (worker_count < count && !start_workers(count, immediate_context())) || !protect_immediate_context(TRUE)) return;
    early.count = count;
    early.sequence = update_sequence;
    early.disturbed = 0;
    early.pending = 1;
    prepare_stats[prepare_mode()].early_kicked += count;
    for (int i = 0; i < count; i++) kick_task(workers[i], early_node_task, &early.nodes[i]);
}

static EarlyNode *early_slot(const BYTE *node) {
    for (int i = 0; i < early.count; i++)
        if (early.nodes[i].node == node) return &early.nodes[i];
    return NULL;
}

static void __attribute__((thiscall)) prepare_hook(BYTE *prepare_interface, BYTE *renderer) {
    if (GetCurrentThreadId() != *api->main_thread) {
        original_prepare(prepare_interface, renderer);
        return;
    }
    BYTE *node = prepare_interface - 4;
    PrepareStats *stats = &prepare_stats[prepare_mode()];
    LONGLONG started = now_ticks();
    if (*frame_stamp != prepare_stamp) {
        prepare_stamp = *frame_stamp;
        stats->frames++;
        frame_node_count = 0;
        prepared_after = update_sequence;
    }
    join_early();
    stats->calls++;
    stats->items += *(uint32_t *)(node + LIST_A_COUNT);
    BOOL repeat = FALSE;
    for (int i = 0; i < frame_node_count; i++) repeat |= frame_nodes[i] == node;
    if (repeat) {
        stats->repeats++;
    } else {
        stats->nodes++;
        if (frame_node_count < EARLY_NODES) frame_nodes[frame_node_count++] = node;
    }
    EarlyNode *slot = early_slot(node);
    if (slot) {
        int reason = early_prepare_holds(slot);
        slot->node = NULL;
        if (reason == HOLDS) {
            // The engine refills on every prepare while the rows stay dirty, so pose data written since the kick lands.
            if (node[NODE_ROWS_DIRTY]) ((NodeFn)(engine + FILL_TEXTURES_RVA))(node);
            stats->early_used++;
            stats->ticks += now_ticks() - started;
            return;
        }
        stats->early_fallbacks++;
        stats->redone[reason]++;
        node[NODE_ROWS_DIRTY] = 1;
    }
    if (!overhaul_on() || !instancing_active(node)) {
        original_prepare(prepare_interface, renderer);
    } else if (overhaul_prepare_split && enabled_frames >= WARMUP_FRAMES && node[NODE_ROWS_DIRTY] &&
               (worker_count >= 1 || start_workers(1, immediate_context())) && protect_immediate_context(TRUE)) {
        kick_task(workers[0], fill_task, node);
        timed_prepare(node, FALSE, &stats->main_keys, &stats->main_fill, &stats->main_sort);
        LONGLONG waiting = now_ticks();
        wait_task(workers[0]);
        protect_immediate_context(FALSE);
        stats->join += now_ticks() - waiting;
        stats->split++;
    } else {
        timed_prepare(node, TRUE, &stats->main_keys, &stats->main_fill, &stats->main_sort);
    }
    stats->ticks += now_ticks() - started;
}

// Before any view renders, the early prepare is joined even if the node's prepare does not come this frame.
static void __attribute__((thiscall)) prepare_queues_hook(BYTE *scene, BYTE *renderer) {
    if (GetCurrentThreadId() == *api->main_thread) join_early();
    original_prepare_queues(scene, renderer);
}

// A worker allocates from its ring clones; one past a clone's capacity grows it first.
static BYTE *__attribute__((thiscall)) ring_allocate_hook(BYTE *holder, BYTE *span, BYTE *renderer, uint32_t bytes, uint32_t unused, uint32_t *offset,
                                                         uint32_t stride) {
    Worker *worker = current_worker();
    if (worker && bytes > *(uint32_t *)(holder + 0x14)) grow_clone(worker, holder, bytes);
    return original_ring_allocate(holder, span, renderer, bytes, unused, offset, stride);
}

// ---- reports ----

static void reset_stats(void) {
    memset(work, 0, sizeof work);
    memset(timers, 0, sizeof timers);
    memset(prepare_stats, 0, sizeof prepare_stats);
    memset(view_stats, 0, sizeof view_stats);
    for (int i = 0; i < worker_count; i++) {
        memset(workers[i]->work, 0, sizeof workers[i]->work);
        workers[i]->busy_ticks = 0;
    }
    escaped_touches = 0;
}

static void log_view_stats(void) {
    for (int mode = 0; mode < 2; mode++) {
        double frames = work[mode].frames ? (double)work[mode].frames : 1;
        for (int type = 0; type < VIEW_TYPES; type++)
            for (int cascade = 0; cascade < STATS_CASCADES; cascade++) {
                const ViewStats *stats = &view_stats[mode][type][cascade];
                if (!stats->calls) continue;
                char name[32];
                snprintf(name, sizeof name, type == 1 ? "%d cascade %d" : "%d", type, cascade);
                api->log_line("  overhaul %s: view type %s: %.2f calls, %.0f groups, %.3f ms a frame in execute (%s)", mode ? "on" : "off", name,
                              stats->calls / frames, stats->groups / frames, ticks_ms(stats->ticks) / frames, stats->split ? "split" : "unsplit");
            }
    }
}

static void overhaul_report(BOOL start) {
    measuring = start;
    if (start) {
        reset_stats();
        return;
    }
    WorkCounts total[2] = {work[0], work[1]};
    for (int i = 0; i < worker_count; i++)
        for (int mode = 0; mode < 2; mode++) {
            total[mode].draws += workers[i]->work[mode].draws;
            total[mode].instances += workers[i]->work[mode].instances;
        }
    for (int mode = 0; mode < 2; mode++) {
        double frames = total[mode].frames ? (double)total[mode].frames : 1;
        const Timers *timer = &timers[mode];
        api->log_line("  overhaul %s: %.1f draws and %.0f instances a frame; %.2f split views a frame (%.0f groups, %.1f ranges each); snapshot %.3f, main range "
                      "%.3f, wait %.3f, replay %.3f ms a frame",
                      mode ? "on" : "off", total[mode].draws / frames, total[mode].instances / frames, timer->views / frames,
                      timer->views ? (double)timer->jobs / timer->views : 0.0, timer->views ? (double)timer->ranges / timer->views : 0.0,
                      ticks_ms(timer->snapshot) / frames, ticks_ms(timer->main_range) / frames, ticks_ms(timer->wait) / frames, ticks_ms(timer->replay) / frames);
    }
    for (int mode = 0; mode < 2; mode++) {
        double frames = total[mode].frames ? (double)total[mode].frames : 1;
        const Timers *timer = &timers[mode];
        if (timer->views)
            api->log_line("  overhaul %s: snapshot parts %.3f groups, %.3f contexts, %.3f values, %.3f targets ms a frame", mode ? "on" : "off",
                          ticks_ms(timer->snapshot_jobs) / frames, ticks_ms(timer->snapshot_contexts) / frames, ticks_ms(timer->snapshot_values) / frames,
                          ticks_ms(timer->snapshot_targets) / frames);
        if (timer->ahead_records || timer->ahead_misses)
            api->log_line("  overhaul %s: %.2f cascades recorded ahead a frame (%.3f ms), %llu hits, %llu misses, %llu verified, %llu differed", mode ? "on" : "off",
                          timer->ahead_records / frames, ticks_ms(timer->ahead) / frames, timer->ahead_hits, timer->ahead_misses, timer->ahead_verified,
                          timer->ahead_mismatched);
    }
    for (int mode = 0; mode < PREPARE_MODES; mode++) {
        const PrepareStats *prepare = &prepare_stats[mode];
        double frames = prepare->frames ? (double)prepare->frames : 1;
        if (!prepare->calls) continue;
        api->log_line("  overhaul %s, prepare %s, earlyprepare %s: Variant prepare %.2f a frame on %.2f nodes (%.0f list A items), %.3f ms a frame on "
                      "main (%.3f waiting); main's parts: keys %.3f, fill %.3f, sort %.3f; workers': keys %.3f, fill %.3f, sort %.3f ms; %llu split; "
                      "%.2f updates a frame; early %llu kicked, %llu used, %llu redone",
                      mode & 1 ? "on" : "off", mode & 2 ? "on" : "off", mode & 4 ? "on" : "off", prepare->calls / frames, prepare->nodes / frames,
                      (double)prepare->items / prepare->calls, ticks_ms(prepare->ticks) / frames, ticks_ms(prepare->join) / frames,
                      ticks_ms(prepare->main_keys) / frames, ticks_ms(prepare->main_fill) / frames, ticks_ms(prepare->main_sort) / frames,
                      ticks_ms(prepare->worker_keys) / frames, ticks_ms(prepare->worker_fill) / frames, ticks_ms(prepare->worker_sort) / frames,
                      prepare->split, prepare->updates / frames, prepare->early_kicked, prepare->early_used, prepare->early_fallbacks);
        if (prepare->early_fallbacks)
            api->log_line("  overhaul: early prepares redone: %llu after a merge waited, %llu after another update, %llu instancing off, %llu poses "
                          "dirty, %llu list A or %llu list B needing a merge, %llu poses added, %llu list A or %llu list B grown",
                          prepare->redone[REDO_DISTURBED], prepare->redone[REDO_SEQUENCE], prepare->redone[REDO_INSTANCING],
                          prepare->redone[REDO_POSES_DIRTY], prepare->redone[REDO_A_MERGE], prepare->redone[REDO_B_MERGE],
                          prepare->redone[REDO_POSES], prepare->redone[REDO_A_COUNT], prepare->redone[REDO_B_COUNT]);
    }
    if (total[0].frames && total[1].frames) {
        double draws_off = total[0].draws / (double)total[0].frames, draws_on = total[1].draws / (double)total[1].frames;
        double instances_off = total[0].instances / (double)total[0].frames, instances_on = total[1].instances / (double)total[1].frames;
        BOOL parity = draws_off > 0 && instances_off > 0 && fabs(draws_on / draws_off - 1) <= PARITY_TOLERANCE &&
                      fabs(instances_on / instances_off - 1) <= PARITY_TOLERANCE;
        api->log_line("  overhaul work parity: %s (draws %+.1f%%, instances %+.1f%%)", parity ? "OK" : "DIFFERS", draws_off ? 100 * (draws_on / draws_off - 1) : 0.0,
                      instances_off ? 100 * (instances_on / instances_off - 1) : 0.0);
    }
    uint64_t texture_sets = 0, shared_holders = 0, grows = 0, leaks = 0, unwrapped = 0, context_calls = 0, restores = 0;
    for (int i = 0; i < worker_count; i++) {
        texture_sets += workers[i]->texture_sets;
        shared_holders += workers[i]->shared_holders;
        leaks += workers[i]->values.leaks;
        unwrapped += workers[i]->values.unwrapped_touches;
        context_calls += workers[i]->values.context_calls;
        restores += workers[i]->values.restores;
        for (int c = 0; c < workers[i]->clone_count; c++) grows += workers[i]->clones[c].grows;
    }
    leaks += main_values.leaks;
    unwrapped += main_values.unwrapped_touches;
    api->log_line("  overhaul state: %d workers%s%s; %llu texture sets in copies, %llu ring clones grown, %llu draws on shared rings; writers: %llu in "
                  "library copies, %llu restores, %llu leaks, %llu unwrapped touches, %ld escaped touches",
                  worker_count, worker_error ? ": " : "", worker_error ? worker_error : "", texture_sets, grows, shared_holders, context_calls, restores, leaks,
                  unwrapped, escaped_touches);
    for (LONG i = 0; i < unwrapped_caller_count && i < MAX_CALLERS; i++)
        api->log_line("  overhaul: unwrapped touch from %p (engine +0x%lx)", unwrapped_callers[i], (unsigned long)((BYTE *)unwrapped_callers[i] - engine));
    for (LONG i = 0; i < leak_caller_count && i < MAX_CALLERS; i++)
        api->log_line("  overhaul: library leak from %p (engine +0x%lx)", leak_callers[i], (unsigned long)((BYTE *)leak_callers[i] - engine));
    log_view_stats();
}

// ---- install ----

// A vtable slot holds a relocated pointer; patch compares it with the file's bytes, at the preferred base.
static const char *patch_slot(void **slot, void *replacement, void **original) {
    BYTE file_bytes[4], wanted[4];
    uint32_t file_value = (uint32_t)(uintptr_t)*slot - (uint32_t)(uintptr_t)engine + 0x10000000u;
    memcpy(file_bytes, &file_value, 4);
    memcpy(wanted, &replacement, 4);
    if (original) *original = *slot;
    return api->patch((BYTE *)slot, file_bytes, wanted, 4);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *s2) {
    api = s2;
    engine = api->engine;
    if (!api->main_thread) return "the host is older than this module (no main_thread)";
    frame_stamp = (volatile uint64_t *)(engine + FRAME_STAMP_RVA);
    sort_bucket = (SortBucketFn)(engine + SORT_BUCKET_RVA);
    stream_in = (StreamInFn)(engine + STREAM_IN_RVA);
    contexts.slot_marks = calloc(LIBRARY_SIZE / 4, 1);
    if (!contexts.slot_marks) return "out of memory";
    void **draw_slots = (void **)(engine + RENDERER_VTABLE_RVA) + FIRST_DRAW_SLOT, **get_renderer_slot = (void **)(engine + GET_RENDERER_SLOT_RVA);
    for (int i = 0; i < DRAW_SLOTS; i++)
        if (draw_slots[i] != engine + expected_draw_rvas[i]) return "RENDERER draw slots differ from the analysed build";
    if (*get_renderer_slot != engine + GET_RENDERER_RVA) return "the device's get_renderer slot differs from the analysed build";
    if (*(BYTE **)(engine + INSTANCE_RING_VTABLE_RVA + RING_ALLOCATE_SLOT) != engine + RING_ALLOCATE_RVA ||
        *(BYTE **)(engine + GEOMETRY_RING_VTABLE_RVA + RING_ALLOCATE_SLOT) != engine + RING_ALLOCATE_RVA)
        return "the rings' allocate slot differs from the analysed build";
    // The texture's stream-in kick is one call site; it is retargeted, since its 5 bytes end inside a rel32 jmp.
    BYTE *kick_call = engine + KICK_CALL_RVA, expected_call[5] = {0xe8}, wanted_call[5] = {0xe8};
    LONG expected_offset = (LONG)((engine + KICK_STREAM_RVA) - (kick_call + 5)), wanted_offset = (LONG)((BYTE *)(void *)kick_stream_hook - (kick_call + 5));
    memcpy(expected_call + 1, &expected_offset, 4);
    memcpy(wanted_call + 1, &wanted_offset, 4);
    if (memcmp(kick_call, expected_call, 5)) return "the texture's stream-in kick differs from the analysed build";
    for (int stage = 0; stage < STAGES; stage++) original_set_shaders[stage] = (SetShaderFn)((void **)(engine + RENDERER_VTABLE_RVA))[set_shader_slots[stage] / 4];
    const char *error = api->hook(engine + EXECUTE_RVA, execute_prologue, sizeof execute_prologue, (const void *)execute_hook, (void **)&original_execute);
    if (!error)
        error = api->hook(engine + UPLOAD_CONSTANT_BUFFERS_RVA, upload_constant_buffers_prologue, sizeof upload_constant_buffers_prologue,
                          (const void *)upload_constant_buffers_hook, (void **)&original_upload_constant_buffers);
    if (!error)
        error = api->hook(engine + RING_ALLOCATE_RVA, ring_allocate_prologue, sizeof ring_allocate_prologue, (const void *)ring_allocate_hook,
                          (void **)&original_ring_allocate);
    if (!error)
        error = api->hook(engine + TEXTURE_SET_RVA, texture_set_prologue, sizeof texture_set_prologue, (const void *)texture_set_hook,
                          (void **)&original_texture_set);
    if (!error)
        error = api->hook(engine + BUILD_SRV_LIST_RVA, build_srv_list_prologue, sizeof build_srv_list_prologue, (const void *)build_srv_list_hook,
                          (void **)&original_build_srv_list);
    if (!error) error = api->hook(engine + TOUCH_RVA, touch_prologue, sizeof touch_prologue, (const void *)touch_hook, (void **)&original_touch);
    if (!error)
        error = api->hook(engine + LIBRARY_GETTER_RVA, library_getter_prologue, sizeof library_getter_prologue, (const void *)library_getter_hook,
                          (void **)&original_library_getter);
    if (!error)
        error = api->hook(engine + NODE_TEXTURE_RVA, node_texture_prologue, sizeof node_texture_prologue, (const void *)node_texture_hook,
                          (void **)&original_node_texture);
    if (!error)
        error = api->hook(engine + GROUP_GATHER_RVA, group_gather_prologue, sizeof group_gather_prologue, (const void *)group_gather_hook,
                          (void **)&original_group_gather);
    if (!error)
        error = api->hook(engine + PREPARE_QUEUES_RVA, prepare_queues_prologue, sizeof prepare_queues_prologue, (const void *)prepare_queues_hook,
                          (void **)&original_prepare_queues);
    if (!error) error = api->hook(engine + PREPARE_RVA, prepare_prologue, sizeof prepare_prologue, (const void *)prepare_hook, (void **)&original_prepare);
    if (!error)
        error = api->hook(engine + LIST_MERGE_RVA, list_merge_prologue, sizeof list_merge_prologue, (const void *)list_merge_hook,
                          (void **)&original_list_merge);
    if (!error)
        error = api->hook(engine + NODE_FLUSH_RVA, list_merge_prologue, sizeof list_merge_prologue, (const void *)node_flush_hook,
                          (void **)&original_node_flush);
    if (!error)
        error = api->hook(engine + BATTLE_VIEW_UPDATE_RVA, battle_view_update_prologue, sizeof battle_view_update_prologue,
                          (const void *)battle_view_update_hook, (void **)&original_battle_view_update);
    if (!error)
        error = api->hook(engine + RECORD_QUEUES_RVA, record_queues_prologue, sizeof record_queues_prologue, (const void *)record_queues_hook,
                          (void **)&original_record_queues);
    if (!error)
        error = api->hook(engine + RESET_BUCKETS_RVA, reset_buckets_prologue, sizeof reset_buckets_prologue, (const void *)reset_buckets_hook,
                          (void **)&original_reset_buckets);
    if (!error)
        error = api->hook(engine + BEGIN_BUCKET_RVA, begin_bucket_prologue, sizeof begin_bucket_prologue, (const void *)begin_bucket_hook,
                          (void **)&original_begin_bucket);
    if (!error) error = api->patch(kick_call, expected_call, wanted_call, 5);
    void *const draw_hooks[DRAW_SLOTS] = {(void *)draw_hook, (void *)draw_indexed_hook, (void *)draw_indexed_instanced_hook, (void *)draw_instanced_hook};
    for (int i = 0; i < DRAW_SLOTS && !error; i++) error = patch_slot(&draw_slots[i], draw_hooks[i], &original_draws[i]);
    if (!error) error = patch_slot(get_renderer_slot, (void *)get_renderer_hook, NULL);
    if (error) return error;
    api->register_switch("overhaul", &overhaul_enabled, overhaul_report);
    api->register_switch("autobalance", &overhaul_autobalance, overhaul_report);
    api->register_switch("texturecopies", &overhaul_texture_copies, overhaul_report);
    api->register_switch("valuecopies", &overhaul_value_copies, overhaul_report);
    api->register_switch("paramcontexts", &overhaul_param_contexts, overhaul_report);
    api->register_switch("cbcopies", &overhaul_cb_copies, overhaul_report);
    api->register_switch("groupgather", &overhaul_group_gather, overhaul_report);
    api->register_switch("prepare", &overhaul_prepare_split, overhaul_report);
    api->register_switch("earlyprepare", &overhaul_early_prepare, overhaul_report);
    api->register_switch("recordcascades", &overhaul_record_cascades, overhaul_report);
    api->register_switch("recordverify", &overhaul_record_verify, overhaul_report);
    return NULL;
}
