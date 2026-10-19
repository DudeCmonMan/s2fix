#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#include "../s2module.h"
#include <initguid.h>
#include <d3d11_4.h>
#include <intrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

// The render overhaul: a view's Draw::execute_pass split into contiguous ranges of its recorded groups. Main draws the
// first range on the immediate context; each worker runs the engine's own execute loop over its range into a deferred
// context, and main replays the command lists in range order, so the GPU sees the original draw order. Each range is
// fed through a private DRAW_CONTEXT whose recorders hand out only that range's groups; a worker gets private copies of
// each DRAW_ITEM's chunker and streams, and its own clones of the rings they allocate from.
// With overlap, main does not wait: it records on into its own deferred context while workers draw, and the lists are
// executed in their original order.
// It also counts the draws and instances each mode issues, because a version that drops draws would look faster.
#define EXECUTE_RVA 0x19f2940
#define BIND_PASS_STATE_RVA 0x1ac58d0
#define UPLOAD_CONSTANT_BUFFERS_RVA 0x1abecc0
#define ALWAYS_UPDATE_CB_RVA 0x345cd28
#define CB_TABLE_SIZE 1024
#define GPU_TIMING_FRAMES 8
#define MAX_TIMED_VIEWS 12
#define SORT_BUCKET_RVA 0x19c1fb0
#define KICK_STREAM_RVA 0x19de350
#define STREAM_IN_RVA 0x19de360
#define RENDERER_VTABLE_RVA 0x23dda20
// Copied whole so a worker's renderer can carry its own vtable; the RENDERER vtable has 57 slots.
#define RENDERER_VTABLE_SLOTS 64
#define STAGES 6
#define PROGRAM_SIZE 0xb0
#define PROGRAM_TABLE_INITIAL 4096
#define GET_RENDERER_SLOT_RVA 0x23de1b8
#define GET_RENDERER_RVA 0x1adadd0
#define SINGLE_RECORDER_VTABLE_RVA 0x23ad958
#define BATCH_RECORDER_VTABLE_RVA 0x23ad9a8
#define INSTANCE_RING_VTABLE_RVA 0x23ad770
#define GEOMETRY_RING_VTABLE_RVA 0x23ad6d8
#define RENDER_DEVICE_RVA 0x3450230
#define ENGINE_RVA 0x3457888
#define OPTIMIZE_CYCLED_LOCKS_RVA 0x3450f48
#define FRAME_STAMP_RVA 0x2ead830
#define SCENE_UPDATE_STAMP_OFFSET 0x69de0
#define RENDERER_OFFSET 0x1f0
#define RENDERER_SIZE 0x28b8
#define RENDERER_TARGETS_START 0x22a4
// The target record and the saved targets and topology (save/restore pairs can span an execute) stay; the stencil ref
// after them is state.
#define RENDERER_TARGETS_END 0x23c4
#define DRAW_ITEM_SIZE 0x48
#define CHUNKER_VTABLE_RVA 0x23aa9b8
#define CHUNKER_SIZE 0x28
// The rigid group-instance meshes' chunkers derive from INSTANCE_CHUNKER and add their mesh (+0x28) and a count.
#define DERIVED_CHUNKER_SIZE 0x38
#define STREAMS_SIZE 0x30
#define INDEX_STREAM_SIZE 0x18
#define STREAM_ENTRY_SIZE 24
#define RING_HOLDER_SIZE 0x88
#define VERTEX_BUFFER_SIZE 0x40
#define VERTEX_BUFFER_VTABLE_RVA 0x23ddbe8
#define VERTEX_BUFFER_VTABLE_SLOTS 16
// Updating a video-memory buffer the GPU is still drawing from makes the copy wait; three in turn avoid that.
#define DEVICE_LOCAL_COPIES 3
// Every list's first map of a clone renames its whole buffer in the driver: full-size clones (5 MB instance, 3.85 MB
// geometry) on every list put the driver over a budget at which main's own constant buffer maps wait.
#define RING_CLONE_BYTES (256 * 1024)
#define CONTEXT_SIZE 0x90
#define MAX_WORKERS 8
#define MAX_STREAMS 16
#define MAX_CLONES 16
#define MAX_KICKS 256
#define FIRST_DRAW_SLOT (0x128 / 8)
#define DRAW_SLOTS 5
#define RECORDER_SLOTS 10
#define WARMUP_FRAMES 60
#define WORKER_SPIN_ROUNDS 20000
#define PARITY_TOLERANCE 0.02
// Every change flag a Variant writer acts on except 0x01 (bone palette, instancing off): a range's first group writes
// all of its own parameters instead of relying on the previous range's groups.
#define FORCED_FLAGS 0x1b6
#define TLS_SLOTS_OFFSET 0x1480
#define PREPARE_RVA 0x1b20520
#define COMPUTE_KEYS_RVA 0x1b4e6e0
#define LIST_C_KEYS_RETURN_RVA 0x1b206ad
#define FILL_INSTANCE_TEXTURES_RVA 0x1b4dfb0
#define QUEUE_SORT_RVA 0x1aff120
#define RENDER_ITEM_SIZE 0xb0
#define PREPARE_WORKERS 2
#define CONTEXT_METHODS 115
#define MAX_SLOTS 16
#define MAX_SUBMISSIONS (2 * MAX_SLOTS + 2)
#define RENDER_VIEW_RVA 0x1a79400
#define END_FRAME_RVA 0x1ad53b0
#define RING_ALLOCATE_RVA 0x19eb720
#define RING_ALLOCATE_SLOT 0x80
#define VIEW_TYPES 64
#define STATS_CASCADES 4
#define DRAW_CONTEXT_RVA 0x34578a8
#define RECORD_QUEUES_RVA 0x19d2070
#define RESET_BUCKETS_RVA 0x19d62f0
#define RENDER_VIEWS_RVA 0x19ee720
#define RECORD_EXECUTE_RVA 0x1a628f0
#define BUCKET_CALLBACK_VTABLE_RVA 0x23e4a80
#define BEGIN_BUCKET_RVA 0x1b46330
#define CUSTOM_LOOP_RVA 0x345fe93
#define BATTLE_VIEW_UPDATE_RVA 0x3bfbd0
#define POSE_MERGE_RVA 0x1b4cc60
#define RENDER_LIST_MERGE_RVA 0x1b4d9d0
#define RENDER_SORT_RVA 0x1afee20
#define EARLY_PREPARE_WORKERS 3
#define FILL_HELPERS 2
#define MAX_TEXTURE_FILLS 64
#define FILL_TEXTURE_RVA 0x1b21940
#define TEXTURE_SET_RVA 0x1ab6470
#define BUILD_SRV_LIST_RVA 0x1abee50
#define SHADER_TEXTURE_SIZE 0x40
#define TEXTURE_COPIES 1024
#define TOUCH_RVA 0x1aeed70
#define VALUE_COPIES 512
#define VALUE_ARENA_BYTES (512 * 1024)
#define MAX_TOUCHED 64
#define WRITER_SLOTS 4
#define WRAPPED_CLASSES 1024
#define MAX_UNWRAPPED_CALLERS 16
#define LEARNED_BUFFERS 32
#define BINDING_INFOS 4096
#define LIBRARY_GETTER_RVA 0x19f80d0
#define LIBRARY_OFFSET 0x26c8
// SHADERS, inline in SCENE ([g_engine+0x30]), up to SCENE's next member: its destructor (0x181a477d0) frees +0xa9f78
// after the library's own.
#define LIBRARY_SIZE (0xa9f78 - LIBRARY_OFFSET)
#define LIBRARY_BUFFERS 512
#define MAX_LIBRARY_RECORDS 8192
#define MAX_LEAK_CALLERS 16
#define MAX_LIBRARY_SLOTS 4096
#define EFFECTS_RECORD_RVA 0x1b0e980
// The two Variant weighted effects' vtbl+0xb8 writers read the library through the SCENE pointer the effect keeps
// (+0xc8), at SCENE-relative offsets, and 0x181b47e50 through the Variant node's (+0xd8); they get a copy of the object
// that points as far before the thread's library.
#define VARIANT_CACHED_WRITER_RVA 0x1b4a040
#define VARIANT_B_CACHED_WRITER_RVA 0x1b49eb0
#define CACHED_EFFECT_BYTES 0x108
#define NODE_TEXTURE_RVA 0x1b47e50
#define CACHED_NODE_BYTES 0x2e0
#define EARLY_EFFECTS 2
#define SORT_CHUNK 512
#define PUSH_RECORD_RVA 0x19d2440
#define RECORD_WHOLE_RVA 0x19d2d80
#define VARIANT_RECORD_RVA 0x1b15590
#define RIGID_RECORD_RVA 0x1b14ca0
#define VEGETATION_RECORD_RVA 0x1b6cb00
#define SHADOW_VIEW_SIZE 0x2918
#define ENGINE_ALLOCATE_RVA 0x18abb0
#define POINTER_PUSH_BACK_RVA 0x132d60
#define INSTANCE_GROUP_SIZE 0x410
#define GROUP_GATHER_RVA 0x1b762d0
#define GROUP_INSTANCE_MESH_VTABLE_RVA 0x23ecbc8
// RIGID_GROUP_INSTANCE_MESH up to the last field its gatherer reads: groups +0x1cc/+0x1d0, chunker +0x1e8, pass bit +0x1f4.
#define GROUP_MESH_VIEW_SIZE 0x1f8
#define VARIANT_WEIGHTED_VTABLE_RVA 0x23e3d28
#define VARIANT_WEIGHTED_B_VTABLE_RVA 0x23e3ef8
#define PROGRAM_OUTPUTS 5
#define GROUP_POOLS 3
#define MAX_GROUP_SAMPLES 6144
#define GROUP_TABLE_SIZE 8192
// Frames between sampling a pool's groups and reading them back.
#define GROUP_DELAY 4

typedef BYTE (*ExecuteFn)(BYTE *context, BYTE *renderer, BYTE *view);
typedef uint64_t (*SetShaderFn)(BYTE *renderer, BYTE *program);
typedef uint64_t (*BindPassStateFn)(BYTE *pass, BYTE *renderer);
typedef BYTE *(*UploadConstantBuffersFn)(BYTE *bindings, uint64_t unused, uint32_t *count, uint32_t *first, BYTE **cache);
typedef BYTE (*MapBufferFn)(BYTE *buffer, BYTE *renderer, uint32_t offset, uint32_t bytes, uint32_t type, void **out, uint32_t flags);
typedef void (*UnmapBufferFn)(BYTE *buffer, BYTE *renderer);
typedef void (*SortBucketFn)(BYTE **first, BYTE **last, int64_t ideal, BYTE predicate);
typedef BYTE (*StreamInFn)(BYTE *asset, BYTE *renderer);
typedef uint32_t (*ViewTypeFn)(BYTE *view);
typedef void (*DrawFn)(BYTE *renderer, UINT vertex_count, UINT start_vertex);
typedef void (*DrawIndexedFn)(BYTE *renderer, UINT index_count, UINT start_index, INT base_vertex);
typedef void (*DrawIndexedInstancedFn)(BYTE *renderer, UINT index_count, UINT instances, UINT start_index, INT base_vertex, UINT start_instance);
typedef void (*DrawInstancedFn)(BYTE *renderer, UINT vertex_count, UINT instances, UINT start_vertex, UINT start_instance);
typedef void (*DrawIndirectFn)(BYTE *renderer, BYTE *arguments, UINT offset);
typedef HRESULT(WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
typedef void (*TaskFn)(BYTE *argument);
typedef void (*PrepareFn)(BYTE *prepare_interface, BYTE *renderer);
typedef void (*ComputeKeysFn)(BYTE *node, BYTE *list);
typedef void (*QueueSortFn)(BYTE *first, BYTE *last, int64_t ideal, uint64_t predicate);
typedef void (*RenderViewFn)(BYTE *view, BYTE *renderer, BYTE *options, BYTE *binding, uint64_t face, uint64_t flag8_only, uint64_t pass_arg);
typedef BYTE (*EndFrameFn)(BYTE *render_device);
typedef BYTE *(*RingAllocateFn)(BYTE *holder, BYTE *span, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
typedef void (*RecordQueuesFn)(BYTE *view);
typedef void (*ResetBucketsFn)(BYTE *context);
typedef void (*RenderViewsFn)(BYTE *render_context, BYTE *renderer, BYTE *options, uint64_t face);
typedef uint64_t (*BeginBucketFn)(BYTE *callbacks, BYTE *argument, BYTE *view);
typedef void (*BattleViewUpdateFn)(BYTE *battle_view, BYTE *frame_argument);
typedef void (*ListMergeFn)(BYTE *list_body);
typedef void (*RenderSortFn)(BYTE *first, BYTE *last, int64_t ideal, uint64_t predicate);
typedef void (*FillTextureFn)(BYTE *node, BYTE *texture, uint32_t first_row);
typedef BYTE *(*TextureSetFn)(BYTE *slot, BYTE *texture);
typedef BYTE *(*BuildSrvListFn)(BYTE *list, uint32_t *count, uint32_t *first, BYTE **cache);
typedef void (*TouchFn)(BYTE *parameter);
typedef uint64_t (*WriterFn)(uint64_t effect, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
typedef uint64_t (*PushRecordFn)(BYTE *context, BYTE *effect, uint32_t index, uint32_t flags);
typedef uint64_t (*RecordWholeFn)(BYTE *effect);
typedef void (*QueueRecordFn)(BYTE *queue, BYTE *view);
typedef uint32_t (*PassBitFn)(BYTE *view);
typedef uint64_t (*EffectRecordFn)(BYTE *effect, uint32_t index, uint32_t flags);
typedef BYTE *(*AllocateFn)(size_t bytes);
typedef void (*PointerPushBackFn)(BYTE *vector, BYTE **value);
typedef uint64_t (*GatherFn)(BYTE *gatherer, BYTE *mapped, const uint32_t *ids);
typedef BYTE *(*LibraryGetterFn)(BYTE *holder);
typedef void (*EffectsRecordFn)(BYTE *view);
typedef void (*NodeTextureFn)(BYTE *node, uint64_t instance_texture);

typedef struct {
    uint64_t draws, instances, elements, frames;
} WorkCounts;

typedef struct {
    uint64_t renames, bytes;
} RingRenames;

enum { JOB_BATCH, JOB_SINGLE, JOB_PACKET };

// A batch recorder's INSTANCE_GROUP or a single recorder, decoded when the range reaches it, or a Packet copied from one.
typedef struct {
    BYTE *object;
    uint32_t bucket, kind, weight;
    // The engine recorder the job came from.
    BYTE *recorder;
    // A group-instance job's instance count (mesh+0x1f0) and pass bit (+0x1f4) when its view was snapshotted.
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
} Clone;

// Follows a ring clone's VERTEX_BUFFER copy. In the staging modes its map points into staging memory, and one upload
// per command list moves what was written to the buffer the draws read.
typedef struct {
    void *vtable[VERTEX_BUFFER_VTABLE_SLOTS];
    BYTE *staging;
    uint32_t used, wraps, turn, small_bytes, full_bytes, grows;
    ID3D11Buffer *small, *dynamic, *device_local[DEVICE_LOCAL_COPIES];
} CloneBuffers;

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
// (+0x40, +0x48 bytes) as this thread left it, and the version (binding+0x38) at which the shared shadow last held them.
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
    ID3D11Query *query;
    BYTE *item, *effect_vtable;
    uint32_t view_key, range, position, flags, instances, forced;
} GroupSample;

// One sampled frame's groups (groupstats): a pipeline statistics query around each group's draws.
typedef struct {
    GroupSample *samples;
    volatile LONG used;
    uint64_t frame;
    int mode;
    BOOL pending;
} GroupPool;

// Per draw item, view and mode; [0] counts the samples drawn in main's range, [1] those drawn on a worker.
typedef struct {
    BYTE *item, *effect_vtable;
    uint32_t view_key, mode, flags;
    uint64_t samples[2], forced[2], first[2];
    double pixels[2], vertices[2], primitives[2], instances[2];
} GroupStat;

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

typedef struct {
    ID3D11Query *disjoint, *start[MAX_TIMED_VIEWS], *middle[MAX_TIMED_VIEWS], *end[MAX_TIMED_VIEWS];
    // gpudetail: before each worker list on the immediate context, at its first and last command, after the last list,
    // and pipeline statistics for main's part (the whole view when unsplit) and the lists' part.
    ID3D11Query *list_begin[MAX_TIMED_VIEWS][MAX_WORKERS + 1], *inside_begin[MAX_TIMED_VIEWS][MAX_WORKERS + 1];
    ID3D11Query *inside_end[MAX_TIMED_VIEWS][MAX_WORKERS + 1], *lists_end[MAX_TIMED_VIEWS];
    ID3D11Query *main_statistics[MAX_TIMED_VIEWS], *list_statistics[MAX_TIMED_VIEWS];
    uint8_t type[MAX_TIMED_VIEWS], cascade[MAX_TIMED_VIEWS], detail[MAX_TIMED_VIEWS], lists[MAX_TIMED_VIEWS];
    int views, mode;
    BOOL detailed;
} GpuFrame;

typedef struct {
    uint64_t views, split_views, lists, inside_lists;
    double view_ms, main_ms, gap_ms, lists_ms, after_ms, entry_ms, inside_ms, exit_ms;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS statistics;
} GpuDetail;

typedef struct Worker Worker;

typedef struct {
    void *const *vtable;
    Worker *worker;
    const Job *jobs;
    uint32_t next_job, end_job, forced_job, current_index;
    const Job *current;
    // The bucket's first job in the range, and the range's first job.
    uint32_t first_job, range_first;
    uint32_t range_index, view_key;
    ID3D11DeviceContext *context;
    GroupPool *pool;
    GroupSample *open;
} RangeRecorder;

typedef struct {
    uint32_t reserved, count;
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
    ID3D11CommandList *list, *upload_list;
    HRESULT finish_result;
    BYTE result;
} Range;

// A split view's groups and ranges, and the main-thread state its workers start from; with overlap several are in flight.
typedef struct {
    Job *jobs;
    uint32_t job_count, job_capacity;
    BYTE *packet_arena;
    size_t packet_capacity;
    Range ranges[MAX_WORKERS + 1];
    int range_count;
    double ends[MAX_WORKERS + 1];
    BOOL overlapped;
    BYTE *view;
    uint32_t view_type;
    TargetState targets;
    // The frame and view whose gpudetail queries the lists write, or NULL.
    GpuFrame *timed_frame;
    int timed_view;
    GroupPool *group_pool;
    volatile LONG remaining;
    __attribute__((aligned(16))) BYTE renderer_template[RENDERER_SIZE];
} ViewSlot;

typedef struct {
    ViewSlot *slot;
    Range *range;
} RangeTask;

// In execute order on the immediate context: a split view's command lists, or one of main's segments between them.
typedef struct {
    ViewSlot *slot;
    ID3D11CommandList *list;
} Submission;

// The proxy's context (wrap_context=1): every method forwards to current.
typedef struct {
    void *const *vtable;
    ID3D11DeviceContext *current, *immediate;
} ContextWrapper;

struct Worker {
    __attribute__((aligned(16))) BYTE renderer[RENDERER_SIZE];
    void *vtable[RENDERER_VTABLE_SLOTS];
    ProgramClone *programs;
    uint32_t program_capacity, program_count;
    UploadTracker uploads;
    __attribute__((aligned(16))) BYTE item[DRAW_ITEM_SIZE];
    __attribute__((aligned(16))) BYTE chunker[DERIVED_CHUNKER_SIZE];
    __attribute__((aligned(16))) BYTE streams[STREAMS_SIZE];
    __attribute__((aligned(16))) BYTE index_stream[INDEX_STREAM_SIZE];
    __attribute__((aligned(16))) BYTE stream_entries[MAX_STREAMS * STREAM_ENTRY_SIZE];
    int number;
    LONG started_at_generation;
    ID3D11DeviceContext *context;
    LONG upload_mode;
    Clone clones[MAX_CLONES];
    int clone_count;
    Kicks kicks;
    RangeTask range_tasks[MAX_SLOTS];
    volatile LONG range_tasks_pushed, range_tasks_done;
    TaskFn volatile task;
    BYTE *task_argument;
    LONGLONG task_ticks[2];
    LONGLONG busy_ticks, range_ticks;
    WorkCounts work[2], view_work[2][VIEW_TYPES + 1];
    RingRenames renames[2];
    uint32_t view_type;
    uint64_t shared_holders;
    uint64_t d3d_calls[2][CONTEXT_METHODS];
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
    __attribute__((aligned(16))) BYTE chunker[DERIVED_CHUNKER_SIZE];
    __attribute__((aligned(16))) BYTE view[GROUP_MESH_VIEW_SIZE];
    BYTE *mesh_of;
    const Job *job;
} MainGroup;

// One push_record a parallel record made instead of writing the engine's recorders: queue is the view's queue index, so
// two threads' packets of one view merge back into record order.
typedef struct {
    BYTE *effect;
    uint32_t index, flags, queue;
} RecordPacket;

// What VegetationNode::record would have stored in a group-instance mesh for the view.
typedef struct {
    BYTE *mesh;
    uint32_t count, bit;
} GroupState;

typedef struct {
    RecordPacket *packets;
    uint32_t count, capacity, queue;
    // Also hand the record to the engine (packetverify).
    BOOL forward;
    // A recorder kind build_packet_jobs does not rebuild: the view is replayed into the engine instead.
    BOOL unknown_recorder;
    DWORD thread;
    GroupState *groups;
    uint32_t group_count, group_capacity;
} PacketBuffer;

// A shadow cascade recorded on a worker while main draws cascade 0: a copy of the view whose cascade index (+0x94) is this
// cascade's, so its pass bit (+0x40 + +0x94) and camera are too. Queues not known to be safe off main record at the
// cascade's own turn, on main.
typedef struct {
    __attribute__((aligned(16))) BYTE view[SHADOW_VIEW_SIZE];
    PacketBuffer worker_packets, main_packets;
    BYTE *real_view;
    uint32_t cascade;
    Worker *worker;
    LONGLONG worker_ticks;
    BOOL kicked, verify;
} CascadeRecord;

// The engine's recorders rebuilt from a view's packets (build_packet_jobs).
typedef struct {
    BYTE *recorder, *item;
    uint32_t bucket, batch, capacity, used, index_count, index_start, first_group, last_group, next;
} RecordEntry;

typedef struct {
    BYTE *item;
    uint32_t count, flags, entry, previous, next;
    uint32_t ids[256];
} BuiltGroup;

typedef struct {
    uint64_t views, jobs, ranges, packet_bytes, overlap_views, segments, drains, peak_in_flight;
    LONGLONG snapshot, main_range, wait, replay, spread, kick, submit, drain, cb_upload;
    RingRenames renames;
    double main_fraction;
    uint64_t ahead_records, ahead_hits, ahead_misses, ahead_verified, ahead_mismatched;
    LONGLONG ahead, ahead_wait;
} Timers;

// Where a view's last split ended its ranges (fractions of the view's weight) and how long each took.
typedef struct {
    int count;
    double ends[MAX_WORKERS + 1];
    LONGLONG ticks[MAX_WORKERS + 1];
} Balance;

typedef struct {
    uint64_t frames, early_used, early_fallbacks, texture_fills, sorts, sort_chunks, helped_chunks;
    LONGLONG ticks, join, sort;
} PrepareStats;

typedef struct {
    BYTE *node, *texture;
    uint32_t first_row;
} TextureFill;

// Main-thread time inside execute per view type (and shadow cascade), split or not: what is left to split.
typedef struct {
    uint64_t calls, groups;
    LONGLONG ticks;
} ViewStats;

// On from the start; Ctrl+Shift+F7 toggles it.
__declspec(dllexport) volatile LONG overhaul_enabled = 1;
// Settings read live, so they can be changed while the game runs: workers beside main (0 runs the range path serially
// on main), the view types to split (bit per VIEW_TYPE), and the fewest groups worth splitting. By default the shadow
// cascades, both GBUFFER views, decals and both effects views split, on 3 workers.
__declspec(dllexport) volatile LONG overhaul_workers = 3;
__declspec(dllexport) volatile uint64_t overhaul_views = 1ull << 0x1 | 1ull << 0xe | 1ull << 0xf | 1ull << 0x16 | 1ull << 0x17 | 1ull << 0x19;
__declspec(dllexport) volatile LONG overhaul_min_jobs = 32;
// Bit per shadow cascade index that may split; percent of a worker's share of the groups that main takes (main also
// snapshots and replays, so a smaller share shortens the view).
__declspec(dllexport) volatile LONG overhaul_cascades = 7;
__declspec(dllexport) volatile LONG overhaul_main_share = 60;
// 1 makes main draw its own range only after the workers finish, so main and workers never draw at the same time.
__declspec(dllexport) volatile LONG overhaul_serial_ranges;
// 0: workers bind private program copies; 1: they bind the shared programs and one thread commits at a time.
__declspec(dllexport) volatile LONG overhaul_commit_mode;
// Instance and geometry data a worker writes: 0 maps the ring clone per chunk inside the command list; 1 writes into
// staging memory and maps the clone once per list, in an upload list replayed first; 2 does the same with one
// UpdateSubresource into a buffer in video memory.
__declspec(dllexport) volatile LONG overhaul_instance_upload;
// 1 replays with RestoreContextState TRUE, the runtime saving and restoring the immediate context around every list;
// 0 replays with FALSE, then makes main's renderer cold and rebinds the targets, viewport and scissor.
__declspec(dllexport) volatile LONG overhaul_restore_state;
// 1 copies each split view's groups at the snapshot, so its ranges stop reading the recorders that execute's caller resets
// for the next view: views that outlive their execute need it.
__declspec(dllexport) volatile LONG overhaul_packets;
// 1 runs prepare's instance-texture fill and list C's first sort of the frame on two idle workers, beside main's keys and
// list A and the engine's list B task. The record-time re-sorts of the then sorted list are what tiegroup's sortonce skips.
__declspec(dllexport) volatile LONG overhaul_prepare_split = 1;
// 1 has workers and main's deferred segments upload into their own copies of the engine's constant buffers. With overlap,
// mapping the engine's own buffers on deferred contexts left main's later maps of them waiting in the driver.
__declspec(dllexport) volatile LONG overhaul_cb_copies = 1;
// 1 lets main go on to the next view while workers draw a split one: main records into its own deferred context through
// the proxy's context (wrap_context=1 in s2prof.ini), and the lists are executed in order as they finish and at each drain.
// It takes effect from the next frame, and implies packets.
__declspec(dllexport) volatile LONG overhaul_overlap;
// 1 sets each split view's range boundaries from its previous frame's range times, so main and the workers finish
// together, instead of giving main overhaul_main_share.
__declspec(dllexport) volatile LONG overhaul_autobalance = 1;
// 1 runs the battle's Variant node prepare (instance textures, keys, merges, the three sorts) on workers as soon as the
// scene update has joined its producers, beside the UI update, instead of on main at the first view group.
__declspec(dllexport) volatile LONG overhaul_early_prepare = 1;
// 1 spreads the early prepare's instance-texture fill over worker 1 and two helpers, a texture each: the fill was its
// longest task.
__declspec(dllexport) volatile LONG overhaul_split_fill = 1;
// 1 lets main, while workers draw a split view, record the next view of the group when it records through the queue
// walk and is of a type in overhaul_record_ahead_views. Record reads only frame constants, and with the view's groups
// copied the engine's buckets are free; that view's own record is then skipped.
__declspec(dllexport) volatile LONG overhaul_record_ahead = 1;
// 1 records each recorded-ahead view again at its own turn and compares the two (record-ahead keeps the second).
__declspec(dllexport) volatile LONG overhaul_record_verify;
// The 3D views of the default group, by type, but the shadow view (see records_ahead).
__declspec(dllexport) volatile uint64_t overhaul_record_ahead_views = 1ull << 0x0 | 1ull << 0xe | 1ull << 0xf | 1ull << 0x10 | 1ull << 0x11 |
                                                                       1ull << 0x16 | 1ull << 0x17 | 1ull << 0x18 | 1ull << 0x19 | 1ull << 0x1a |
                                                                       1ull << 0x1b | 1ull << 0x1c | 1ull << 0x1d | 1ull << 0x1e;
// 1 adds detail to the GPU timing of the views this module may split, reported per view: the wait before the first
// worker list, each list's time before its first command, inside it and after its last, and pipeline statistics. A switch.
__declspec(dllexport) volatile LONG overhaul_gpu_detail;
// 1 executes overhaul_probe_count empty command lists after each split view's own lists (overhaul_probe_bind 1: each binds
// the view's targets, as a worker's list does), so what a list costs the GPU apart from its draws shows. A switch.
__declspec(dllexport) volatile LONG overhaul_probe_lists;
__declspec(dllexport) volatile LONG overhaul_probe_count = 4;
__declspec(dllexport) volatile LONG overhaul_probe_bind;
// The change flags a forced job writes all of, instead of trusting what earlier groups left in the parameter library.
// Without the switches below only a worker range's first job is forced.
__declspec(dllexport) volatile LONG overhaul_forced_flags = FORCED_FLAGS;
// 1 forces the first job of each engine recorder a range reaches, main's range included: a recorder whose first group in
// the range opened without flags relies on parameters a group before the range set. A switch.
__declspec(dllexport) volatile LONG overhaul_force_recorders;
// 1 forces every job. A switch.
__declspec(dllexport) volatile LONG overhaul_force_all;
// 1 adds the bone palette (0x01) to a forced Variant group whose node draws without instancing. A switch.
__declspec(dllexport) volatile LONG overhaul_force_bones;
// 1 refreshes a worker's program copy from the program each time the worker binds it. A switch.
__declspec(dllexport) volatile LONG overhaul_program_refresh;
// 1 has a worker set a split view's texture slots in copies of its own, which its commits bind. Every thread's draws set
// the same library slots (a unit's material textures and the t_animation_map its bones come from, a particle's, an
// imposter's), and a commit bound whatever any thread had set last: units flashing through textures and shapes. Main
// keeps the engine's slots, which the workers then never write. A switch, on by default.
__declspec(dllexport) volatile LONG overhaul_texture_copies = 1;
// 1 does the same for the constant buffers draws write their values into (a unit's team colours and blood amounts, a
// particle's flipbook frame, UVs and colour), which the engine's writers store into inline, so nothing can redirect the
// store. Each thread keeps its own copy of every buffer its writers changed in its range: around each writer call (effect
// vtbl+0x48, +0x50, +0x58, +0xb8), under a lock, it puts back its values the other threads overwrote, the writer runs as
// it would alone (its compare-first and change-flag skips see this thread's values), and what it wrote is copied out;
// the uploads then read the thread's copy without the lock. A switch, on by default; the lock covers the writers only,
// since one thread committing at a time was far slower.
__declspec(dllexport) volatile LONG overhaul_value_copies = 1;
// 1 gives each thread drawing a split view its own copy of the shader library (SHADERS), whose parameters point into the
// thread's own values: the getter every unit and particle writer reaches the library through (0x1819f80d0) answers a
// thread's writers with its copy, so they run without valuecopies' lock and their compare-first skips see the thread's
// own values. Needs valuecopies, whose lock stays for a writer that reaches a parameter some other way. A switch, on by
// default.
__declspec(dllexport) volatile LONG overhaul_param_contexts = 1;
// 1 moves list A's items in the early prepare with every early-prepare worker that is free: its sort is the early
// prepare's longest task. A switch, off: the move is memory-bound, so it gains little, and the helpers spin beside main's
// UI update.
__declspec(dllexport) volatile LONG overhaul_parallel_sort;
// 1 records the effects views on two spare workers as the scene's views begin, into packets that each view's own record
// replays. A view gathers only its own emitters (the two share none), from the frame's camera. A switch, on by default.
__declspec(dllexport) volatile LONG overhaul_early_effects = 1;
// 1 records each early-recorded effects view again at its turn and compares the two; the engine's record is drawn. A switch.
__declspec(dllexport) volatile LONG overhaul_effects_verify;
// 1 records ahead on a worker of its own (the one after overhaul_workers), started as soon as the view's groups are
// copied, so the next view's record leaves main instead of filling its wait. A switch, off: main then waits for it, as a
// record is about as long as main's part of a view.
__declspec(dllexport) volatile LONG overhaul_record_worker;
// 1 records shadow cascades 1 and up on workers of their own (after overhaul_workers) while main draws cascade 0, into
// per-view packets instead of the engine's recorders; each cascade's execute builds its groups from them. A switch.
__declspec(dllexport) volatile LONG overhaul_parallel_cascades;
// 1 has each cascade recorded in parallel also recorded by the engine at its turn, and the groups the packets give compared
// with the engine's (the engine's are drawn). A switch.
__declspec(dllexport) volatile LONG overhaul_packet_verify;
// 1 lets the shadow view record its next cascade ahead (needs groupgather's per-view snapshot). A switch, on by default.
__declspec(dllexport) volatile LONG overhaul_record_cascades = 1;
// 1 has every thread that draws a split view's group-instance meshes see them through a view of its own (bind_group_view).
// A RIGID_GROUP_INSTANCE_MESH keeps a pointer to the item's chunker (+0x1e8) and its gatherer reads the chunk from there,
// so a worker drawing from its copy gathered the group the shared chunker was at, shading thousands of times the pixels.
// A switch, on by default.
__declspec(dllexport) volatile LONG overhaul_group_gather = 1;
// 1 samples every overhaul_group_every-th frame: pixels, vertices and primitives per group of each split view's ranges,
// reported by draw item. A switch.
__declspec(dllexport) volatile LONG overhaul_group_stats;
__declspec(dllexport) volatile LONG overhaul_group_every = 8;
// View types an overlap may run through; any other view drains it first. The 3D views from the shadow cascades to
// EFFECTS_ADDITIVE, without DOF, the upscalers, the HDR chain and the UI.
__declspec(dllexport) volatile uint64_t overhaul_overlap_views = 1ull << 0x0 | 1ull << 0x1 | 1ull << 0x5 | 1ull << 0xe | 1ull << 0xf | 1ull << 0x11 |
                                                                  1ull << 0x16 | 1ull << 0x17 | 1ull << 0x18 | 1ull << 0x19 | 1ull << 0x1a | 1ull << 0x1b |
                                                                  1ull << 0x1c | 1ull << 0x1d | 1ull << 0x1e | 1ull << 0x28 | 1ull << 0x2b | 1ull << 0x2c |
                                                                  1ull << 0x30 | 1ull << 0x31;

static const BYTE execute_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4c, 0x24, 0x08};
static const BYTE bind_pass_state_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
static const BYTE upload_constant_buffers_prologue[] = {0x4c, 0x8b, 0xdc, 0x4d, 0x89, 0x4b, 0x20, 0x4d, 0x89, 0x43, 0x18, 0x55, 0x41, 0x54, 0x41, 0x55};
static const BYTE kick_stream_prologue[] = {0x48, 0x8b, 0x49, 0x20, 0xe9, 0x07, 0x00, 0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
static const uint32_t expected_draw_rvas[DRAW_SLOTS] = {0x1abcb50, 0x1abcb60, 0x1abcb70, 0x1abcb90, 0x1abcba0};
static const uint32_t derived_chunker_vtable_rvas[] = {0x23f35e0, 0x23f0108, 0x23f0340};
static const BYTE prepare_prologue[] = {0x48, 0x8b, 0xc4, 0x55, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1, 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00};
static const BYTE compute_keys_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20};
static const BYTE fill_instance_textures_prologue[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x80, 0xb9, 0xe1, 0x02, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9};
static const BYTE render_view_prologue[] = {0x4c, 0x89, 0x44, 0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00};
static const BYTE end_frame_prologue[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83, 0xb9, 0x80, 0x01, 0x00, 0x00, 0x00};
static const BYTE ring_allocate_prologue[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x8b, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xda};
static const BYTE record_queues_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x59, 0x28, 0x48, 0x8b, 0xf9};
static const BYTE reset_buckets_prologue[] = {0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x45, 0x33, 0xf6, 0x48, 0x89, 0x6c, 0x24, 0x38};
static const BYTE render_views_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54};
static const BYTE battle_view_update_prologue[] = {0x48, 0x8b, 0xc4, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x48, 0x89, 0x58, 0x10};
static const BYTE fill_texture_prologue[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec, 0x40};
static const BYTE texture_set_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20};
static const BYTE build_srv_list_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56};
// Its 11 bytes and the padding after them.
static const BYTE touch_prologue[] = {0x48, 0x8b, 0x41, 0x18, 0xff, 0x41, 0x20, 0xff, 0x40, 0x38, 0xc3, 0xcc, 0xcc, 0xcc};
static const BYTE push_record_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20};
// Its 11 bytes and the padding after them.
static const BYTE library_getter_prologue[] = {0x48, 0x8b, 0x41, 0x30, 0x48, 0x05, 0xc8, 0x26, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc};
static const BYTE node_texture_prologue[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xc2, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x91, 0xd8, 0x02, 0x00, 0x00};
static const BYTE effects_record_prologue[] = {0x48, 0x8b, 0xc4, 0x55, 0x41, 0x56, 0x48, 0x8d, 0xa8, 0x68, 0xff, 0xff, 0xff, 0x48, 0x81, 0xec, 0x88, 0x01, 0x00, 0x00};
static const BYTE record_whole_prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20};
static const BYTE group_gather_prologue[] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x55, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30};

static const S2Api *api;
static BYTE *engine;
static ExecuteFn original_execute;
static SetShaderFn original_set_shaders[STAGES];
static BindPassStateFn original_bind_pass_state;
static TextureSetFn original_texture_set;
static BuildSrvListFn original_build_srv_list;
static BOOL texture_copies_full_logged;
static TouchFn original_touch;
// Set by main for the whole of a split view whose values are kept per thread.
static volatile LONG values_active;
static ValueState main_values;
static SRWLOCK value_lock = SRWLOCK_INIT;
static const uint32_t writer_slots[WRITER_SLOTS] = {0x48, 0x50, 0x58, 0xb8};
// Effect vtables whose writer slots point at the wrappers, with the functions they pointed at.
typedef struct {
    void **vtable;
    WriterFn original[WRITER_SLOTS];
    // A slot is locked once it has been seen writing, and restores the buffers it has written before.
    volatile LONG writes[WRITER_SLOTS];
    BYTE *learned[WRITER_SLOTS][LEARNED_BUFFERS];
    // paramcontexts: a slot seen writing a buffer the library copies do not hold runs under the lock from then on.
    volatile LONG context_locked[WRITER_SLOTS];
} WrappedClass;
static WrappedClass wrapped_classes[WRAPPED_CLASSES];
static LONG wrapped_class_count;
static BOOL wrapped_classes_full_logged, value_copies_full_logged, learned_full_logged, binding_infos_full_logged;
// Every constant buffer a wrapped writer has touched: the bytes the first write left, and whether a write has left others.
typedef struct {
    BYTE *binding, *reference;
    volatile LONG varies;
} BindingInfo;
static BindingInfo binding_infos[BINDING_INFOS];
static LONG binding_info_count;
static LibraryGetterFn original_library_getter;
static NodeTextureFn original_node_texture;
// A SHADER_PARAMETER of the library (binding +0x18, version +0x20, value pointer +0x28): its offset, its buffer, where its
// value starts and how far the next one in the buffer is, and the two pointers it had when found.
typedef struct {
    uint32_t offset, buffer, value_offset, value_bytes;
    BYTE *binding, *value;
} LibraryRecord;
#define MAX_BUFFER_BINDINGS 4
typedef struct {
    BYTE *buffer, *shadow, *bindings[MAX_BUFFER_BINDINGS];
    uint32_t bytes, data_offset, binding_count;
} LibraryBuffer;
static struct {
    BYTE *shared, *snapshot;
    LibraryRecord *records;
    uint32_t record_count, buffer_count, data_bytes, epoch;
    LibraryBuffer buffers[LIBRARY_BUFFERS];
    uint32_t index[2 * LIBRARY_BUFFERS];
    uint64_t checked_stamp;
    // Per qword of the library: 1 where a copy is expected to differ (versions, values, texture slots), 2 at a texture slot.
    uint8_t *volatile_qwords;
    // The texture slots seen set, and their contents at the fork, which each range's copy starts from.
    uint32_t slot_offsets[MAX_LIBRARY_SLOTS];
    volatile LONG slot_count;
    uint32_t snapshot_slots;
    BYTE *slot_snapshot;
    BOOL failed;
} contexts;
// Set by main for a split view whose drawing threads write in their library copies.
static volatile LONG contexts_split;
static void *leak_callers[MAX_LEAK_CALLERS], *escape_callers[MAX_LEAK_CALLERS];
static volatile LONG leak_caller_count, escape_caller_count, escaped_touches;
// Where touches came from that no wrapped writer made, on a drawing thread of a split view.
static void *unwrapped_callers[MAX_UNWRAPPED_CALLERS];
static volatile LONG unwrapped_caller_count;
static GatherFn original_group_gather;
static UploadConstantBuffersFn original_upload_constant_buffers;
static SRWLOCK commit_lock = SRWLOCK_INIT;
static volatile LONG64 commit_wait_ticks;
static GpuFrame gpu_frames[GPU_TIMING_FRAMES];
static uint64_t gpu_frame_number, gpu_last_stamp;
static BOOL gpu_timing_ready;
static double gpu_view_ms[2], gpu_main_ms, gpu_lists_ms;
static uint64_t gpu_timed_frames[2], gpu_lost_frames;
static BOOL gpu_detail_ready;
static GpuDetail gpu_detail[2][VIEW_TYPES][STATS_CASCADES];
static uint64_t gpu_detail_frames[2], gpu_detail_lost, gpu_inside_lost;
static ID3D11DeviceContext *probe_context;
static BOOL probe_error;
static uint64_t probe_lists_run[2];
static MainGroup main_group;
static volatile LONG packets_active;
static CascadeRecord cascade_records[STATS_CASCADES];
// The cascade whose packets its execute draws, set at its record and taken at its execute.
static CascadeRecord *packet_view;
static PushRecordFn original_push_record;
static RecordWholeFn original_record_whole;
// Per mode: cascades recorded on workers, worker record time, main's wait for them and its own queues, packets, and the
// packetverify comparison.
static uint64_t parallel_cascade_count[2], packet_count[2], replayed_views[2], verified_views, differing_views;
static LONGLONG parallel_worker_ticks[2], parallel_wait_ticks[2], parallel_main_ticks[2];
static GroupPool group_pools[GROUP_POOLS];
static GroupPool *current_group_pool;
static BOOL group_pools_ready;
static GroupStat group_stats[GROUP_TABLE_SIZE];
static uint64_t group_frames[2], group_dropped, group_lost, group_overflow;
static void *ignored_original;
static SortBucketFn sort_bucket;
static StreamInFn stream_in;
static volatile uint64_t *frame_stamp;
static uint64_t last_stamp;
__attribute__((used)) DWORD tls_index;
static WorkCounts work[2], view_work[2][VIEW_TYPES + 1];
// The view main's draws belong to; VIEW_TYPES outside execute.
static uint32_t main_view_type = VIEW_TYPES, main_view_cascade;
static void *original_draws[DRAW_SLOTS];
static Worker *workers[MAX_WORKERS];
static int worker_count;
static const char *worker_error;
static ID3D11Device *device;
static volatile LONG generation;
static ID3D11DeviceContext *immediate_context;
static ViewSlot slots[MAX_SLOTS];
static uint32_t slots_taken, slots_freed;
static Submission submissions[MAX_SUBMISSIONS];
static uint32_t submissions_pushed, submissions_done;
static ContextWrapper *wrapper;
static ID3D11DeviceContext *main_deferred;
static BOOL overlap_checked, overlap_frame, overlap_active;
static UploadTracker main_uploads;
static DWORD render_thread;
static UploadedBuffer main_rings[CB_TABLE_SIZE];
static uint32_t main_segment, main_segment_rings;
static RenderViewFn original_render_view;
static EndFrameFn original_end_frame;
static RingAllocateFn original_ring_allocate;
static BYTE *zero_sort_flags;
static uint32_t zero_sort_capacity;
static uint64_t enabled_frames;
static Timers timers[2];
static Balance balances[VIEW_TYPES][STATS_CASCADES];
// The balance of the view main is executing; NULL outside execute.
static Balance *current_balance;
static RecordQueuesFn original_record_queues;
static ResetBucketsFn original_reset_buckets;
static RenderViewsFn original_render_views;
static BeginBucketFn original_begin_bucket;
static BattleViewUpdateFn original_battle_view_update;
// The view group being rendered, for the view after the current one.
static BYTE *render_context;
// A view recorded during the previous view's draws, by main in its wait or by the record worker: the reset after that view
// and this view's own record are skipped. worker is set while the record worker may still be filling the buckets.
static struct {
    BYTE *view;
    uint32_t cascade;
    BOOL skip_reset;
    Worker *volatile worker;
} ahead;
static volatile LONG64 record_worker_ticks[2];
static BYTE *verify_records[2];
static size_t verify_capacity[2];
// The battle's Variant node is the first prepared in each scene update; it is prepared early only while the battle
// updated and prepared it in the scene update before, so the node is known to be alive.
static BYTE *battle_node;
static uint32_t battle_node_stamp, battle_update_stamp;
static struct {
    BYTE *node;
    BOOL pending;
    volatile LONG poses_merged;
    uint32_t stamp, counts[4];
} early;
static FillTextureFn original_fill_texture;
static ID3D11Multithread *context_lock;
static TextureFill texture_fills[MAX_TEXTURE_FILLS];
static uint32_t texture_fill_count;
static volatile LONG next_texture_fill;
static __thread BOOL collecting_fills;
// The flag of the switch an A/B run flips; the per-mode counters split by it.
__attribute__((used)) volatile LONG *ab_flag = &overhaul_enabled;
static const char *ab_name = "overhaul";
static ViewStats view_stats[2][VIEW_TYPES][STATS_CASCADES];
static const char *const view_type_names[VIEW_TYPES] = {[0x0] = "FRAME", [0x1] = "SHADOW_MAP", [0x2] = "HDR_TO_FRAME", [0x5] = "SUPER_TEXTURE", [0x8] = "HIGH_PASS", [0x9] = "HDR_LENSE_FLARE", [0xa] = "HDR_PASS_THROUGH", [0xb] = "HDR_DOWNSAMPLE", [0xc] = "HDR_BLURX", [0xd] = "HDR_BLURY", [0xe] = "EFFECTS_BLEND", [0xf] = "EFFECTS_ADDITIVE", [0x10] = "EFFECTS_TO_FRAME", [0x11] = "DISTORTION", [0x12] = "OVERLAY / UI", [0x13] = "GBUFFER_HIGHLIGHT_OVERLAY", [0x14] = "ENVMAP_GEN", [0x16] = "GBUFFER", [0x17] = "GBUFFER_HIGHLIGHT", [0x18] = "DEFERRED_SHADING", [0x19] = "DEFERRED_DECAL", [0x1a] = "DEFERRED_POINTLIGHT", [0x1b] = "DEFERRED_REFRACTION", [0x1c] = "DEFERRED_FOG", [0x1d] = "DEFERRED_ALPHA", [0x1e] = "DEFERRED_OPAQUE", [0x1f] = "DOF", [0x21] = "MLAA_EDGES", [0x22] = "MLAA_WEIGHTS_PREPASS", [0x23] = "MLAA_WEIGHTS", [0x24] = "MLAA_FINAL", [0x25] = "FXAA", [0x26] = "FXAA_UI", [0x27] = "TAA / DLSS", [0x28] = "STATIC_MOTION_VECTORS", [0x29] = "HDR_BLOOM_HIGH_PASS", [0x2a] = "LIGHT_PROBE", [0x2b] = "SSAO", [0x2c] = "SCREEN_SPACE_SHADOWS", [0x2d] = "FSR3", [0x2f] = "XESS", [0x30] = "DEFERRED_COMPUTE", [0x31] = "ENV_SPECULAR", [0x32] = "HDR_GBUFFER_DEBUG"};
static uint64_t fallback_unknown_recorders;
static Kicks main_kicks;
static volatile DWORD split_thread;
static PrepareFn original_prepare;
static EffectsRecordFn original_effects_record;
// The effects views recorded early this frame, in packets, by the workers in their records.
static CascadeRecord *effects_records[EARLY_EFFECTS];
static int effects_pending;
static uint32_t effects_mxcsr;
static uint64_t early_effect_views[2], early_effect_packets[2];
static LONGLONG early_effect_worker_ticks[2], early_effect_wait_ticks[2], early_effect_replay_ticks[2];
typedef struct {
    uint64_t key, index;
} SortKey;
enum { SORT_WAITING, SORT_GATHER, SORT_COPY, SORT_DONE };
// List A's sort in the early prepare (parallelsort): the order from one radix sort over the keys, the items moved in chunks
// by whichever early-prepare workers are free, out of place into scratch, then back.
static struct {
    BYTE *items, *scratch;
    SortKey *keys, *swap, *order;
    uint32_t capacity, count, chunks;
    volatile LONG state, next[2], finished[2], helped;
    LONGLONG ticks;
} list_sort = {.state = SORT_DONE};
static ComputeKeysFn original_compute_keys;
static TaskFn original_fill_instance_textures;
static volatile BOOL prepare_split_active;
static PrepareStats prepare_stats[2];
static uint64_t prepare_stamp;

static int ab_mode(void) {
    return *ab_flag != 0;
}

static Worker *current_worker(void) {
    return (Worker *)__readgsqword(TLS_SLOTS_OFFSET + 8 * tls_index);
}

static void *method(void *object, int offset) {
    return (*(void ***)object)[offset / 8];
}

static LONGLONG now_ticks(void);
static BYTE *job_item(const Job *job);
static void log_contexts(double busy_frames);

// The value state of a thread drawing a range of a split view, or NULL.
static ValueState *drawing_values(void) {
    if (!values_active) return NULL;
    Worker *worker = current_worker();
    if (worker) return worker->values.drawing ? &worker->values : NULL;
    return main_values.drawing && GetCurrentThreadId() == split_thread ? &main_values : NULL;
}

static BOOL heap_pointer(const BYTE *value) {
    return (uintptr_t)value >= 0x10000 && (uintptr_t)value < 0x7ff000000000ull && !((uintptr_t)value & 7);
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

// A parameter is recognised by its binding (+0x18) leading to a buffer whose shadow its value pointer (+0x28) points into
// (about 1,200 in 190 buffers). The library holds other pointers too, so each is read through ReadProcessMemory.
static BOOL build_library(void) {
    contexts.shared = *(BYTE **)(*(BYTE **)(engine + ENGINE_RVA) + 0x30) + LIBRARY_OFFSET;
    if (!contexts.records && !(contexts.records = malloc(MAX_LIBRARY_RECORDS * sizeof *contexts.records))) {
        api->log_line("overhaul: out of memory for the shader library's parameters");
        __builtin_trap();
    }
    contexts.record_count = contexts.buffer_count = contexts.data_bytes = 0;
    memset(contexts.index, 0, sizeof contexts.index);
    for (uint32_t offset = 0; offset + 0x30 <= LIBRARY_SIZE; offset += 8) {
        BYTE *binding = *(BYTE **)(contexts.shared + offset + 0x18), *value = *(BYTE **)(contexts.shared + offset + 0x28), *buffer, *shadow;
        uint32_t bytes;
        if (!heap_pointer(binding) || !value || !read_memory(binding + 0x30, &buffer, sizeof buffer) || !heap_pointer(buffer) ||
            !read_memory(buffer + 0x40, &shadow, sizeof shadow) || !read_memory(buffer + 0x48, &bytes, sizeof bytes) || !shadow || !bytes ||
            bytes > 65536 || value < shadow || value >= shadow + bytes)
            continue;
        int b = add_library_buffer(buffer, binding, shadow, bytes);
        if (b < 0 || contexts.record_count == MAX_LIBRARY_RECORDS) {
            api->log_line("overhaul: paramcontexts: the shader library has more parameters or buffers than the tables hold");
            return FALSE;
        }
        contexts.records[contexts.record_count++] = (LibraryRecord){offset, (uint32_t)b, (uint32_t)(value - shadow), 0, binding, value};
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
    if (!contexts.volatile_qwords) contexts.volatile_qwords = calloc(LIBRARY_SIZE / 8, 1);
    if (!(contexts.snapshot = malloc(contexts.data_bytes ? contexts.data_bytes : 16)) || !contexts.volatile_qwords) {
        api->log_line("overhaul: out of memory for the shader library's values");
        __builtin_trap();
    }
    for (uint32_t r = 0; r < contexts.record_count; r++) contexts.volatile_qwords[contexts.records[r].offset / 8 + 4] = contexts.volatile_qwords[contexts.records[r].offset / 8 + 5] = 1;
    // Most bindings live in the library; their versions (+0x38) are the engine's, never a copy's.
    for (uint32_t b = 0; b < contexts.buffer_count; b++)
        for (uint32_t k = 0; k < contexts.buffers[b].binding_count; k++) {
            BYTE *binding = contexts.buffers[b].bindings[k];
            if (binding >= contexts.shared && binding + 0x40 <= contexts.shared + LIBRARY_SIZE) contexts.volatile_qwords[(binding - contexts.shared) / 8 + 7] = 1;
        }
    contexts.epoch++;
    api->log_line("overhaul: paramcontexts: %u parameters in %u buffers (%u bytes) in the shader library", contexts.record_count, contexts.buffer_count,
                  contexts.data_bytes);
    return contexts.record_count > 0;
}

static BOOL library_holds(void) {
    if (*(BYTE **)(*(BYTE **)(engine + ENGINE_RVA) + 0x30) + LIBRARY_OFFSET != contexts.shared) return FALSE;
    for (uint32_t r = 0; r < contexts.record_count; r++) {
        const LibraryRecord *record = &contexts.records[r];
        if (*(BYTE **)(contexts.shared + record->offset + 0x18) != record->binding || *(BYTE **)(contexts.shared + record->offset + 0x28) != record->value)
            return FALSE;
    }
    for (uint32_t b = 0; b < contexts.buffer_count; b++)
        if (*(BYTE **)(contexts.buffers[b].buffer + 0x40) != contexts.buffers[b].shadow || *(uint32_t *)(contexts.buffers[b].buffer + 0x48) != contexts.buffers[b].bytes)
            return FALSE;
    return TRUE;
}

// On main at a split view's fork: the library checked once a frame and rebuilt if its parameters moved, and the values
// every range of the view starts from.
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

// On the drawing thread, at its first range of a library build.
static void prepare_state_library(ValueState *state) {
    if (state->library && state->library_epoch == contexts.epoch) return;
    free(state->library);
    free(state->data);
    free(state->library_copies);
    state->library = malloc(LIBRARY_SIZE);
    state->data = malloc(contexts.data_bytes);
    state->library_copies = calloc(contexts.buffer_count, sizeof *state->library_copies);
    if (!state->library || !state->data || !state->library_copies) {
        api->log_line("overhaul: out of memory for a thread's copy of the shader library");
        __builtin_trap();
    }
    memcpy(state->library, contexts.shared, LIBRARY_SIZE);
    for (uint32_t r = 0; r < contexts.record_count; r++) {
        const LibraryRecord *record = &contexts.records[r];
        *(BYTE **)(state->library + record->offset + 0x28) = state->data + contexts.buffers[record->buffer].data_offset + record->value_offset;
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
        uint32_t bytes = *(uint32_t *)(buffer + 0x48);
        if (state->active_count == VALUE_COPIES / 2 || state->arena_used + bytes > VALUE_ARENA_BYTES) {
            state->full++;
            if (!value_copies_full_logged) {
                value_copies_full_logged = TRUE;
                api->log_line("overhaul: a range wrote more than %d constant buffers or %d KB of them; the rest stay shared", VALUE_COPIES / 2,
                              VALUE_ARENA_BYTES / 1024);
            }
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
    return copy ? copy->data : *(void **)(buffer + 0x40);
}

static void note_caller(void **callers, volatile LONG *count, void *caller) {
    LONG seen = *count;
    for (LONG i = 0; i < seen && i < MAX_LEAK_CALLERS; i++)
        if (callers[i] == caller) return;
    LONG index = InterlockedIncrement(count) - 1;
    if (index < MAX_LEAK_CALLERS) callers[index] = caller;
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

// A touch by a writer running in the thread's library copy. A parameter outside the copy was reached without the getter
// (the Variant node keeps a pointer to the library for 0x181b47e50): its store went to the shared values, which the thread's
// copy takes it from.
static void context_touch(ValueState *state, BYTE *parameter, BYTE *binding, void *caller) {
    BYTE *buffer = *(BYTE **)(binding + 0x30);
    int b = buffer ? library_buffer_index(buffer) : -1;
    if (parameter >= state->library && parameter < state->library + LIBRARY_SIZE) {
        // The engine's own parameter keeps its version moving, as every writer would have it.
        InterlockedIncrement((volatile LONG *)(contexts.shared + (parameter - state->library) + 0x20));
        if (b >= 0) state->library_copies[b].changed = 1;
        return;
    }
    state->leaks++;
    note_caller(leak_callers, &leak_caller_count, caller);
    const LibraryRecord *record = NULL;
    if (parameter >= contexts.shared && parameter < contexts.shared + LIBRARY_SIZE) record = find_record((uint32_t)(parameter - contexts.shared));
    if (!record && b >= 0) {
        BYTE *value = *(BYTE **)(parameter + 0x28), *shadow = contexts.buffers[b].shadow;
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
            memcpy(copy->data, *(void **)(buffer + 0x40), *(uint32_t *)(buffer + 0x48));
            copy->version = *(volatile uint32_t *)(binding + 0x38);
            copy->changed = 1;
        }
    }
    if (state->writer_lock && !*state->writer_lock) {
        *state->writer_lock = 1;
        state->demotions++;
    }
}

// SHADER_PARAMETER::touch (0x181aeed70) follows every parameter write: it bumps the parameter's version (+0x20) and its
// buffer's (binding +0x38). The engine's increments are not atomic, and a lost one is a missed upload.
static void touch_hook(BYTE *parameter) {
    BYTE *binding = *(BYTE **)(parameter + 0x18);
    if (!values_active) {
        ++*(volatile uint32_t *)(parameter + 0x20);
        ++*(volatile uint32_t *)(binding + 0x38);
        return;
    }
    InterlockedIncrement((volatile LONG *)(parameter + 0x20));
    InterlockedIncrement((volatile LONG *)(binding + 0x38));
    ValueState *state = drawing_values();
    if (contexts_split && (!state || !state->in_context) && in_library_copy(parameter)) {
        InterlockedIncrement(&escaped_touches);
        note_caller(escape_callers, &escape_caller_count, __builtin_return_address(0));
    }
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
    void *caller = __builtin_return_address(0);
    LONG count = unwrapped_caller_count;
    for (LONG i = 0; i < count && i < MAX_UNWRAPPED_CALLERS; i++)
        if (unwrapped_callers[i] == caller) return;
    LONG index = InterlockedIncrement(&unwrapped_caller_count) - 1;
    if (index < MAX_UNWRAPPED_CALLERS) unwrapped_callers[index] = caller;
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
        BYTE *buffer = *(BYTE **)(binding + 0x30);
        uint32_t bytes = *(uint32_t *)(buffer + 0x48);
        if (2 * (binding_info_count + 1) > BINDING_INFOS) {
            if (!binding_infos_full_logged) {
                binding_infos_full_logged = TRUE;
                api->log_line("overhaul: more than %d constant buffers written in split views; the rest count as varying", BINDING_INFOS / 2);
            }
            return NULL;
        }
        if (!(info->reference = malloc(bytes ? bytes : 1))) {
            api->log_line("overhaul: out of memory for a constant buffer's reference values");
            __builtin_trap();
        }
        memcpy(info->reference, *(void **)(buffer + 0x40), bytes);
        info->varies = 0;
        binding_info_count++;
        InterlockedExchangePointer((void *volatile *)&info->binding, binding);
        return info;
    }
}

// The buffers the writer just wrote become the thread's copies, and what the slot is known to write.
static void copy_out_values(ValueState *state, WrappedClass *wrapped, int slot) {
    for (uint32_t i = 0; i < state->touched_count; i++) {
        BYTE *binding = state->touched[i], *buffer = *(BYTE **)(binding + 0x30);
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
        if (k == LEARNED_BUFFERS && !learned_full_logged) {
            learned_full_logged = TRUE;
            api->log_line("overhaul: a writer slot wrote more than %d constant buffers; the rest are not put back before it", LEARNED_BUFFERS);
        }
        ValueCopy *copy = value_copy(state, buffer, TRUE);
        if (!copy) continue;
        copy->binding = binding;
        memcpy(copy->data, *(void **)(buffer + 0x40), *(uint32_t *)(buffer + 0x48));
        copy->version = *(volatile uint32_t *)(binding + 0x38);
        copy->changed = 1;
        state->copies_out++;
    }
}

static uint64_t call_writer(int slot, uint64_t effect, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    void **vtable = *(void ***)effect;
    WrappedClass *wrapped = wrapped_class(vtable);
    if (wrapped->vtable != vtable) {
        api->log_line("overhaul: a wrapped writer was called through a vtable it does not know");
        __builtin_trap();
    }
    WriterFn original = wrapped->original[slot];
    ValueState *state = drawing_values();
    // A writer the effect's own writer calls runs inside its lock.
    if (!state || state->depth) return original(effect, b, c, d, e, f);
    if (state->contexts && !wrapped->context_locked[slot]) {
        __attribute__((aligned(16))) BYTE view[CACHED_EFFECT_BYTES];
        uint64_t target = effect;
        BYTE *base = original == (WriterFn)(void *)(engine + VARIANT_CACHED_WRITER_RVA) || original == (WriterFn)(void *)(engine + VARIANT_B_CACHED_WRITER_RVA)
                         ? copy_base(state, *(BYTE **)(effect + 0xc8))
                         : NULL;
        if (base) {
            memcpy(view, (const void *)effect, sizeof view);
            *(BYTE **)(view + 0xc8) = base;
            target = (uint64_t)view;
        }
        state->depth = 1;
        state->in_context = TRUE;
        state->writer_lock = &wrapped->context_locked[slot];
        uint64_t result = original(target, b, c, d, e, f);
        state->in_context = FALSE;
        state->depth = 0;
        state->context_calls++;
        return result;
    }
    if (state->contexts) wrapped->writes[slot] = 1;
    state->touched_count = 0;
    // Most writer calls change nothing: a shadow view's materials write nothing, its units only the same 1024.0 each chunk.
    // A slot runs unlocked while every buffer it touches only ever holds the bytes of its first write, the same for every
    // thread; one that varies locks the slot from then on (that first varying write is copied out as it stands).
    if (!wrapped->writes[slot]) {
        state->depth = 1;
        uint64_t result = original(effect, b, c, d, e, f);
        state->depth = 0;
        state->unlocked_calls++;
        if (!state->touched_count) return result;
        BOOL constant = TRUE;
        for (uint32_t i = 0; i < state->touched_count && constant; i++) {
            BYTE *buffer = *(BYTE **)(state->touched[i] + 0x30);
            BindingInfo *info = buffer ? binding_info(state->touched[i], FALSE) : NULL;
            constant = info && !info->varies && !memcmp(info->reference, *(void **)(buffer + 0x40), *(uint32_t *)(buffer + 0x48));
        }
        if (constant) return result;
        lock_values(state);
        for (uint32_t i = 0; i < state->touched_count; i++) {
            BYTE *buffer = *(BYTE **)(state->touched[i] + 0x30);
            BindingInfo *info = buffer ? binding_info(state->touched[i], TRUE) : NULL;
            if (info && memcmp(info->reference, *(void **)(buffer + 0x40), *(uint32_t *)(buffer + 0x48))) info->varies = 1;
            if (!info || info->varies) wrapped->writes[slot] = 1;
        }
        copy_out_values(state, wrapped, slot);
        ReleaseSRWLockExclusive(&value_lock);
        return result;
    }
    lock_values(state);
    // Each buffer the slot writes holds this thread's values when its writer runs: put back where another thread changed
    // it, or, the first time in the range, taken as it stands (a compare-first writer that then skips a value equal to it
    // has left the thread the right one).
    BYTE **learned = wrapped->learned[slot];
    for (int k = 0; k < LEARNED_BUFFERS && learned[k]; k++) {
        BYTE *buffer = *(BYTE **)(learned[k] + 0x30);
        if (!buffer) continue;
        ValueCopy *copy = value_copy(state, buffer, FALSE);
        if (!copy) {
            if (!(copy = value_copy(state, buffer, TRUE))) continue;
            copy->binding = learned[k];
            memcpy(copy->data, *(void **)(buffer + 0x40), *(uint32_t *)(buffer + 0x48));
            copy->version = *(volatile uint32_t *)(learned[k] + 0x38);
            state->seeds++;
            continue;
        }
        uint32_t version = *(volatile uint32_t *)(copy->binding + 0x38);
        if (version == copy->version) continue;
        memcpy(*(void **)(buffer + 0x40), copy->data, *(uint32_t *)(buffer + 0x48));
        copy->version = (uint32_t)InterlockedIncrement((volatile LONG *)(copy->binding + 0x38));
        state->restores++;
    }
    state->depth = 1;
    uint64_t result = original(effect, b, c, d, e, f);
    state->depth = 0;
    copy_out_values(state, wrapped, slot);
    state->calls++;
    ReleaseSRWLockExclusive(&value_lock);
    return result;
}

#define WRITER_WRAPPER(index)                                                                                                       \
    static uint64_t writer_wrapper_##index(uint64_t effect, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {          \
        return call_writer(index, effect, b, c, d, e, f);                                                                          \
    }
WRITER_WRAPPER(0)
WRITER_WRAPPER(1)
WRITER_WRAPPER(2)
WRITER_WRAPPER(3)
static void *const writer_wrappers[WRITER_SLOTS] = {(void *)writer_wrapper_0, (void *)writer_wrapper_1, (void *)writer_wrapper_2, (void *)writer_wrapper_3};

// On main, before a split view's workers start: the writer slots of each effect class it draws point at the wrappers from
// then on (they pass straight through outside a split view's ranges).
static void wrap_writers(const ViewSlot *slot) {
    void **last = NULL;
    for (uint32_t j = 0; j < slot->job_count; j++) {
        void **vtable = **(void ****)(job_item(&slot->jobs[j]) + 0x10);
        if (vtable == last) continue;
        last = vtable;
        WrappedClass *wrapped = wrapped_class(vtable);
        if (wrapped->vtable) continue;
        if (2 * (wrapped_class_count + 1) > WRAPPED_CLASSES) {
            if (!wrapped_classes_full_logged) {
                wrapped_classes_full_logged = TRUE;
                api->log_line("overhaul: more than %d effect classes to wrap; the rest write the shared buffers", WRAPPED_CLASSES);
            }
            continue;
        }
        DWORD protection;
        if (!VirtualProtect(vtable, 0xc0, PAGE_READWRITE, &protection)) {
            api->log_line("overhaul: VirtualProtect of an effect vtable failed: error %lu", GetLastError());
            continue;
        }
        for (int s = 0; s < WRITER_SLOTS; s++) wrapped->original[s] = (WriterFn)vtable[writer_slots[s] / 8];
        // Published, originals first, before the slots point at the wrappers that look it up.
        InterlockedExchangePointer((void *volatile *)&wrapped->vtable, vtable);
        wrapped_class_count++;
        for (int s = 0; s < WRITER_SLOTS; s++) vtable[writer_slots[s] / 8] = writer_wrappers[s];
        VirtualProtect(vtable, 0xc0, protection, &protection);
    }
}

// At the join: the engine's shadows get the values the view's last writer of each left, ranges in draw order, as if one
// thread had drawn the view.
static void write_back_values(const ValueState *state) {
    for (uint32_t i = 0; i < state->active_count; i++) {
        const ValueCopy *copy = &state->copies[state->active[i]];
        memcpy(*(void **)(copy->buffer + 0x40), copy->data, *(uint32_t *)(copy->buffer + 0x48));
        InterlockedIncrement((volatile LONG *)(copy->binding + 0x38));
    }
    if (!state->contexts) return;
    for (uint32_t b = 0; b < contexts.buffer_count; b++) {
        if (!state->library_copies[b].changed) continue;
        const LibraryBuffer *buffer = &contexts.buffers[b];
        memcpy(buffer->shadow, state->library_copies[b].data, buffer->bytes);
        for (uint32_t k = 0; k < buffer->binding_count; k++) InterlockedIncrement((volatile LONG *)(buffer->bindings[k] + 0x38));
    }
}

static void node_texture_hook(BYTE *node, uint64_t instance_texture) {
    ValueState *state = contexts_split ? drawing_values() : NULL;
    BYTE *base = state && state->in_context ? copy_base(state, *(BYTE **)(node + 0xd8)) : NULL;
    if (!base) {
        original_node_texture(node, instance_texture);
        return;
    }
    __attribute__((aligned(16))) BYTE view[CACHED_NODE_BYTES];
    memcpy(view, node, sizeof view);
    *(BYTE **)(view + 0xd8) = base;
    original_node_texture(view, instance_texture);
}

// The library getter (0x1819f80d0): a writer running in a thread's library copy gets the copy.
static BYTE *library_getter_hook(BYTE *holder) {
    BYTE *library = *(BYTE **)(holder + 0x30) + LIBRARY_OFFSET;
    if (!contexts_split) return library;
    ValueState *state = drawing_values();
    return state && state->in_context && library == contexts.shared ? state->library : library;
}

static LONGLONG now_ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static void count_draw(uint64_t instances, uint64_t elements) {
    Worker *worker = current_worker();
    if (worker) {
        WorkCounts *counts = &worker->work[ab_mode()], *view_counts = &worker->view_work[ab_mode()][worker->view_type];
        counts->draws++;
        counts->instances += instances;
        counts->elements += elements;
        view_counts->draws++;
        view_counts->instances += instances;
        return;
    }
    WorkCounts *view_counts = &view_work[ab_mode()][main_view_type];
    view_counts->draws++;
    view_counts->instances += instances;
    WorkCounts *counts = &work[ab_mode()];
    if (*frame_stamp != last_stamp) {
        last_stamp = *frame_stamp;
        counts->frames++;
        if (overhaul_enabled) enabled_frames++;
    }
    counts->draws++;
    counts->instances += instances;
    counts->elements += elements;
}

static void draw_hook(BYTE *renderer, UINT vertex_count, UINT start_vertex) {
    count_draw(1, vertex_count);
    ((DrawFn)original_draws[0])(renderer, vertex_count, start_vertex);
}

static void draw_indexed_hook(BYTE *renderer, UINT index_count, UINT start_index, INT base_vertex) {
    count_draw(1, index_count);
    ((DrawIndexedFn)original_draws[1])(renderer, index_count, start_index, base_vertex);
}

static void draw_indexed_instanced_hook(BYTE *renderer, UINT index_count, UINT instances, UINT start_index, INT base_vertex, UINT start_instance) {
    count_draw(instances, (uint64_t)index_count * instances);
    ((DrawIndexedInstancedFn)original_draws[2])(renderer, index_count, instances, start_index, base_vertex, start_instance);
}

static void draw_instanced_hook(BYTE *renderer, UINT vertex_count, UINT instances, UINT start_vertex, UINT start_instance) {
    count_draw(instances, (uint64_t)vertex_count * instances);
    ((DrawInstancedFn)original_draws[3])(renderer, vertex_count, instances, start_vertex, start_instance);
}

// The instance count lives in a GPU buffer, so an indirect draw counts as one instance.
static void draw_indirect_hook(BYTE *renderer, BYTE *arguments, UINT offset) {
    count_draw(1, 0);
    ((DrawIndirectFn)original_draws[4])(renderer, arguments, offset);
}

// CB uploads and texture streaming reach the renderer through g_render_device->get_renderer(), not execute's argument.
static BYTE *get_renderer_hook(BYTE *render_device) {
    Worker *worker = current_worker();
    return worker ? worker->renderer : render_device + RENDERER_OFFSET;
}

// Streaming a texture in replaces its device texture, releasing the view other threads may be binding, so while a view
// is split nobody streams: main's and the workers' kicks run after the join, and the texture streams one view later.
static BYTE kick_stream_hook(BYTE *texture, BYTE *renderer) {
    Worker *worker = current_worker();
    if (!worker && GetCurrentThreadId() != split_thread) return stream_in(*(BYTE **)(texture + 0x20), renderer);
    Kicks *kicks = worker ? &worker->kicks : &main_kicks;
    if (kicks->count < MAX_KICKS) kicks->textures[kicks->count++] = texture;
    else kicks->dropped++;
    return 1;
}

static void run_kicks(Kicks *kicks, BYTE *renderer) {
    for (int i = 0; i < kicks->count; i++) stream_in(*(BYTE **)(kicks->textures[i] + 0x20), renderer);
    kicks->count = 0;
}

// With shared programs (commit mode 1) the commits' output arrays are shared, and the D3D runtime reads the list it is
// given twice; one thread commits at a time while a view is split.
static void lock_commits(void) {
    if (!TryAcquireSRWLockExclusive(&commit_lock)) {
        LONGLONG waiting = now_ticks();
        AcquireSRWLockExclusive(&commit_lock);
        InterlockedAdd64(&commit_wait_ticks, now_ticks() - waiting);
    }
}

static uint64_t bind_pass_state_hook(BYTE *pass, BYTE *renderer) {
    if (!split_thread || !overhaul_commit_mode) return original_bind_pass_state(pass, renderer);
    lock_commits();
    uint64_t result = original_bind_pass_state(pass, renderer);
    ReleaseSRWLockExclusive(&commit_lock);
    return result;
}

// The worker's copy of a slot in its current range, made from the engine's slot on its first set there; NULL when the
// range has none (the commit then reads the engine's slot, as a range's draws set every slot they rely on: its first
// group is forced) or the table is half full.
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
            if (!texture_copies_full_logged) {
                texture_copies_full_logged = TRUE;
                api->log_line("overhaul: a worker range set more than %d texture slots; the rest are set in the engine's", TEXTURE_COPIES / 2);
            }
            return NULL;
        }
        worker->texture_copy_count++;
        copy->slot = slot;
        copy->generation = worker->texture_generation;
        memcpy(copy->copy, slot, SHADER_TEXTURE_SIZE);
        return copy;
    }
}

// SHADER_TEXTURE::set (0x181ab6470), the one writer of texture slots (340 call sites): a worker's lands in its copy.
static void note_library_slot(const BYTE *slot) {
    uint32_t qword = (uint32_t)(slot - contexts.shared) / 8;
    if (contexts.volatile_qwords[qword] == 2) return;
    memset(contexts.volatile_qwords + qword, 1, SHADER_TEXTURE_SIZE / 8);
    contexts.volatile_qwords[qword] = 2;
    LONG index = InterlockedIncrement(&contexts.slot_count) - 1;
    if (index < MAX_LIBRARY_SLOTS) contexts.slot_offsets[index] = qword * 8;
}

static BYTE *texture_set_hook(BYTE *slot, BYTE *texture) {
    BYTE *passed = slot;
    // The commits read the engine's slots: a set in a thread's library copy is one of the engine's, and the copy's slot
    // follows it for what the thread reads back.
    ValueState *state = contexts_split ? drawing_values() : NULL;
    BOOL in_copy = state && state->library && slot >= state->library && slot + SHADER_TEXTURE_SIZE <= state->library + LIBRARY_SIZE;
    if (in_copy) slot = contexts.shared + (slot - state->library);
    if (contexts.volatile_qwords && slot >= contexts.shared && slot + SHADER_TEXTURE_SIZE <= contexts.shared + LIBRARY_SIZE) note_library_slot(slot);
    Worker *worker = overhaul_texture_copies ? current_worker() : NULL;
    TextureCopy *copy = worker && worker->texture_generation ? texture_copy(worker, slot, TRUE) : NULL;
    BYTE *set = copy ? copy->copy : slot;
    original_texture_set(set, texture);
    if (copy) worker->texture_sets++;
    if (in_copy) memcpy(passed, set, SHADER_TEXTURE_SIZE);
    return passed;
}

// Device::build_srv_list (0x181abee50), behind every stage's SRV commit, as the engine has it (each branch of its switch
// on the resource kind ends in the same store), reading a worker's copy of the slots it set.
static BYTE *build_srv_list_hook(BYTE *list, uint32_t *count, uint32_t *first, BYTE **cache) {
    Worker *worker = current_worker();
    if (!worker || !worker->texture_generation) return original_build_srv_list(list, count, first, cache);
    *count = *first = 0;
    BOOL prefix = TRUE;
    BYTE *entries = *(BYTE **)(list + 0x50), **views = *(BYTE ***)(list + 0x70);
    for (uint32_t i = 0; i < *(uint32_t *)(list + 0x4c); i++) {
        BYTE *slot = *(BYTE **)(entries + 16 * i);
        if (!slot) break;
        TextureCopy *copy = texture_copy(worker, slot, FALSE);
        BYTE *handle = *(BYTE **)((copy ? copy->copy : slot) + 0x18);
        if (prefix && handle == cache[i]) {
            *first = i + 1;
            continue;
        }
        prefix = FALSE;
        cache[i] = handle;
        views[(*count)++] = handle ? *(BYTE **)(handle + 0x50) : NULL;
    }
    return list + 0x68;
}

// The engine uploads a constant buffer only when its binding's version differs from the one last uploaded, tracked
// once per buffer: right for one timeline, wrong for several. A worker that skips an upload main made reads, on the
// GPU, whatever its command list follows (the leaf alpha-test constants of another mesh: tree shadows became sticks).
// A worker, and main while it records a deferred segment, uploads each buffer once per command list at each version and
// leaves the engine's record alone; once another timeline uploaded a buffer, main forgets the engine's record of it.
// Open addressing over entries of the current list; entries of earlier lists count as free.
static UploadedBuffer *list_slot(UploadedBuffer *table, BYTE *key, uint32_t list) {
    UploadedBuffer *slot = &table[((uintptr_t)key >> 4) * 0x9e3779b9u & (CB_TABLE_SIZE - 1)];
    while (slot->buffer && slot->buffer != key && slot->list == list) slot = slot + 1 == table + CB_TABLE_SIZE ? table : slot + 1;
    return slot;
}

// A list that uploaded into copies left the engine's buffers, and so its record, alone.
static void forget_uploads(UploadTracker *uploads) {
    if (!uploads->use_copies)
        for (uint32_t i = 0; i < uploads->uploaded_this_list_count; i++) *(uint32_t *)(uploads->uploaded_this_list[i] + 0x3c) = ~0u;
    uploads->uploaded_this_list_count = 0;
}

static CbCopy *copy_slot(CbCopy *table, uint32_t capacity, BYTE *buffer) {
    CbCopy *slot = &table[((uintptr_t)buffer >> 4) * 0x9e3779b9u & (capacity - 1)];
    while (slot->buffer && slot->buffer != buffer) slot = slot + 1 == table + capacity ? table : slot + 1;
    return slot;
}

// The timeline's own buffer for an engine constant buffer. Keyed by the engine's object, whose address a freed buffer
// can hand on to one of another size; a new copy holds nothing yet.
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

static void grow_clone(Worker *worker, BYTE *holder, uint32_t bytes);

// Mirrors ring_allocate's choice of map: each DISCARD has the driver hand out the holder's whole buffer.
static void count_rename(RingRenames *renames, BYTE *holder, uint32_t bytes) {
    uint32_t capacity = *(uint32_t *)(holder + 0x20), position = *(uint32_t *)(holder + 0x28);
    uint32_t stamp = *(uint32_t *)(*(BYTE **)(engine + ENGINE_RVA) + SCENE_UPDATE_STAMP_OFFSET);
    if (!bytes || bytes > capacity || (position + bytes < capacity && stamp == *(uint32_t *)(holder + 0x78))) return;
    renames->renames++;
    renames->bytes += *(uint32_t *)(holder + 0x24);
}

// Main's segments are deferred lists too, so each ring restarts with a DISCARD at its first allocation in a segment.
static BYTE *ring_allocate_hook(BYTE *holder, BYTE *span, uint64_t renderer, uint64_t bytes, uint64_t c, uint64_t d, uint64_t e) {
    Worker *worker = current_worker();
    if (worker) {
        if ((uint32_t)bytes > *(uint32_t *)(holder + 0x20)) grow_clone(worker, holder, (uint32_t)bytes);
        count_rename(&worker->renames[ab_mode()], holder, (uint32_t)bytes);
        return original_ring_allocate(holder, span, renderer, bytes, c, d, e);
    }
    if (GetCurrentThreadId() != render_thread) return original_ring_allocate(holder, span, renderer, bytes, c, d, e);
    if (overlap_active && GetCurrentThreadId() == split_thread) {
        UploadedBuffer *slot = list_slot(main_rings, holder, main_segment);
        if (slot->buffer != holder || slot->list != main_segment) {
            if (++main_segment_rings >= CB_TABLE_SIZE / 2) {
                api->log_line("overhaul: main allocated from more than %d rings in one segment", CB_TABLE_SIZE / 2);
                __builtin_trap();
            }
            *slot = (UploadedBuffer){holder, 0, main_segment};
            *(uint32_t *)(holder + 0x78) = *(uint32_t *)(*(BYTE **)(engine + ENGINE_RVA) + SCENE_UPDATE_STAMP_OFFSET) ^ 1;
        }
    }
    count_rename(&timers[ab_mode()].renames, holder, (uint32_t)bytes);
    return original_ring_allocate(holder, span, renderer, bytes, c, d, e);
}

// Device::upload_constant_buffers (0x181abecc0) as the engine has it, for main's range of a split view, uploading main's
// own values where it wrote the buffer in its range.
static BYTE *upload_main_values(BYTE *bindings, uint32_t *count, uint32_t *first, BYTE **cache) {
    BYTE *renderer = *(BYTE **)(engine + RENDER_DEVICE_RVA) + RENDERER_OFFSET;
    *count = *first = 0;
    BOOL prefix = TRUE, always = *(BYTE *)(engine + ALWAYS_UPDATE_CB_RVA);
    uint32_t binding_count = *(uint32_t *)(bindings + 4);
    BYTE **binding_list = *(BYTE ***)(bindings + 8);
    for (uint32_t i = 0; i < binding_count; i++) {
        BYTE *binding = binding_list[i];
        if (!binding) break;
        BYTE *buffer = *(BYTE **)(binding + 0x30);
        uint32_t version = *(uint32_t *)(binding + 0x38), bytes = *(uint32_t *)(buffer + 0x48);
        if (version != *(uint32_t *)(buffer + 0x3c) || always) {
            *(uint32_t *)(buffer + 0x3c) = version;
            void *destination;
            if (((MapBufferFn)method(buffer, 0x20))(buffer, renderer, 0, bytes, D3D11_MAP_WRITE_DISCARD, &destination, 0)) {
                memcpy(destination, value_source(buffer), bytes);
                ((UnmapBufferFn)method(buffer, 0x28))(buffer, renderer);
            }
        }
        if (prefix && buffer == cache[i]) {
            *first = i + 1;
            continue;
        }
        prefix = FALSE;
        cache[i] = buffer;
        (*(void ***)(bindings + 0x18))[(*count)++] = *(void **)(buffer + 0x30);
    }
    return bindings + 0x10;
}

static BYTE *upload_constant_buffers_hook(BYTE *bindings, uint64_t unused, uint32_t *count, uint32_t *first, BYTE **cache) {
    Worker *worker = current_worker();
    UploadTracker *uploads = worker ? &worker->uploads : overlap_active && GetCurrentThreadId() == split_thread ? &main_uploads : NULL;
    if (!uploads && !worker && drawing_values()) return upload_main_values(bindings, count, first, cache);
    if (!uploads) {
        if (GetCurrentThreadId() != render_thread) return original_upload_constant_buffers(bindings, unused, count, first, cache);
        LONGLONG started = now_ticks();
        BYTE *result = original_upload_constant_buffers(bindings, unused, count, first, cache);
        timers[ab_mode()].cb_upload += now_ticks() - started;
        return result;
    }
    BYTE *renderer = worker ? worker->renderer : *(BYTE **)(engine + RENDER_DEVICE_RVA) + RENDERER_OFFSET;
    ID3D11DeviceContext *context = *(ID3D11DeviceContext **)(renderer + 0x10);
    *count = *first = 0;
    BOOL prefix = TRUE, always = *(BYTE *)(engine + ALWAYS_UPDATE_CB_RVA);
    uint32_t binding_count = *(uint32_t *)(bindings + 4);
    BYTE **binding_list = *(BYTE ***)(bindings + 8);
    for (uint32_t i = 0; i < binding_count; i++) {
        BYTE *binding = binding_list[i];
        if (!binding) break;
        BYTE *buffer = *(BYTE **)(binding + 0x30);
        uint32_t version = *(uint32_t *)(binding + 0x38);
        // Half full keeps every probe short and guarantees it ends.
        if (uploads->uploaded_this_list_count >= CB_TABLE_SIZE / 2) {
            api->log_line("overhaul: %s uploaded more than %d constant buffers in one command list", worker ? "a worker" : "main", CB_TABLE_SIZE / 2);
            __builtin_trap();
        }
        uint32_t bytes = *(uint32_t *)(buffer + 0x48);
        ID3D11Buffer *engine_buffer = *(ID3D11Buffer **)(buffer + 0x30);
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
                if (((MapBufferFn)method(buffer, 0x20))(buffer, renderer, 0, bytes, D3D11_MAP_WRITE_DISCARD, &destination, 0)) {
                    memcpy(destination, value_source(buffer), bytes);
                    ((UnmapBufferFn)method(buffer, 0x28))(buffer, renderer);
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
        (*(void ***)(bindings + 0x18))[(*count)++] = bound;
    }
    return bindings + 0x10;
}

static ID3D11Query *create_query(D3D11_QUERY kind) {
    D3D11_QUERY_DESC desc = {kind, 0};
    ID3D11Query *query;
    HRESULT result = ID3D11Device_CreateQuery(device, &desc, &query);
    if (FAILED(result)) {
        api->log_line("overhaul: CreateQuery failed: 0x%08lx", (unsigned long)result);
        __builtin_trap();
    }
    return query;
}

static BOOL read_query(ID3D11DeviceContext *immediate, ID3D11Query *query, void *data, UINT size) {
    return ID3D11DeviceContext_GetData(immediate, (ID3D11Asynchronous *)query, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
}

static void add_statistics(D3D11_QUERY_DATA_PIPELINE_STATISTICS *sum, const D3D11_QUERY_DATA_PIPELINE_STATISTICS *add) {
    sum->IAVertices += add->IAVertices;
    sum->IAPrimitives += add->IAPrimitives;
    sum->VSInvocations += add->VSInvocations;
    sum->GSInvocations += add->GSInvocations;
    sum->GSPrimitives += add->GSPrimitives;
    sum->CInvocations += add->CInvocations;
    sum->CPrimitives += add->CPrimitives;
    sum->PSInvocations += add->PSInvocations;
    sum->HSInvocations += add->HSInvocations;
    sum->DSInvocations += add->DSInvocations;
    sum->CSInvocations += add->CSInvocations;
}

static double ticks_ms(UINT64 later, UINT64 earlier, UINT64 frequency) {
    return 1000.0 * (double)(INT64)(later - earlier) / frequency;
}

// A split view's lists in order: begins[k] before list k, begins[lists + 1] after the last.
static void read_gpu_detail(ID3D11DeviceContext *immediate, GpuFrame *frame, int v, UINT64 frequency, UINT64 middle, UINT64 end) {
    GpuDetail *detail = &gpu_detail[frame->mode][frame->type[v]][frame->cascade[v]];
    D3D11_QUERY_DATA_PIPELINE_STATISTICS main_statistics, list_statistics;
    int lists = frame->lists[v];
    UINT64 begins[MAX_WORKERS + 2], inside_begin[MAX_WORKERS + 1], inside_end[MAX_WORKERS + 1];
    BOOL ok = read_query(immediate, frame->main_statistics[v], &main_statistics, sizeof main_statistics) &&
              read_query(immediate, frame->list_statistics[v], &list_statistics, sizeof list_statistics);
    for (int k = 1; ok && k <= lists; k++) ok = read_query(immediate, frame->list_begin[v][k], &begins[k], sizeof begins[k]);
    if (ok && lists) ok = read_query(immediate, frame->lists_end[v], &begins[lists + 1], sizeof begins[lists + 1]);
    if (!ok) {
        gpu_detail_lost++;
        return;
    }
    add_statistics(&detail->statistics, &main_statistics);
    add_statistics(&detail->statistics, &list_statistics);
    if (!lists) return;
    detail->split_views++;
    detail->lists += lists;
    detail->gap_ms += ticks_ms(begins[1], middle, frequency);
    detail->lists_ms += ticks_ms(begins[lists + 1], begins[1], frequency);
    detail->after_ms += ticks_ms(end, begins[lists + 1], frequency);
    for (int k = 1; k <= lists; k++)
        if (!read_query(immediate, frame->inside_begin[v][k], &inside_begin[k], sizeof inside_begin[k]) ||
            !read_query(immediate, frame->inside_end[v][k], &inside_end[k], sizeof inside_end[k])) {
            gpu_inside_lost++;
            return;
        }
    for (int k = 1; k <= lists; k++) {
        detail->entry_ms += ticks_ms(inside_begin[k], begins[k], frequency);
        detail->inside_ms += ticks_ms(inside_end[k], inside_begin[k], frequency);
        detail->exit_ms += ticks_ms(begins[k + 1], inside_end[k], frequency);
    }
    detail->inside_lists += lists;
}

static void read_gpu_frame(ID3D11DeviceContext *immediate, GpuFrame *frame) {
    if (frame->mode < 0) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
    if (!read_query(immediate, frame->disjoint, &disjoint, sizeof disjoint) || disjoint.Disjoint) {
        gpu_lost_frames++;
        frame->mode = -1;
        return;
    }
    UINT64 starts[MAX_TIMED_VIEWS], middles[MAX_TIMED_VIEWS], ends[MAX_TIMED_VIEWS];
    for (int v = 0; v < frame->views; v++)
        if (!read_query(immediate, frame->start[v], &starts[v], sizeof starts[v]) || !read_query(immediate, frame->middle[v], &middles[v], sizeof middles[v]) ||
            !read_query(immediate, frame->end[v], &ends[v], sizeof ends[v])) {
            gpu_lost_frames++;
            frame->mode = -1;
            return;
        }
    double view_ms = 0, main_ms = 0, lists_ms = 0;
    for (int v = 0; v < frame->views; v++) {
        view_ms += ticks_ms(ends[v], starts[v], disjoint.Frequency);
        main_ms += ticks_ms(middles[v], starts[v], disjoint.Frequency);
        lists_ms += ticks_ms(ends[v], middles[v], disjoint.Frequency);
        GpuDetail *detail = &gpu_detail[frame->mode][frame->type[v]][frame->cascade[v]];
        detail->views++;
        detail->view_ms += ticks_ms(ends[v], starts[v], disjoint.Frequency);
        detail->main_ms += ticks_ms(middles[v], starts[v], disjoint.Frequency);
        if (frame->detail[v]) read_gpu_detail(immediate, frame, v, disjoint.Frequency, middles[v], ends[v]);
    }
    gpu_view_ms[frame->mode] += view_ms;
    gpu_timed_frames[frame->mode]++;
    if (frame->detailed) gpu_detail_frames[frame->mode]++;
    if (frame->mode) {
        gpu_main_ms += main_ms;
        gpu_lists_ms += lists_ms;
    }
    frame->mode = -1;
}

static void create_detail_queries(void) {
    for (int f = 0; f < GPU_TIMING_FRAMES; f++)
        for (int v = 0; v < MAX_TIMED_VIEWS; v++) {
            for (int k = 0; k <= MAX_WORKERS; k++) {
                gpu_frames[f].list_begin[v][k] = create_query(D3D11_QUERY_TIMESTAMP);
                gpu_frames[f].inside_begin[v][k] = create_query(D3D11_QUERY_TIMESTAMP);
                gpu_frames[f].inside_end[v][k] = create_query(D3D11_QUERY_TIMESTAMP);
            }
            gpu_frames[f].lists_end[v] = create_query(D3D11_QUERY_TIMESTAMP);
            gpu_frames[f].main_statistics[v] = create_query(D3D11_QUERY_PIPELINE_STATISTICS);
            gpu_frames[f].list_statistics[v] = create_query(D3D11_QUERY_PIPELINE_STATISTICS);
        }
    gpu_detail_ready = TRUE;
}

// Whether the driver builds command lists itself (without, the runtime records the calls and replays them at execute).
static void log_device(void) {
    D3D11_FEATURE_DATA_THREADING threading = {0};
    ID3D11Device_CheckFeatureSupport(device, D3D11_FEATURE_THREADING, &threading, sizeof threading);
    DXGI_ADAPTER_DESC desc = {0};
    LARGE_INTEGER driver = {0};
    IDXGIDevice *dxgi;
    if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi))) {
        IDXGIAdapter *adapter;
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter))) {
            IDXGIAdapter_GetDesc(adapter, &desc);
            IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &driver);
            IDXGIAdapter_Release(adapter);
        }
        IDXGIDevice_Release(dxgi);
    }
    api->log_line("overhaul: device: driver command lists %s, concurrent creates %s, feature level 0x%x, creation flags 0x%x | %ls, driver %u.%u.%u.%u, %llu MB "
                  "video memory",
                  threading.DriverCommandLists ? "yes" : "no", threading.DriverConcurrentCreates ? "yes" : "no", ID3D11Device_GetFeatureLevel(device),
                  ID3D11Device_GetCreationFlags(device), desc.Description, HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart),
                  LOWORD(driver.LowPart), (unsigned long long)(desc.DedicatedVideoMemory >> 20));
}

static void view_name(char *name, size_t size, int type, int cascade) {
    if (type == 1) snprintf(name, size, "SHADOW_MAP cascade %d", cascade);
    else snprintf(name, size, "%s (0x%x)", view_type_names[type] ? view_type_names[type] : "?", type);
}

static int describe_target(char *out, size_t size, ID3D11Resource *resource, UINT format, UINT first_slice, UINT slices) {
    D3D11_RESOURCE_DIMENSION dimension;
    ID3D11Resource_GetType(resource, &dimension);
    D3D11_TEXTURE2D_DESC texture = {0};
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)resource, &texture);
    ID3D11Resource_Release(resource);
    return snprintf(out, size, " format %u (texture %u) %ux%u, slices %u+%u of %u, %u samples;", format, texture.Format, texture.Width, texture.Height, first_slice,
                    slices, texture.ArraySize, texture.SampleDesc.Count);
}

// Once per split view: what its lists bind, by DXGI format number (shadow slices of one array lose more in a list).
static void log_targets(uint32_t type, uint32_t cascade, const TargetState *state) {
    static BOOL logged[VIEW_TYPES][STATS_CASCADES];
    if (logged[type][cascade]) return;
    logged[type][cascade] = TRUE;
    char line[1024], name[48];
    view_name(name, sizeof name, type, cascade);
    int length = snprintf(line, sizeof line, "overhaul: %s targets:", name);
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && length < (int)sizeof line; i++) {
        if (!state->rtvs[i]) continue;
        D3D11_RENDER_TARGET_VIEW_DESC desc;
        ID3D11RenderTargetView_GetDesc(state->rtvs[i], &desc);
        ID3D11Resource *resource;
        ID3D11RenderTargetView_GetResource(state->rtvs[i], &resource);
        BOOL array = desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        length += snprintf(line + length, sizeof line - length, " RTV%d", i);
        if (length < (int)sizeof line)
            length += describe_target(line + length, sizeof line - length, resource, desc.Format, array ? desc.Texture2DArray.FirstArraySlice : 0,
                                      array ? desc.Texture2DArray.ArraySize : 1);
    }
    if (state->dsv && length < (int)sizeof line) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc;
        ID3D11DepthStencilView_GetDesc(state->dsv, &desc);
        ID3D11Resource *resource;
        ID3D11DepthStencilView_GetResource(state->dsv, &resource);
        BOOL array = desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        length += snprintf(line + length, sizeof line - length, " DSV (flags 0x%x)", desc.Flags);
        if (length < (int)sizeof line)
            length += describe_target(line + length, sizeof line - length, resource, desc.Format, array ? desc.Texture2DArray.FirstArraySlice : 0,
                                      array ? desc.Texture2DArray.ArraySize : 1);
    }
    if (state->viewport_count && length < (int)sizeof line)
        snprintf(line + length, sizeof line - length, " viewport %.0fx%.0f at %.0f,%.0f, depth %.2f-%.2f", state->viewports[0].Width, state->viewports[0].Height,
                 state->viewports[0].TopLeftX, state->viewports[0].TopLeftY, state->viewports[0].MinDepth, state->viewports[0].MaxDepth);
    api->log_line("%s", line);
}

static void rotate_group_pools(ID3D11DeviceContext *immediate);

// GPU time of the views this module may split, in both modes: a timestamp before the view, after main's own range and
// after the replay; a disjoint query per frame. Results are read GPU_TIMING_FRAMES frames later.
static int gpu_view_begin(ID3D11DeviceContext *immediate, uint32_t type, uint32_t cascade) {
    if (!device) ID3D11DeviceContext_GetDevice(immediate, &device);
    if (!gpu_timing_ready) {
        for (int f = 0; f < GPU_TIMING_FRAMES; f++) {
            gpu_frames[f].disjoint = create_query(D3D11_QUERY_TIMESTAMP_DISJOINT);
            for (int v = 0; v < MAX_TIMED_VIEWS; v++) {
                gpu_frames[f].start[v] = create_query(D3D11_QUERY_TIMESTAMP);
                gpu_frames[f].middle[v] = create_query(D3D11_QUERY_TIMESTAMP);
                gpu_frames[f].end[v] = create_query(D3D11_QUERY_TIMESTAMP);
            }
            gpu_frames[f].mode = -1;
        }
        gpu_timing_ready = TRUE;
        log_device();
    }
    BOOL detail = overhaul_gpu_detail != 0;
    if (detail && !gpu_detail_ready) create_detail_queries();
    GpuFrame *frame = &gpu_frames[gpu_frame_number % GPU_TIMING_FRAMES];
    if (*frame_stamp != gpu_last_stamp) {
        if (gpu_last_stamp) ID3D11DeviceContext_End(immediate, (ID3D11Asynchronous *)frame->disjoint);
        gpu_last_stamp = *frame_stamp;
        frame = &gpu_frames[++gpu_frame_number % GPU_TIMING_FRAMES];
        read_gpu_frame(immediate, frame);
        rotate_group_pools(immediate);
        frame->views = 0;
        frame->mode = ab_mode();
        frame->detailed = FALSE;
        ID3D11DeviceContext_Begin(immediate, (ID3D11Asynchronous *)frame->disjoint);
    }
    if (frame->views == MAX_TIMED_VIEWS) return -1;
    // A group sample's statistics query would overlap the view's on the immediate context.
    detail = detail && !current_group_pool;
    int view = frame->views++;
    frame->type[view] = type < VIEW_TYPES ? type : VIEW_TYPES - 1;
    frame->cascade[view] = cascade < STATS_CASCADES ? cascade : STATS_CASCADES - 1;
    frame->detail[view] = detail;
    frame->lists[view] = 0;
    frame->detailed |= detail;
    ID3D11DeviceContext_End(immediate, (ID3D11Asynchronous *)frame->start[view]);
    if (detail) ID3D11DeviceContext_Begin(immediate, (ID3D11Asynchronous *)frame->main_statistics[view]);
    return view;
}

static void gpu_view_mark(ID3D11DeviceContext *immediate, int view, BOOL end) {
    if (view < 0) return;
    GpuFrame *frame = &gpu_frames[gpu_frame_number % GPU_TIMING_FRAMES];
    if (frame->detail[view]) {
        ID3D11DeviceContext_End(immediate, (ID3D11Asynchronous *)(end ? frame->list_statistics[view] : frame->main_statistics[view]));
        if (!end) ID3D11DeviceContext_Begin(immediate, (ID3D11Asynchronous *)frame->list_statistics[view]);
    }
    ID3D11DeviceContext_End(immediate, (ID3D11Asynchronous *)(end ? frame->end[view] : frame->middle[view]));
}

static void create_group_pools(void) {
    for (int p = 0; p < GROUP_POOLS; p++) {
        group_pools[p].samples = calloc(MAX_GROUP_SAMPLES, sizeof *group_pools[p].samples);
        if (!group_pools[p].samples) {
            api->log_line("overhaul: out of memory for group samples");
            __builtin_trap();
        }
        for (int i = 0; i < MAX_GROUP_SAMPLES; i++) group_pools[p].samples[i].query = create_query(D3D11_QUERY_PIPELINE_STATISTICS);
    }
    group_pools_ready = TRUE;
}

static GroupStat *group_stat(BYTE *item, uint32_t view_key, int mode) {
    uint64_t hash = ((uintptr_t)item >> 4) * 0x9e3779b97f4a7c15ull ^ (uint64_t)(view_key * 2 + mode) * 0xff51afd7ed558ccdull;
    for (uint32_t probe = 0; probe < GROUP_TABLE_SIZE; probe++) {
        GroupStat *stat = &group_stats[(hash + probe) & (GROUP_TABLE_SIZE - 1)];
        if (!stat->item) {
            stat->item = item;
            stat->view_key = view_key;
            stat->mode = mode;
            return stat;
        }
        if (stat->item == item && stat->view_key == view_key && stat->mode == (uint32_t)mode) return stat;
    }
    return NULL;
}

// Read once every query is done; a pool still unfinished 60 frames after its frame is dropped.
static void read_group_pool(ID3D11DeviceContext *immediate, GroupPool *pool) {
    static D3D11_QUERY_DATA_PIPELINE_STATISTICS data[MAX_GROUP_SAMPLES];
    uint32_t used = pool->used < MAX_GROUP_SAMPLES ? (uint32_t)pool->used : MAX_GROUP_SAMPLES;
    for (uint32_t i = 0; i < used; i++)
        if (!read_query(immediate, pool->samples[i].query, &data[i], sizeof data[i])) {
            if (gpu_frame_number > pool->frame + 60) {
                group_lost++;
                pool->pending = FALSE;
            }
            return;
        }
    group_frames[pool->mode]++;
    if ((uint32_t)pool->used > used) group_overflow += pool->used - used;
    for (uint32_t i = 0; i < used; i++) {
        const GroupSample *sample = &pool->samples[i];
        GroupStat *stat = group_stat(sample->item, sample->view_key, pool->mode);
        if (!stat) {
            group_dropped++;
            continue;
        }
        int on_worker = sample->range != 0;
        stat->effect_vtable = sample->effect_vtable;
        stat->flags |= sample->flags;
        stat->samples[on_worker]++;
        stat->forced[on_worker] += sample->forced;
        stat->first[on_worker] += sample->position == 0;
        stat->pixels[on_worker] += data[i].PSInvocations;
        stat->vertices[on_worker] += data[i].VSInvocations;
        stat->primitives[on_worker] += data[i].CPrimitives;
        stat->instances[on_worker] += sample->instances;
    }
    pool->pending = FALSE;
}

// At each new frame: read the pools old enough, and on every overhaul_group_every-th frame hand a free pool to the splits.
static void rotate_group_pools(ID3D11DeviceContext *immediate) {
    current_group_pool = NULL;
    if (!group_pools_ready) {
        if (!overhaul_group_stats) return;
        create_group_pools();
    }
    for (int p = 0; p < GROUP_POOLS; p++)
        if (group_pools[p].pending && gpu_frame_number >= group_pools[p].frame + GROUP_DELAY) read_group_pool(immediate, &group_pools[p]);
    LONG every = overhaul_group_every > 0 ? overhaul_group_every : 1;
    if (!overhaul_group_stats || gpu_frame_number % every) return;
    for (int p = 0; p < GROUP_POOLS; p++)
        if (!group_pools[p].pending) {
            group_pools[p].used = 0;
            group_pools[p].frame = gpu_frame_number;
            group_pools[p].mode = ab_mode();
            group_pools[p].pending = TRUE;
            current_group_pool = &group_pools[p];
            return;
        }
}

// The current frame when the view's lists should write gpudetail queries, or NULL.
static GpuFrame *gpu_detail_frame(int view) {
    if (view < 0) return NULL;
    GpuFrame *frame = &gpu_frames[gpu_frame_number % GPU_TIMING_FRAMES];
    return frame->detail[view] ? frame : NULL;
}

static __attribute__((noreturn)) void unexpected_recorder_call(void) {
    api->log_line("overhaul: the engine called a recorder method the range recorder does not implement");
    __builtin_trap();
}

static BYTE *job_item(const Job *job) {
    return job->kind == JOB_SINGLE ? *(BYTE **)(job->object + 0x10) : *(BYTE **)job->object;
}

static const uint32_t *job_ids(const Job *job) {
    if (job->kind == JOB_PACKET) return ((const Packet *)job->object)->ids;
    return job->kind == JOB_SINGLE ? *(const uint32_t **)(job->object + 0x20) : (const uint32_t *)(job->object + 0x08);
}

static uint32_t job_instances(const Job *job) {
    if (job->kind == JOB_PACKET) return ((const Packet *)job->object)->count;
    return *(uint32_t *)(job->object + (job->kind == JOB_SINGLE ? 0x1c : 0x408));
}

static uint32_t job_flags(const Job *job) {
    if (job->kind == JOB_PACKET) return ((const Packet *)job->object)->flags;
    return job->kind == JOB_SINGLE ? 0 : *(uint32_t *)(job->object + 0x40c);
}

static CloneBuffers *clone_buffers(BYTE *vertex_buffer) {
    return (CloneBuffers *)(vertex_buffer + VERTEX_BUFFER_SIZE);
}

static BYTE staging_map(BYTE *vertex_buffer, BYTE *renderer, uint32_t offset, uint32_t bytes, uint32_t type, void **out, uint32_t flags) {
    (void)renderer; (void)type; (void)flags;
    CloneBuffers *buffers = clone_buffers(vertex_buffer);
    // The ring restarts at 0 when it wraps; data written before the wrap would be overwritten before its draws ran.
    if (offset == 0 && buffers->used) buffers->wraps++;
    if (offset + bytes > buffers->used) buffers->used = offset + bytes;
    *out = buffers->staging + offset;
    return 1;
}

static void staging_unmap(BYTE *vertex_buffer, BYTE *renderer) {
    (void)vertex_buffer; (void)renderer;
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

// The clone's buffer and capacity for the list's upload mode: mapping writes into the small buffer, the staging modes keep
// the original's size and upload once a list. A stamp unlike the scene's makes the first map a DISCARD, as a deferred
// context requires.
static void select_clone_buffer(Worker *worker, BYTE *copy) {
    BYTE *buffer_copy = copy + RING_HOLDER_SIZE;
    CloneBuffers *buffers = clone_buffers(buffer_copy);
    *(uint32_t *)(copy + 0x20) = *(uint32_t *)(copy + 0x24) = *(uint32_t *)(buffer_copy + 0x10) = worker->upload_mode ? buffers->full_bytes : buffers->small_bytes;
    *(void **)buffer_copy = worker->upload_mode ? (void *)buffers->vtable : (void *)(engine + VERTEX_BUFFER_VTABLE_RVA);
    *(ID3D11Buffer **)(buffer_copy + 0x28) =
        worker->upload_mode == 2 ? buffers->device_local[buffers->turn] : worker->upload_mode ? buffers->dynamic : buffers->small;
    *(uint32_t *)(copy + 0x78) = *(uint32_t *)(*(BYTE **)(engine + ENGINE_RVA) + SCENE_UPDATE_STAMP_OFFSET) ^ 1;
}

// A request past a small clone's capacity grows it; the geometry ring's reach about 1 MB.
static void grow_clone(Worker *worker, BYTE *holder, uint32_t bytes) {
    if (worker->upload_mode) return;
    for (int i = 0; i < worker->clone_count; i++) {
        if (worker->clones[i].copy != holder) continue;
        CloneBuffers *buffers = clone_buffers(holder + RING_HOLDER_SIZE);
        uint32_t size = buffers->small_bytes;
        while (size < bytes) size *= 2;
        D3D11_BUFFER_DESC desc;
        ID3D11Buffer_GetDesc(buffers->dynamic, &desc);
        ID3D11Buffer_Release(buffers->small);
        buffers->small = create_clone_buffer(desc, size);
        buffers->small_bytes = size;
        buffers->grows++;
        select_clone_buffer(worker, holder);
        return;
    }
}

static BYTE *clone_holder(Worker *worker, BYTE *holder) {
    void *vtable = *(void **)holder;
    if (vtable != engine + INSTANCE_RING_VTABLE_RVA && vtable != engine + GEOMETRY_RING_VTABLE_RVA) return holder;
    for (int i = 0; i < worker->clone_count; i++)
        if (worker->clones[i].original == holder) return worker->clones[i].copy;
    BYTE *buffer = *(BYTE **)(holder + 0x18);
    ID3D11Buffer *d3d_buffer = buffer ? *(ID3D11Buffer **)(buffer + 0x28) : NULL;
    if (worker->clone_count == MAX_CLONES || !d3d_buffer) {
        worker->shared_holders++;
        return holder;
    }
    D3D11_BUFFER_DESC desc;
    ID3D11Buffer_GetDesc(d3d_buffer, &desc);
    ID3D11Buffer *own;
    HRESULT result = ID3D11Device_CreateBuffer(device, &desc, NULL, &own);
    if (FAILED(result)) {
        api->log_line("overhaul: worker %d: CreateBuffer for a ring clone failed: 0x%08lx", worker->number, (unsigned long)result);
        worker->shared_holders++;
        return holder;
    }
    D3D11_BUFFER_DESC device_local_desc = {desc.ByteWidth, D3D11_USAGE_DEFAULT, desc.BindFlags, 0, 0, 0};
    ID3D11Buffer *device_local[DEVICE_LOCAL_COPIES];
    for (int i = 0; i < DEVICE_LOCAL_COPIES; i++) {
        result = ID3D11Device_CreateBuffer(device, &device_local_desc, NULL, &device_local[i]);
        if (FAILED(result)) {
            api->log_line("overhaul: worker %d: CreateBuffer for a video-memory ring clone failed: 0x%08lx", worker->number, (unsigned long)result);
            __builtin_trap();
        }
    }
    BYTE *copy = _aligned_malloc(RING_HOLDER_SIZE + VERTEX_BUFFER_SIZE + sizeof(CloneBuffers), 16), *buffer_copy = copy + RING_HOLDER_SIZE;
    memcpy(copy, holder, RING_HOLDER_SIZE);
    memcpy(buffer_copy, buffer, VERTEX_BUFFER_SIZE);
    CloneBuffers *buffers = clone_buffers(buffer_copy);
    memset(buffers, 0, sizeof *buffers);
    memcpy(buffers->vtable, engine + VERTEX_BUFFER_VTABLE_RVA, sizeof buffers->vtable);
    buffers->vtable[0x20 / 8] = (void *)staging_map;
    buffers->vtable[0x28 / 8] = (void *)staging_unmap;
    buffers->staging = malloc(desc.ByteWidth);
    buffers->dynamic = own;
    buffers->full_bytes = desc.ByteWidth;
    buffers->small_bytes = desc.ByteWidth < RING_CLONE_BYTES ? desc.ByteWidth : RING_CLONE_BYTES;
    buffers->small = create_clone_buffer(desc, buffers->small_bytes);
    memcpy(buffers->device_local, device_local, sizeof device_local);
    if (!buffers->staging) {
        api->log_line("overhaul: out of memory for %u bytes of ring staging", desc.ByteWidth);
        __builtin_trap();
    }
    *(DWORD *)(buffer_copy + 0x30) = 0;
    *(BYTE **)(copy + 0x18) = buffer_copy;
    // The holder's capacity can run ahead of its buffer while a grow is pending; the clone holds exactly its buffer.
    *(uint32_t *)(copy + 0x28) = 0;
    memset(copy + 0x58, 0, 0x70 - 0x58);
    *(BYTE *)(copy + 0x70) = 0;
    select_clone_buffer(worker, copy);
    worker->clones[worker->clone_count++] = (Clone){holder, copy};
    return copy;
}

static size_t chunker_size(BYTE *chunker) {
    void *vtable = *(void **)chunker;
    if (vtable == engine + CHUNKER_VTABLE_RVA) return CHUNKER_SIZE;
    for (size_t i = 0; i < sizeof derived_chunker_vtable_rvas / sizeof *derived_chunker_vtable_rvas; i++)
        if (vtable == engine + derived_chunker_vtable_rvas[i]) return DERIVED_CHUNKER_SIZE;
    api->log_line("overhaul: a draw item's chunker has vtable %p, a class this module does not know the size of", vtable);
    __builtin_trap();
}

// A group-instance mesh as one view's draws must see it: record writes the view's instance count (+0x1f0) and pass bit
// (+0x1f4) into the shared mesh (VegetationNode::record 0x181b6cb00), and the chunker's size and the gatherer read them
// while the next view may already be recording. The view also holds the thread's own chunker (+0x1e8), and the chunker's
// mesh (+0x28) points back at it.
static BOOL bind_group_view(BYTE *view, BYTE *chunker, const Job *job) {
    BYTE *mesh = *(BYTE **)(job_item(job) + 0x10);
    if (!overhaul_group_gather || *(BYTE **)mesh != engine + GROUP_INSTANCE_MESH_VTABLE_RVA) return FALSE;
    memcpy(view, mesh, GROUP_MESH_VIEW_SIZE);
    *(BYTE **)(view + 0x1e8) = chunker;
    *(uint32_t *)(view + 0x1f0) = job->group_count;
    *(uint32_t *)(view + 0x1f4) = job->group_bit;
    *(BYTE **)(chunker + 0x28) = view;
    return TRUE;
}

static void prepare_private_item(Worker *worker, const Job *job) {
    BYTE *item = job_item(job);
    memcpy(worker->item, item, DRAW_ITEM_SIZE);
    BYTE *chunker = *(BYTE **)(item + 0x18);
    memcpy(worker->chunker, chunker, chunker_size(chunker));
    *(BYTE **)(worker->item + 0x18) = worker->chunker;
    worker->group_mesh_of = bind_group_view(worker->group_mesh, worker->chunker, job) ? *(BYTE **)(item + 0x10) : NULL;
    BYTE *streams = *(BYTE **)(item + 0x20);
    if (streams) {
        memcpy(worker->streams, streams, STREAMS_SIZE);
        uint32_t count = *(uint32_t *)(streams + 0x08);
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
        BYTE **instance_holder = (BYTE **)(worker->streams + 0x10);
        if (*instance_holder) *instance_holder = clone_holder(worker, *instance_holder);
        *(BYTE **)(worker->item + 0x20) = worker->streams;
    }
    BYTE *index_stream = *(BYTE **)(item + 0x28);
    if (index_stream) {
        memcpy(worker->index_stream, index_stream, INDEX_STREAM_SIZE);
        *(BYTE **)(worker->item + 0x28) = worker->index_stream;
    }
}

// Main's range draws the engine's items, except a group-instance job, which gets a private item and chunker.
static void prepare_main_group_item(const Job *job) {
    main_group.mesh_of = NULL;
    BYTE *item = job_item(job), *mesh = *(BYTE **)(item + 0x10), *chunker = *(BYTE **)(item + 0x18);
    if (!overhaul_group_gather || *(BYTE **)mesh != engine + GROUP_INSTANCE_MESH_VTABLE_RVA || chunker_size(chunker) != DERIVED_CHUNKER_SIZE) return;
    memcpy(main_group.item, item, DRAW_ITEM_SIZE);
    memcpy(main_group.chunker, chunker, DERIVED_CHUNKER_SIZE);
    *(BYTE **)(main_group.item + 0x18) = main_group.chunker;
    bind_group_view(main_group.view, main_group.chunker, job);
    main_group.mesh_of = mesh;
    main_group.job = job;
}

static BYTE *group_view_for(BYTE *mesh) {
    Worker *worker = current_worker();
    if (worker) return worker->group_mesh_of == mesh ? worker->group_mesh : NULL;
    return main_group.mesh_of == mesh ? main_group.view : NULL;
}

// The gatherer (rcx) reads only its mesh (+0x08) and, through it, the fields up to GROUP_MESH_VIEW_SIZE; it writes none.
static uint64_t group_gather_hook(BYTE *gatherer, BYTE *mapped, const uint32_t *ids) {
    BYTE *view = group_view_for(*(BYTE **)(gatherer + 8));
    if (!view) return original_group_gather(gatherer, mapped, ids);
    void *fake[2] = {*(void **)gatherer, view};
    return original_group_gather((BYTE *)fake, mapped, ids);
}

// Program objects keep the lists their commits build (CBs, samplers, SRVs, UAVs) in output arrays inside the object, and
// the D3D runtime reads the list it is given twice (once to set up each view's per-context record, once to bind): two
// threads committing one program would hand each other's lists to the runtime mid-call. A worker's renderer binds
// private copies instead, each with its own output arrays; the binding lists and the D3D shader stay shared.
static const struct {
    uint32_t output, bindings;
} program_outputs[] = {{0x18, 0x08}, {0x40, 0x30}, {0x70, 0x50}, {0x80, 0x60}, {0x90, 0x60}};

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
        if (worker->programs[slot].original == program) {
            if (overhaul_program_refresh) fill_program_clone(&worker->programs[slot]);
            return worker->programs[slot].copy;
        }
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
    ProgramClone clone = {program, malloc(PROGRAM_SIZE), {0}};
    if (!clone.copy) {
        api->log_line("overhaul: out of memory for a program copy");
        __builtin_trap();
    }
    memset(clone.copy, 0, PROGRAM_SIZE);
    ProgramClone *inserted = insert_program(worker, clone);
    fill_program_clone(inserted);
    return inserted->copy;
}

#define SET_SHADER_WRAPPER(stage)                                                                                               \
    static uint64_t set_shader_##stage(BYTE *renderer, BYTE *program) {                                                         \
        return original_set_shaders[stage](renderer, program && !overhaul_commit_mode ? program_clone(current_worker(), program) : program); \
    }
SET_SHADER_WRAPPER(0)
SET_SHADER_WRAPPER(1)
SET_SHADER_WRAPPER(2)
SET_SHADER_WRAPPER(3)
SET_SHADER_WRAPPER(4)
SET_SHADER_WRAPPER(5)
// RENDERER slots that bind a stage's program: VS, GS, HS, DS, PS, CS.
static const uint32_t set_shader_slots[STAGES] = {0x20, 0x40, 0x60, 0x80, 0xa0, 0xc0};
static void *const set_shader_wrappers[STAGES] = {(void *)set_shader_0, (void *)set_shader_1, (void *)set_shader_2,
                                                  (void *)set_shader_3, (void *)set_shader_4, (void *)set_shader_5};

static BOOL job_forced(const RangeRecorder *recorder) {
    if (overhaul_force_all || recorder->current_index == recorder->forced_job) return TRUE;
    return overhaul_force_recorders &&
           (recorder->current_index == recorder->first_job || recorder->jobs[recorder->current_index - 1].recorder != recorder->current->recorder);
}

// Only a Variant node drawing without instancing writes the bone palette; forced on an instanced one, the writer would
// read a pose the item need not have.
static BOOL draws_without_instancing(const Job *job) {
    BYTE *effect = *(BYTE **)(job_item(job) + 0x10);
    void *vtable = *(void **)effect;
    if (vtable == engine + VARIANT_WEIGHTED_B_VTABLE_RVA) effect = *(BYTE **)(effect + 0x100);
    else if (vtable != engine + VARIANT_WEIGHTED_VTABLE_RVA) return FALSE;
    if (!effect) return FALSE;
    BYTE *node = *(BYTE **)(effect + 0xa0);
    return node && !(node[0x2e0] && !node[0x2e2] && *(uint32_t *)(node + 0x2d4));
}

static void end_group_sample(RangeRecorder *recorder) {
    if (!recorder->open) return;
    ID3D11DeviceContext_End(recorder->context, (ID3D11Asynchronous *)recorder->open->query);
    recorder->open = NULL;
}

// The engine draws a group between this next and the following one.
static void begin_group_sample(RangeRecorder *recorder) {
    GroupPool *pool = recorder->pool;
    LONG index = InterlockedIncrement(&pool->used) - 1;
    if (index >= MAX_GROUP_SAMPLES) return;
    GroupSample *sample = &pool->samples[index];
    const Job *job = recorder->current;
    BYTE *item = job_item(job);
    sample->item = item;
    sample->effect_vtable = *(BYTE **)*(BYTE **)(item + 0x10);
    sample->view_key = recorder->view_key;
    sample->range = recorder->range_index;
    sample->position = recorder->current_index - recorder->range_first;
    sample->flags = job_flags(job);
    sample->instances = job_instances(job);
    sample->forced = job_forced(recorder);
    ID3D11DeviceContext_Begin(recorder->context, (ID3D11Asynchronous *)sample->query);
    recorder->open = sample;
}

static BYTE range_next(RangeRecorder *recorder) {
    if (recorder->pool) end_group_sample(recorder);
    if (recorder->next_job == recorder->end_job) return 0;
    recorder->current_index = recorder->next_job++;
    recorder->current = &recorder->jobs[recorder->current_index];
    if (recorder->worker) prepare_private_item(recorder->worker, recorder->current);
    else prepare_main_group_item(recorder->current);
    if (recorder->pool) begin_group_sample(recorder);
    return 1;
}

static BYTE *range_item(RangeRecorder *recorder) {
    if (recorder->worker) return recorder->worker->item;
    return main_group.mesh_of && main_group.job == recorder->current ? main_group.item : job_item(recorder->current);
}

static const uint32_t *range_begin(RangeRecorder *recorder) {
    return job_ids(recorder->current);
}

static const uint32_t *range_end(RangeRecorder *recorder) {
    return job_ids(recorder->current) + job_instances(recorder->current);
}

static uint32_t range_flags(RangeRecorder *recorder) {
    uint32_t flags = job_flags(recorder->current);
    if (!job_forced(recorder)) return flags;
    flags |= overhaul_forced_flags;
    if (overhaul_force_bones && draws_without_instancing(recorder->current)) flags |= 0x01;
    return flags;
}

static void *const range_recorder_vtable[RECORDER_SLOTS] = {
    (void *)unexpected_recorder_call, (void *)unexpected_recorder_call, (void *)unexpected_recorder_call, (void *)range_next,
    (void *)range_item, (void *)range_begin, (void *)range_end, (void *)range_flags, (void *)unexpected_recorder_call,
    (void *)unexpected_recorder_call};

// Weighing groups by their instances balanced worse.
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
    BYTE *effect = *(BYTE **)(job_item(job) + 0x10);
    if (*(BYTE **)effect == engine + GROUP_INSTANCE_MESH_VTABLE_RVA) {
        job->group_count = *(uint32_t *)(effect + 0x1f0);
        job->group_bit = *(uint32_t *)(effect + 0x1f4);
    }
}

static size_t packet_size(uint32_t count) {
    return (sizeof(Packet) + count * sizeof(uint32_t) + 7) & ~(size_t)7;
}

static void copy_packets(ViewSlot *slot, Timers *timer) {
    Job *jobs = slot->jobs;
    size_t bytes = 0;
    for (uint32_t i = 0; i < slot->job_count; i++) bytes += packet_size(job_instances(&jobs[i]));
    if (bytes > slot->packet_capacity) {
        free(slot->packet_arena);
        slot->packet_capacity = bytes * 2;
        slot->packet_arena = malloc(slot->packet_capacity);
        if (!slot->packet_arena) {
            api->log_line("overhaul: out of memory for %zu bytes of packets", slot->packet_capacity);
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
    timer->packet_bytes += bytes;
}

// Groups the view would draw, or -1 for a recorder kind this module does not know. Counted before any sort: the
// program sort is not a strict weak ordering, so a view handed back to the original must not be sorted twice.
static int64_t count_jobs(BYTE *context) {
    uint32_t bucket_count = *(uint32_t *)(context + 0x1c);
    BYTE *recorder_vectors = *(BYTE **)(context + 0x40);
    uint32_t *record_counts = *(uint32_t **)(context + 0x70);
    int64_t count = 0;
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++) {
        if (!record_counts[bucket]) continue;
        BYTE *vector = recorder_vectors + 16 * bucket;
        BYTE **recorders = *(BYTE ***)(vector + 8);
        for (uint32_t i = 0; i < *(uint32_t *)(vector + 4); i++) {
            void *vtable = *(void **)recorders[i];
            if (vtable == engine + SINGLE_RECORDER_VTABLE_RVA) {
                count++;
            } else if (vtable == engine + BATCH_RECORDER_VTABLE_RVA) {
                count += *(uint32_t *)(recorders[i] + 0x20);
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
static void snapshot_jobs(ViewSlot *slot, BYTE *context) {
    slot->job_count = 0;
    uint32_t bucket_count = *(uint32_t *)(context + 0x1c);
    BYTE *sort_flags = *(BYTE **)(context + 0x30), *recorder_vectors = *(BYTE **)(context + 0x40);
    uint32_t *record_counts = *(uint32_t **)(context + 0x70);
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++) {
        if (!record_counts[bucket]) continue;
        BYTE *vector = recorder_vectors + 16 * bucket;
        uint32_t count = *(uint32_t *)(vector + 4);
        BYTE **recorders = *(BYTE ***)(vector + 8);
        if (sort_flags[bucket]) sort_bucket(recorders, recorders + count, count, 0);
        for (uint32_t i = 0; i < count; i++) {
            BYTE *recorder = recorders[i];
            void *vtable = *(void **)recorder;
            if (vtable == engine + SINGLE_RECORDER_VTABLE_RVA) {
                append_job(slot, recorder, bucket, JOB_SINGLE, recorder);
            } else {
                uint32_t used = *(uint32_t *)(recorder + 0x20);
                BYTE **groups = *(BYTE ***)(recorder + 0x18);
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

// Begin callbacks run for every bucket, empty ones too, before its draws. Each range runs those of the buckets it
// draws, a bucket split between two ranges in both, and the empty buckets before its first draw.
static void build_range_context(ViewSlot *slot, Range *range, BYTE *context, Worker *worker) {
    const Job *jobs = slot->jobs;
    uint32_t bucket_count = *(uint32_t *)(context + 0x1c);
    void **callbacks = *(void ***)(context + 0x80);
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
                                    .current_index = UINT32_MAX,
                                    .first_job = first,
                                    .range_first = range->first_job,
                                    .range_index = (uint32_t)(range - slot->ranges),
                                    .view_key = main_view_type * STATS_CASCADES + main_view_cascade,
                                    .context = worker ? worker->context : immediate_context,
                                    .pool = slot->group_pool};
        range->recorder_pointers[bucket] = recorder;
        range->recorders[bucket] = (RecorderVector){1, 1, &range->recorder_pointers[bucket]};
        range->record_counts[bucket] = end - first;
        first = end;
    }
    memset(range->context, 0, CONTEXT_SIZE);
    *(uint32_t *)(range->context + 0x1c) = bucket_count;
    *(BYTE **)(range->context + 0x30) = zero_sort_flags;
    *(RecorderVector **)(range->context + 0x40) = range->recorders;
    *(uint32_t **)(range->context + 0x70) = range->record_counts;
    *(void ***)(range->context + 0x80) = range->callbacks;
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

// Cold, like the engine's own renderer after end_frame's reset_state, but keeping the target record and saved targets.
static void make_cold(BYTE *renderer) {
    memset(renderer + 0x20, 0, RENDERER_TARGETS_START - 0x20);
    memset(renderer + RENDERER_TARGETS_END, 0, RENDERER_SIZE - RENDERER_TARGETS_END);
    *(uint32_t *)(renderer + 0x90) = *(uint32_t *)(renderer + 0x94) = ~0u;
    for (int slot = 0; slot < 2; slot++) *(uint64_t *)(renderer + 0xa0 + 16 * slot) = ~0ull;
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
    worker->uploads.list_number++;
    worker->uploads.use_copies = overhaul_cb_copies;
    *(ID3D11DeviceContext **)(worker->renderer + 0x10) = worker->context;
    *(void **)(worker->renderer + 0x18) = NULL;
    make_cold(worker->renderer);
    worker->upload_mode = overhaul_instance_upload;
    for (int i = 0; i < worker->clone_count; i++) {
        CloneBuffers *buffers = clone_buffers(worker->clones[i].copy + RING_HOLDER_SIZE);
        buffers->turn = (buffers->turn + 1) % DEVICE_LOCAL_COPIES;
        buffers->used = 0;
        select_clone_buffer(worker, worker->clones[i].copy);
    }
    bind_targets(worker->context, &slot->targets);
}

static void run_range(Worker *worker, ViewSlot *slot, Range *range) {
    LONGLONG started = now_ticks();
    worker->view_type = slot->view_type;
    GpuFrame *timed = slot->timed_frame;
    int k = (int)(range - slot->ranges);
    if (timed) ID3D11DeviceContext_End(worker->context, (ID3D11Asynchronous *)timed->inside_begin[slot->timed_view][k]);
    begin_command_list(worker, slot);
    range->result = original_execute(range->context, worker->renderer, slot->view);
    worker->texture_generation = 0;
    worker->values.drawing = FALSE;
    if (timed) ID3D11DeviceContext_End(worker->context, (ID3D11Asynchronous *)timed->inside_end[slot->timed_view][k]);
    range->finish_result = ID3D11DeviceContext_FinishCommandList(worker->context, FALSE, &range->list);
    range->upload_list = NULL;
    BOOL uploads = FALSE;
    if (worker->upload_mode && SUCCEEDED(range->finish_result)) {
        for (int i = 0; i < worker->clone_count; i++) {
            CloneBuffers *buffers = clone_buffers(worker->clones[i].copy + RING_HOLDER_SIZE);
            if (!buffers->used) continue;
            uploads = TRUE;
            if (worker->upload_mode == 2) {
                D3D11_BOX box = {0, 0, 0, buffers->used, 1, 1};
                ID3D11DeviceContext_UpdateSubresource(worker->context, (ID3D11Resource *)buffers->device_local[buffers->turn], 0, &box, buffers->staging, 0, 0);
                continue;
            }
            D3D11_MAPPED_SUBRESOURCE mapped;
            HRESULT result = ID3D11DeviceContext_Map(worker->context, (ID3D11Resource *)buffers->dynamic, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(result)) {
                api->log_line("overhaul: worker %d: Map of the ring clone for its upload list failed: 0x%08lx", worker->number, (unsigned long)result);
                __builtin_trap();
            }
            memcpy(mapped.pData, buffers->staging, buffers->used);
            ID3D11DeviceContext_Unmap(worker->context, (ID3D11Resource *)buffers->dynamic, 0);
        }
        if (uploads) range->finish_result = ID3D11DeviceContext_FinishCommandList(worker->context, FALSE, &range->upload_list);
    }
    // Main records on meanwhile and reads only its own upload record until the drain.
    if (slot->overlapped) forget_uploads(&worker->uploads);
    worker->range_ticks = now_ticks() - started;
    worker->busy_ticks += worker->range_ticks;
}

// get_renderer answers a worker with its own renderer, and a task's D3D work belongs on the immediate context.
static void run_task(Worker *worker) {
    LONGLONG started = now_ticks();
    TlsSetValue(tls_index, NULL);
    worker->task(worker->task_argument);
    TlsSetValue(tls_index, worker);
    worker->task_ticks[ab_mode()] += now_ticks() - started;
    worker->task = NULL;
}

static DWORD WINAPI worker_main(LPVOID parameter) {
    Worker *worker = parameter;
    TlsSetValue(tls_index, worker);
    // Main may kick this worker's first range before the thread runs, so it starts from the generation main saw.
    LONG seen = worker->started_at_generation;
    for (;;) {
        for (int spin = 0; generation == seen && spin < WORKER_SPIN_ROUNDS; spin++) _mm_pause();
        while (generation == seen) WaitOnAddress(&generation, &seen, sizeof seen, INFINITE);
        seen = generation;
        if (worker->task) {
            run_task(worker);
            continue;
        }
        while (worker->range_tasks_done != worker->range_tasks_pushed) {
            RangeTask task = worker->range_tasks[worker->range_tasks_done % MAX_SLOTS];
            run_range(worker, task.slot, task.range);
            InterlockedIncrement(&worker->range_tasks_done);
            InterlockedDecrement(&task.slot->remaining);
        }
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
    SetThreadDescriptionFn describe = (SetThreadDescriptionFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    while (worker_count < wanted) {
        Worker *worker = _aligned_malloc(sizeof *worker, 64);
        memset(worker, 0, sizeof *worker);
        worker->number = worker_count + 1;
        worker->started_at_generation = generation;
        memcpy(worker->vtable, engine + RENDERER_VTABLE_RVA, sizeof worker->vtable);
        for (int stage = 0; stage < STAGES; stage++) worker->vtable[set_shader_slots[stage] / 8] = set_shader_wrappers[stage];
        worker->program_capacity = PROGRAM_TABLE_INITIAL;
        worker->programs = calloc(worker->program_capacity, sizeof *worker->programs);
        HRESULT result = ID3D11Device_CreateDeferredContext(device, 0, &worker->context);
        if (FAILED(result)) {
            static char error[160];
            snprintf(error, sizeof error, "CreateDeferredContext failed: 0x%08lx%s", (unsigned long)result,
                     result == DXGI_ERROR_INVALID_CALL ? " (a single-threaded device: set multithreaded_device=1 in s2prof.ini)" : "");
            worker_error = error;
            api->log_line("overhaul: no more workers: %s", worker_error);
            _aligned_free(worker);
            return FALSE;
        }
        HANDLE thread = CreateThread(NULL, 0, worker_main, worker, 0, NULL);
        if (!thread) {
            worker_error = "CreateThread failed";
            api->log_line("overhaul: no more workers: %s", worker_error);
            return FALSE;
        }
        if (describe) {
            wchar_t name[32];
            swprintf(name, 32, L"overhaul worker %d", worker->number);
            describe(thread, name);
        }
        CloseHandle(thread);
        workers[worker_count++] = worker;
    }
    return TRUE;
}

static void kick_task(Worker *worker, TaskFn task, BYTE *argument) {
    worker->task_argument = argument;
    worker->task = task;
    InterlockedIncrement(&generation);
    WakeByAddressAll((void *)&generation);
}

static void sort_list_c(BYTE *list) {
    uint32_t count = *(uint32_t *)(list + 0x0c);
    BYTE *items = *(BYTE **)(list + 0x10);
    ((QueueSortFn)(engine + QUEUE_SORT_RVA))(items, items + (size_t)count * RENDER_ITEM_SIZE, count, 0);
}

// Runs on the engine's list B task, which then goes straight on to list B.
static void fill_instance_textures_hook(BYTE *node) {
    if (prepare_split_active) kick_task(workers[0], original_fill_instance_textures, node);
    else original_fill_instance_textures(node);
}

// compute_keys merges list C, and record_list_c sorts it unchanged until the frame's lists are cleared.
static void compute_keys_hook(BYTE *node, BYTE *list) {
    original_compute_keys(node, list);
    if (prepare_split_active && __builtin_return_address(0) == engine + LIST_C_KEYS_RETURN_RVA) kick_task(workers[1], sort_list_c, list);
}

static uint32_t scene_stamp(void) {
    return *(uint32_t *)(*(BYTE **)(engine + ENGINE_RVA) + SCENE_UPDATE_STAMP_OFFSET);
}

static ID3D11DeviceContext *engine_immediate_context(void) {
    return *(ID3D11DeviceContext **)(*(BYTE **)(engine + RENDER_DEVICE_RVA) + RENDERER_OFFSET + 0x10);
}

// The node's lists: pose pairs at +0x270, A, B and C at +0xf8, +0x160 and +0x1c8 (list objects; body at +0x08:
// count +0x04, items +0x08, needs_merge +0x30).
static void sort_render_list(BYTE *body) {
    uint32_t count = *(uint32_t *)(body + 0x04);
    BYTE *items = *(BYTE **)(body + 0x08);
    ((RenderSortFn)(engine + RENDER_SORT_RVA))(items, items + (size_t)count * RENDER_ITEM_SIZE, count, 0);
}

static SortKey *radix_sort_keys(SortKey *keys, SortKey *scratch, uint32_t count) {
    uint32_t histogram[8][256];
    memset(histogram, 0, sizeof histogram);
    for (uint32_t i = 0; i < count; i++)
        for (int digit = 0; digit < 8; digit++) histogram[digit][(keys[i].key >> (digit * 8)) & 0xff]++;
    for (int digit = 0; digit < 8; digit++) {
        uint32_t *buckets = histogram[digit];
        if (buckets[(keys[0].key >> (digit * 8)) & 0xff] == count) continue;
        uint32_t offset = 0;
        for (int bucket = 0; bucket < 256; bucket++) {
            uint32_t size = buckets[bucket];
            buckets[bucket] = offset;
            offset += size;
        }
        for (uint32_t i = 0; i < count; i++) scratch[buckets[(keys[i].key >> (digit * 8)) & 0xff]++] = keys[i];
        SortKey *sorted = scratch;
        scratch = keys;
        keys = sorted;
    }
    return keys;
}

static void sort_chunks(int phase, BOOL helping) {
    for (;;) {
        LONG chunk = InterlockedIncrement(&list_sort.next[phase]) - 1;
        if (chunk >= (LONG)list_sort.chunks) return;
        uint32_t first = (uint32_t)chunk * SORT_CHUNK, end = first + SORT_CHUNK < list_sort.count ? first + SORT_CHUNK : list_sort.count;
        if (phase == 0) {
            for (uint32_t i = first; i < end; i++) {
                if (i + 8 < end) {
                    const BYTE *ahead = list_sort.items + list_sort.order[i + 8].index * RENDER_ITEM_SIZE;
                    __builtin_prefetch(ahead);
                    __builtin_prefetch(ahead + 64);
                    __builtin_prefetch(ahead + RENDER_ITEM_SIZE - 1);
                }
                memcpy(list_sort.scratch + (size_t)i * RENDER_ITEM_SIZE, list_sort.items + list_sort.order[i].index * RENDER_ITEM_SIZE, RENDER_ITEM_SIZE);
            }
        } else {
            memcpy(list_sort.items + (size_t)first * RENDER_ITEM_SIZE, list_sort.scratch + (size_t)first * RENDER_ITEM_SIZE, (size_t)(end - first) * RENDER_ITEM_SIZE);
        }
        if (helping) InterlockedIncrement(&list_sort.helped);
        InterlockedIncrement(&list_sort.finished[phase]);
    }
}

// Every early-prepare task that ends before list A's sort helps move its items; the sort's owner always ends it.
static void help_list_sort(void) {
    for (LONG state; (state = list_sort.state) != SORT_DONE; _mm_pause())
        if (state == SORT_GATHER || state == SORT_COPY) sort_chunks(state - SORT_GATHER, TRUE);
}

static BOOL grow_list_sort(uint32_t count) {
    if (count <= list_sort.capacity) return TRUE;
    uint32_t capacity = 2 * count;
    free(list_sort.keys);
    free(list_sort.swap);
    free(list_sort.scratch);
    list_sort.keys = malloc(capacity * sizeof *list_sort.keys);
    list_sort.swap = malloc(capacity * sizeof *list_sort.swap);
    list_sort.scratch = malloc((size_t)capacity * RENDER_ITEM_SIZE);
    list_sort.capacity = list_sort.keys && list_sort.swap && list_sort.scratch ? capacity : 0;
    return list_sort.capacity != 0;
}

// The order the render sort's radix sort (s2prof sortfix) gives: stable by the 64-bit key at +0xa8.
static void parallel_sort_list(BYTE *body) {
    LONGLONG started = now_ticks();
    uint32_t count = *(uint32_t *)(body + 0x04);
    BYTE *items = *(BYTE **)(body + 0x08);
    if (count >= 2 * SORT_CHUNK && grow_list_sort(count)) {
        for (uint32_t i = 0; i < count; i++) list_sort.keys[i] = (SortKey){*(uint64_t *)(items + (size_t)i * RENDER_ITEM_SIZE + 0xa8), i};
        SortKey *order = radix_sort_keys(list_sort.keys, list_sort.swap, count);
        uint32_t i = 0;
        while (i < count && order[i].index == i) i++;
        if (i < count) {
            list_sort.items = items;
            list_sort.order = order;
            list_sort.count = count;
            list_sort.chunks = (count + SORT_CHUNK - 1) / SORT_CHUNK;
            list_sort.next[0] = list_sort.next[1] = list_sort.finished[0] = list_sort.finished[1] = 0;
            for (int phase = 0; phase < 2; phase++) {
                InterlockedExchange(&list_sort.state, SORT_GATHER + phase);
                sort_chunks(phase, FALSE);
                while (list_sort.finished[phase] < (LONG)list_sort.chunks) _mm_pause();
            }
        }
    } else {
        sort_render_list(body);
    }
    list_sort.ticks = now_ticks() - started;
    InterlockedExchange(&list_sort.state, SORT_DONE);
}

// While the fill collects, fill_instance_textures creates the textures and hands each to fill_texture in turn.
static void fill_texture_hook(BYTE *node, BYTE *texture, uint32_t first_row) {
    if (!collecting_fills) {
        original_fill_texture(node, texture, first_row);
        return;
    }
    if (texture_fill_count == MAX_TEXTURE_FILLS) {
        api->log_line("overhaul: more than %d instance textures to fill", MAX_TEXTURE_FILLS);
        __builtin_trap();
    }
    texture_fills[texture_fill_count++] = (TextureFill){node, texture, first_row};
}

static void drain_texture_fills(BYTE *unused) {
    (void)unused;
    for (LONG i; (i = InterlockedIncrement(&next_texture_fill) - 1) < (LONG)texture_fill_count;)
        original_fill_texture(texture_fills[i].node, texture_fills[i].texture, texture_fills[i].first_row);
}

static void drain_and_help(BYTE *unused) {
    drain_texture_fills(unused);
    help_list_sort();
}

// Each texture maps on its own and copies its own 1024 rows; the immediate context's lock (below) serialises the maps.
static void early_fill_task(BYTE *node) {
    texture_fill_count = 0;
    next_texture_fill = 0;
    collecting_fills = overhaul_split_fill != 0;
    original_fill_instance_textures(node);
    collecting_fills = FALSE;
    if (!texture_fill_count) {
        help_list_sort();
        return;
    }
    for (int h = 0; h < FILL_HELPERS; h++) kick_task(workers[EARLY_PREPARE_WORKERS + h], drain_and_help, NULL);
    drain_texture_fills(NULL);
    help_list_sort();
    for (int h = 0; h < FILL_HELPERS; h++)
        while (workers[EARLY_PREPARE_WORKERS + h]->task) _mm_pause();
}

// prepare's main part: the pose merge the keys resolve through, then A's keys, merge and sort.
static void early_keys_task(BYTE *node) {
    if (node[0x2a0]) ((ListMergeFn)(engine + POSE_MERGE_RVA))(node + 0x270);
    early.counts[0] = *(uint32_t *)(node + 0x274);
    InterlockedExchange(&early.poses_merged, 1);
    original_compute_keys(node, node + 0xf8);
    if (node[0x130]) ((ListMergeFn)(engine + RENDER_LIST_MERGE_RVA))(node + 0x100);
    if (list_sort.state == SORT_WAITING) parallel_sort_list(node + 0x100);
    else sort_render_list(node + 0x100);
    early.counts[1] = *(uint32_t *)(node + 0x104);
}

// The engine's list B task, then C's keys and first sort; the keys need the merged pose list.
static void early_lists_task(BYTE *node) {
    if (node[0x198]) ((ListMergeFn)(engine + RENDER_LIST_MERGE_RVA))(node + 0x168);
    sort_render_list(node + 0x168);
    early.counts[2] = *(uint32_t *)(node + 0x16c);
    while (!early.poses_merged) _mm_pause();
    original_compute_keys(node, node + 0x1c8);
    early.counts[3] = *(uint32_t *)(node + 0x1d4);
    sort_list_c(node + 0x1c8);
    help_list_sort();
}

// The immediate context takes no lock of its own unless asked: three fill threads mapping instance textures on it at
// once crashed the driver, and main may call it during the UI update beside them.
static void protect_immediate_context(BOOL on) {
    static BOOL logged;
    if (!context_lock) {
        HRESULT result = ID3D11DeviceContext_QueryInterface(engine_immediate_context(), &IID_ID3D11Multithread, (void **)&context_lock);
        if (FAILED(result)) {
            api->log_line("overhaul: the immediate context has no ID3D11Multithread: 0x%08lx", (unsigned long)result);
            __builtin_trap();
        }
    }
    BOOL was = ID3D11Multithread_SetMultithreadProtected(context_lock, on);
    if (!logged) api->log_line("overhaul: the immediate context's multithread protection was %s before the first early prepare", was ? "on" : "off");
    logged = TRUE;
}

static void join_early_prepare(void) {
    for (int i = 0; i < EARLY_PREPARE_WORKERS; i++)
        while (workers[i]->task) _mm_pause();
    protect_immediate_context(FALSE);
    early.pending = FALSE;
}

// BattleView::update ends by joining the producers; the UI update runs next, then the first view group's prepare.
static void battle_view_update_hook(BYTE *battle_view, BYTE *frame_argument) {
    original_battle_view_update(battle_view, frame_argument);
    uint32_t stamp = scene_stamp();
    BOOL continuing = battle_update_stamp + 1 == stamp && battle_node_stamp + 1 == stamp;
    battle_update_stamp = stamp;
    if (early.pending) join_early_prepare();
    if (!overhaul_enabled || !overhaul_early_prepare || overlap_frame || !continuing) return;
    BYTE *node = battle_node;
    // prepare's own condition: instancing active and rows dirty.
    if (!node[0x2e0] || node[0x2e2] || !*(uint32_t *)(node + 0x2d4) || !node[0x2e1]) return;
    int needed = EARLY_PREPARE_WORKERS + FILL_HELPERS;
    if (worker_count < needed && !start_workers(needed, engine_immediate_context())) return;
    early.node = node;
    early.pending = TRUE;
    early.poses_merged = 0;
    early.stamp = stamp;
    list_sort.helped = 0;
    list_sort.ticks = 0;
    list_sort.chunks = 0;
    list_sort.state = overhaul_parallel_sort ? SORT_WAITING : SORT_DONE;
    protect_immediate_context(TRUE);
    kick_task(workers[0], early_fill_task, node);
    kick_task(workers[1], early_keys_task, node);
    kick_task(workers[2], early_lists_task, node);
}

// Lists that grew or need a merge again since the early prepare, or rows dirty again, mean something added after it.
static BOOL early_prepare_holds(BYTE *node, uint32_t stamp) {
    return early.stamp == stamp && !node[0x2a0] && !node[0x130] && !node[0x198] && !node[0x200] && !node[0x2e1] &&
           early.counts[0] == *(uint32_t *)(node + 0x274) && early.counts[1] == *(uint32_t *)(node + 0x104) &&
           early.counts[2] == *(uint32_t *)(node + 0x16c) && early.counts[3] == *(uint32_t *)(node + 0x1d4);
}

static void prepare_hook(BYTE *prepare_interface, BYTE *renderer) {
    BYTE *node = prepare_interface - 8;
    uint32_t stamp = scene_stamp();
    if (stamp != battle_node_stamp) {
        battle_node = node;
        battle_node_stamp = stamp;
    }
    PrepareStats *stats = &prepare_stats[ab_mode()];
    if (*frame_stamp != prepare_stamp) {
        prepare_stamp = *frame_stamp;
        stats->frames++;
    }
    LONGLONG started = now_ticks();
    if (early.pending && early.node == node) {
        join_early_prepare();
        LONGLONG joined = now_ticks();
        stats->texture_fills += texture_fill_count;
        if (list_sort.ticks) {
            stats->sorts++;
            stats->sort += list_sort.ticks;
            stats->sort_chunks += 2 * list_sort.chunks;
            stats->helped_chunks += list_sort.helped;
        }
        if (early_prepare_holds(node, stamp)) {
            stats->early_used++;
            stats->ticks += joined - started;
            stats->join += joined - started;
            return;
        }
        stats->early_fallbacks++;
        node[0x2e1] = 1;
    }
    int split = overhaul_prepare_split && overhaul_enabled;
    prepare_split_active = split && !overlap_active && (worker_count >= PREPARE_WORKERS || start_workers(PREPARE_WORKERS, engine_immediate_context()));
    original_prepare(prepare_interface, renderer);
    LONGLONG joined = now_ticks();
    if (prepare_split_active) {
        for (int i = 0; i < PREPARE_WORKERS; i++)
            while (workers[i]->task) _mm_pause();
        prepare_split_active = FALSE;
    }
    LONGLONG finished = now_ticks();
    stats->ticks += finished - started;
    stats->join += finished - joined;
}

static void push_range(Worker *worker, ViewSlot *slot, Range *range) {
    worker->range_tasks[worker->range_tasks_pushed % MAX_SLOTS] = (RangeTask){slot, range};
    InterlockedIncrement(&worker->range_tasks_pushed);
}

static void wake_workers(void) {
    InterlockedIncrement(&generation);
    WakeByAddressAll((void *)&generation);
}

static BYTE execute_lists(ViewSlot *slot, int first, ID3D11DeviceContext *immediate, BOOL restore) {
    BYTE result = 1;
    GpuFrame *timed = slot->timed_frame;
    for (int k = first; k < slot->range_count; k++) {
        Range *range = &slot->ranges[k];
        if (FAILED(range->finish_result)) {
            api->log_line("overhaul: range %d: FinishCommandList failed: 0x%08lx", k, (unsigned long)range->finish_result);
            __builtin_trap();
        }
        if (timed) {
            ID3D11DeviceContext_End(immediate, (ID3D11Asynchronous *)timed->list_begin[slot->timed_view][k]);
            timed->lists[slot->timed_view] = k;
        }
        if (range->upload_list) {
            ID3D11DeviceContext_ExecuteCommandList(immediate, range->upload_list, restore);
            ID3D11CommandList_Release(range->upload_list);
        }
        ID3D11DeviceContext_ExecuteCommandList(immediate, range->list, restore);
        ID3D11CommandList_Release(range->list);
        range->list = NULL;
        result &= range->result;
    }
    return result;
}

// The probe lists: what executing a list costs the GPU when it draws nothing, executed where the view's own lists were.
static void execute_probe_lists(ViewSlot *slot, ID3D11DeviceContext *immediate, BOOL restore) {
    if (probe_error) return;
    if (!probe_context) {
        HRESULT result = ID3D11Device_CreateDeferredContext(device, 0, &probe_context);
        if (FAILED(result)) {
            api->log_line("overhaul: no probe lists: CreateDeferredContext failed: 0x%08lx", (unsigned long)result);
            probe_error = TRUE;
            return;
        }
    }
    int count = overhaul_probe_count < 0 ? 0 : overhaul_probe_count > MAX_WORKERS ? MAX_WORKERS : overhaul_probe_count;
    for (int i = 0; i < count; i++) {
        if (overhaul_probe_bind) bind_targets(probe_context, &slot->targets);
        ID3D11CommandList *list;
        HRESULT result = ID3D11DeviceContext_FinishCommandList(probe_context, FALSE, &list);
        if (FAILED(result)) {
            api->log_line("overhaul: no probe lists: FinishCommandList failed: 0x%08lx", (unsigned long)result);
            probe_error = TRUE;
            return;
        }
        ID3D11DeviceContext_ExecuteCommandList(immediate, list, restore);
        ID3D11CommandList_Release(list);
        probe_lists_run[ab_mode()]++;
    }
}

static void free_slot(void) {
    slots_freed++;
}

static void push_submission(ViewSlot *slot, ID3D11CommandList *list) {
    if (submissions_pushed - submissions_done == MAX_SUBMISSIONS) {
        api->log_line("overhaul: more than %d command lists waiting", MAX_SUBMISSIONS);
        __builtin_trap();
    }
    submissions[submissions_pushed++ % MAX_SUBMISSIONS] = (Submission){slot, list};
}

// Executes the finished front of the queue on the immediate context; with wait, all of it.
static void submit(BOOL wait) {
    while (submissions_done != submissions_pushed) {
        Submission *submission = &submissions[submissions_done % MAX_SUBMISSIONS];
        ViewSlot *slot = submission->slot;
        if (slot && slot->remaining) {
            if (!wait) return;
            _mm_pause();
            continue;
        }
        if (slot) {
            execute_lists(slot, 0, wrapper->immediate, FALSE);
            release_targets(&slot->targets);
            free_slot();
        } else {
            ID3D11DeviceContext_ExecuteCommandList(wrapper->immediate, submission->list, FALSE);
            ID3D11CommandList_Release(submission->list);
        }
        submissions_done++;
    }
}

// Slots are taken in kick order and freed in execute order, so they cycle.
static ViewSlot *take_slot(void) {
    while (slots_taken - slots_freed == MAX_SLOTS) {
        submit(FALSE);
        _mm_pause();
    }
    return &slots[slots_taken++ % MAX_SLOTS];
}

// The proxy forwards every slot of the newest interface, so main's deferred context must have them all.
static ID3D11DeviceContext *newest_context(ID3D11DeviceContext *context) {
    static const IID *const iids[] = {&IID_ID3D11DeviceContext4, &IID_ID3D11DeviceContext3, &IID_ID3D11DeviceContext2, &IID_ID3D11DeviceContext1};
    for (size_t i = 0; i < sizeof iids / sizeof *iids; i++) {
        ID3D11DeviceContext *newest;
        if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(context, iids[i], (void **)&newest))) {
            ID3D11DeviceContext_Release(newest);
            return newest;
        }
    }
    return context;
}

// The engine must hold the proxy's context, so every holder of it follows main into its deferred context.
static BOOL overlap_ready(BYTE *renderer) {
    if (overlap_checked) return main_deferred != NULL;
    overlap_checked = TRUE;
    ID3D11DeviceContext *context = *(ID3D11DeviceContext **)(renderer + 0x10);
    void *vtable = *(void **)context;
    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, vtable, &module) ||
        (void *)GetProcAddress(module, "wrapped_context_vtable") != vtable) {
        api->log_line("overhaul: overlap needs the proxy's context; set wrap_context=1 in s2prof.ini");
        return FALSE;
    }
    ID3D11DeviceContext *deferred;
    HRESULT result = ID3D11Device_CreateDeferredContext(device, 0, &deferred);
    if (FAILED(result)) {
        api->log_line("overhaul: no overlap: CreateDeferredContext for main failed: 0x%08lx", (unsigned long)result);
        return FALSE;
    }
    wrapper = (ContextWrapper *)context;
    main_deferred = newest_context(deferred);
    return TRUE;
}

static void finish_segment(Timers *timer) {
    ID3D11CommandList *list;
    HRESULT result = ID3D11DeviceContext_FinishCommandList(main_deferred, FALSE, &list);
    if (FAILED(result)) {
        api->log_line("overhaul: FinishCommandList of main's segment failed: 0x%08lx", (unsigned long)result);
        __builtin_trap();
    }
    forget_uploads(&main_uploads);
    push_submission(NULL, list);
    timer->segments++;
}

// A deferred context starts with nothing bound, so main's renderer starts cold.
static void begin_segment(BYTE *renderer, const TargetState *state) {
    make_cold(renderer);
    bind_targets(main_deferred, state);
    main_uploads.list_number++;
    main_uploads.use_copies = overhaul_cb_copies;
    main_segment++;
    main_segment_rings = 0;
}

// Workers draw every range; main queues its segment so far, then the view's lists, and records on into a new segment.
static BYTE overlap_execute(ViewSlot *slot, BYTE *context, BYTE *renderer, BYTE *view, int range_count, Timers *timer, LONGLONG started) {
    uint32_t bucket_count = *(uint32_t *)(context + 0x1c);
    double ends[MAX_WORKERS + 1];
    share_ends(range_count, 100, ends);
    slot->range_count = split_ranges(slot, range_count, bucket_count, ends);
    for (int k = 0; k < slot->range_count; k++) build_range_context(slot, &slot->ranges[k], context, workers[k]);
    capture_targets(&slot->targets, (ID3D11DeviceContext *)wrapper);
    memcpy(slot->renderer_template, *(BYTE **)(engine + RENDER_DEVICE_RVA) + RENDERER_OFFSET, RENDERER_SIZE);
    slot->view = view;
    slot->view_type = main_view_type;
    slot->overlapped = TRUE;
    slot->remaining = slot->range_count;
    if (overlap_active) {
        finish_segment(timer);
    } else {
        split_thread = GetCurrentThreadId();
        wrapper->current = main_deferred;
        overlap_active = TRUE;
    }
    push_submission(slot, NULL);
    for (int k = 0; k < slot->range_count; k++) push_range(workers[k], slot, &slot->ranges[k]);
    wake_workers();
    begin_segment(renderer, &slot->targets);
    LONGLONG kicked = now_ticks();
    timer->kick += kicked - started;
    submit(FALSE);
    timer->submit += now_ticks() - kicked;
    uint64_t in_flight = slots_taken - slots_freed;
    if (in_flight > timer->peak_in_flight) timer->peak_in_flight = in_flight;
    timer->overlap_views++;
    timer->views++;
    timer->jobs += slot->job_count;
    timer->ranges += slot->range_count;
    return 1;
}

static void drain(void) {
    if (!overlap_active) return;
    Timers *timer = &timers[ab_mode()];
    LONGLONG started = now_ticks();
    BYTE *renderer = *(BYTE **)(engine + RENDER_DEVICE_RVA) + RENDERER_OFFSET;
    TargetState state;
    capture_targets(&state, main_deferred);
    finish_segment(timer);
    submit(TRUE);
    wrapper->current = wrapper->immediate;
    overlap_active = FALSE;
    split_thread = 0;
    make_cold(renderer);
    bind_targets(wrapper->immediate, &state);
    release_targets(&state);
    run_kicks(&main_kicks, renderer);
    for (int i = 0; i < worker_count; i++) run_kicks(&workers[i]->kicks, renderer);
    timer->drains++;
    timer->drain += now_ticks() - started;
}

static BYTE *draw_context(void) {
    return *(BYTE **)(engine + DRAW_CONTEXT_RVA);
}

// The shadow view's next cascade is recorded ahead only with recordcascades: it records on the view object the cascade
// being drawn uses (its index at +0x94), and its vegetation record rewrote the group-instance meshes the draws read,
// which groupgather's snapshot now covers.
static BOOL records_ahead(uint32_t type) {
    return type < VIEW_TYPES && type != 1 && (overhaul_record_ahead_views >> type & 1);
}

// The view whose record can run during this view's draws, or NULL: the next cascade of the shadow view, or the next view
// the group renders when it records through the queue walk (a view with its own pass group or record could use the
// buckets in between). The record is checked at the view's own turn.
static BYTE *record_ahead_target(BYTE *view, uint32_t type, uint32_t *cascade) {
    *cascade = UINT32_MAX;
    if (!render_context) return NULL;
    if (type == 1) {
        uint32_t index = *(uint32_t *)(view + 0x94);
        if (!overhaul_record_cascades || overhaul_parallel_cascades || !overhaul_group_gather || index + 1 >= *(uint32_t *)(view + 0x98)) return NULL;
        *cascade = index + 1;
        return method(view, 0xe0) == engine + RECORD_QUEUES_RVA ? view : NULL;
    }
    if (!records_ahead(type)) return NULL;
    BYTE *list = *(BYTE **)(render_context + 0x28);
    uint32_t count = *(uint32_t *)(list + 0x14), i = 0;
    BYTE **views = *(BYTE ***)(list + 0x18);
    while (i < count && views[i] != view) i++;
    for (i++; i < count; i++) {
        BYTE *next = views[i];
        if (!next[0x60] || !next[0x18]) continue;
        if (*(BYTE *)(render_context + 0x21) && !(*(uint32_t *)((BYTE *(*)(BYTE *))method(next, 0x90))(next) & 8)) continue;
        uint32_t next_type = ((ViewTypeFn)method(next, 0x78))(next);
        BOOL queue_walk = method(next, 0xe0) == engine + RECORD_QUEUES_RVA && method(next, 0x168) == engine + RECORD_EXECUTE_RVA;
        return queue_walk && records_ahead(next_type) ? next : NULL;
    }
    return NULL;
}

static void record_into_buckets(BYTE *next, uint32_t cascade) {
    original_reset_buckets(draw_context());
    if (cascade != UINT32_MAX) *(uint32_t *)(next + 0x94) = cascade;
    original_record_queues(next);
    if (cascade != UINT32_MAX) *(uint32_t *)(next + 0x94) = cascade - 1;
}

static void record_ahead(BYTE *next, uint32_t cascade, Timers *timer) {
    LONGLONG started = now_ticks();
    record_into_buckets(next, cascade);
    ahead.view = next;
    ahead.cascade = cascade;
    ahead.skip_reset = TRUE;
    timer->ahead_records++;
    timer->ahead += now_ticks() - started;
}

static void record_task(BYTE *unused) {
    (void)unused;
    LONGLONG started = now_ticks();
    record_into_buckets(ahead.view, ahead.cascade);
    InterlockedAdd64(&record_worker_ticks[ab_mode()], now_ticks() - started);
}

// The record worker starts once the view's groups are copied and before main draws, so the next view records beside
// main's own range as well as the workers'.
static void kick_record_ahead(BYTE *next, uint32_t cascade, Worker *worker, Timers *timer) {
    ahead.view = next;
    ahead.cascade = cascade;
    ahead.skip_reset = TRUE;
    ahead.worker = worker;
    timer->ahead_records++;
    kick_task(worker, record_task, NULL);
}

// By thread id: the game leaves the overhaul one TEB slot under 64, and a second TLS index lands past them.
// Every record of every view asks while packets are taken, so the thread id is the TEB's (ClientId.UniqueThread).
static PacketBuffer *thread_packets(void) {
    DWORD self = __readgsdword(0x48);
    if (effects_pending)
        for (int e = 0; e < EARLY_EFFECTS; e++)
            if (effects_records[e] && effects_records[e]->worker_packets.thread == self) return &effects_records[e]->worker_packets;
    if (!packets_active) return NULL;
    for (int c = 1; c < STATS_CASCADES; c++) {
        if (cascade_records[c].worker_packets.thread == self) return &cascade_records[c].worker_packets;
        if (cascade_records[c].main_packets.thread == self) return &cascade_records[c].main_packets;
    }
    return NULL;
}

static void append_packet(PacketBuffer *buffer, BYTE *effect, uint32_t index, uint32_t flags) {
    if (buffer->count == buffer->capacity) {
        buffer->capacity = buffer->capacity ? 2 * buffer->capacity : 8192;
        buffer->packets = realloc(buffer->packets, buffer->capacity * sizeof *buffer->packets);
        if (!buffer->packets) {
            api->log_line("overhaul: out of memory for %u record packets", buffer->capacity);
            __builtin_trap();
        }
    }
    buffer->packets[buffer->count++] = (RecordPacket){effect, index, flags, buffer->queue};
}

// TRUE when the record is the packets' alone.
static BOOL capture_record(BYTE *effect, uint32_t index, uint32_t flags) {
    PacketBuffer *buffer = packets_active || effects_pending ? thread_packets() : NULL;
    if (!buffer) return FALSE;
    append_packet(buffer, effect, index, flags);
    void *vtable = **(void ***)(*(BYTE **)(effect + 0x18) + 0x38);
    if (vtable != engine + BATCH_RECORDER_VTABLE_RVA && vtable != engine + SINGLE_RECORDER_VTABLE_RVA) buffer->unknown_recorder = TRUE;
    return !buffer->forward;
}

// Records reach the engine's recorders through two effect methods: vtbl+0x158 (thunk 0x1819d2d60, and extrasrecord's
// first record) calls this; a thread recording into packets keeps the record and leaves the recorders and buckets alone.
static uint64_t push_record_hook(BYTE *context, BYTE *effect, uint32_t index, uint32_t flags) {
    if (capture_record(effect, index, flags)) return 0;
    return original_push_record(context, effect, index, flags);
}

// vtbl+0x160 records the whole effect with push_record's body inlined: index ~0, flags 0 (the dynamic node's meshes).
static uint64_t record_whole_hook(BYTE *effect) {
    if (capture_record(effect, UINT32_MAX, 0)) return 0;
    return original_record_whole(effect);
}

// VegetationNode::record (0x181b6cb00) without its stores into the shared meshes: the instance count and pass bit it would
// store for this view go into the packets, where the cascade's group-instance jobs take them from.
static void vegetation_record_packets(BYTE *queue, BYTE *view, PacketBuffer *buffer) {
    uint32_t count = *(uint32_t *)(queue + 0xdc);
    BYTE **meshes = *(BYTE ***)(queue + 0xe0);
    for (uint32_t i = 0; i < count; i++) {
        BYTE *mesh = meshes[i];
        uint32_t bit = ((PassBitFn)method(view, 0xa0))(view) & 31, instances = 0, groups = *(uint32_t *)(mesh + 0x1cc);
        const BYTE *group = *(BYTE **)(mesh + 0x1d0);
        for (uint32_t g = 0; g < groups; g++, group += 24)
            if ((*(const uint32_t *)(group + 16) >> bit & 1) && *(const uint32_t *)(group + 12) != 1) instances += *(const uint32_t *)(group + 8);
        if (buffer->group_count == buffer->group_capacity) {
            buffer->group_capacity = buffer->group_capacity ? 2 * buffer->group_capacity : 256;
            buffer->groups = realloc(buffer->groups, buffer->group_capacity * sizeof *buffer->groups);
            if (!buffer->groups) {
                api->log_line("overhaul: out of memory for %u group-instance meshes", buffer->group_capacity);
                __builtin_trap();
            }
        }
        buffer->groups[buffer->group_count++] = (GroupState){mesh, instances, bit};
        if (instances) ((EffectRecordFn)method(mesh, 0x158))(mesh, 0, 0);
    }
}

// The queue records known to touch only their own walk off main: units (their view type is 1 in every cascade), rigid
// models, and vegetation through vegetation_record_packets. Imposters (a TBB batch and counters per call), terrain, weather
// and the rest record on main.
static BOOL worker_records(void *record) {
    return record == engine + VARIANT_RECORD_RVA || record == engine + RIGID_RECORD_RVA || record == engine + VEGETATION_RECORD_RVA;
}

static void record_queue(BYTE *queue, BYTE *view, PacketBuffer *buffer) {
    void *record = method(queue, 0x10);
    if (record == engine + VEGETATION_RECORD_RVA && buffer && !buffer->forward) vegetation_record_packets(queue, view, buffer);
    else ((QueueRecordFn)record)(queue, view);
}

static void cascade_record_task(BYTE *argument) {
    CascadeRecord *record = (CascadeRecord *)argument;
    LONGLONG started = now_ticks();
    record->worker_packets.thread = GetCurrentThreadId();
    uint32_t count = *(uint32_t *)(record->view + 0x24);
    BYTE **queues = *(BYTE ***)(record->view + 0x28);
    for (uint32_t q = 0; q < count; q++) {
        if (!worker_records(method(queues[q], 0x10))) continue;
        record->worker_packets.queue = q;
        record_queue(queues[q], record->view, &record->worker_packets);
    }
    record->worker_packets.thread = 0;
    record->worker_ticks = now_ticks() - started;
}

// Right after cascade 0's own record, on the workers after the drawing ones.
static void kick_cascade_records(BYTE *view) {
    uint32_t cascades = *(uint32_t *)(view + 0x98);
    int first = overhaul_workers < 0 ? 0 : overhaul_workers;
    if (cascades < 2 || cascades > STATS_CASCADES || first + (int)cascades - 1 > worker_count) return;
    packets_active = 1;
    for (uint32_t c = 1; c < cascades; c++) {
        if (!(overhaul_cascades >> c & 1)) continue;
        CascadeRecord *record = &cascade_records[c];
        memcpy(record->view, view, SHADOW_VIEW_SIZE);
        *(uint32_t *)(record->view + 0x94) = c;
        record->real_view = view;
        record->cascade = c;
        record->worker_packets.count = record->worker_packets.group_count = record->main_packets.count = record->main_packets.group_count = 0;
        record->worker_packets.unknown_recorder = record->main_packets.unknown_recorder = FALSE;
        record->worker = workers[first + c - 1];
        record->kicked = TRUE;
        kick_task(record->worker, cascade_record_task, (BYTE *)record);
    }
}

static CascadeRecord *kicked_cascade(BYTE *view) {
    if (!packets_active || ((ViewTypeFn)method(view, 0x78))(view) != 1) return NULL;
    uint32_t cascade = *(uint32_t *)(view + 0x94);
    if (cascade >= STATS_CASCADES || !cascade_records[cascade].kicked || cascade_records[cascade].real_view != view) return NULL;
    return &cascade_records[cascade];
}

// At the cascade's own turn: main records the queues the worker left, into packets. With packetverify the engine also
// records the whole view into its buckets, as it would have, and those are what get drawn.
static void finish_cascade_record(BYTE *view, CascadeRecord *record) {
    int mode = ab_mode();
    LONGLONG started = now_ticks();
    while (record->worker->task) _mm_pause();
    LONGLONG waited = now_ticks();
    parallel_wait_ticks[mode] += waited - started;
    parallel_worker_ticks[mode] += record->worker_ticks;
    BOOL verify = record->verify = overhaul_packet_verify != 0;
    record->main_packets.forward = verify;
    if (!verify)
        for (uint32_t i = 0; i < record->worker_packets.group_count; i++) {
            const GroupState *group = &record->worker_packets.groups[i];
            *(uint32_t *)(group->mesh + 0x1f0) = group->count;
            *(uint32_t *)(group->mesh + 0x1f4) = group->bit;
        }
    uint32_t count = *(uint32_t *)(view + 0x24);
    BYTE **queues = *(BYTE ***)(view + 0x28);
    DWORD self = GetCurrentThreadId();
    for (uint32_t q = 0; q < count; q++) {
        BOOL on_worker = worker_records(method(queues[q], 0x10));
        if (on_worker && !verify) continue;
        record->main_packets.queue = q;
        record->main_packets.thread = on_worker ? 0 : self;
        record_queue(queues[q], view, on_worker ? NULL : &record->main_packets);
    }
    record->main_packets.thread = 0;
    record->kicked = FALSE;
    packet_view = record;
    parallel_cascade_count[mode]++;
    packet_count[mode] += record->worker_packets.count + record->main_packets.count;
    parallel_main_ticks[mode] += now_ticks() - waited;
}

static void finish_all_cascade_records(void) {
    for (int c = 0; c < STATS_CASCADES; c++)
        if (cascade_records[c].kicked) {
            while (cascade_records[c].worker->task) _mm_pause();
            cascade_records[c].kicked = FALSE;
        }
    packet_view = NULL;
    packets_active = 0;
}

// Scratch for build_packet_jobs, reused by every view.
static RecordEntry *build_entries;
static uint32_t build_entry_count, build_entry_capacity;
static uint32_t *build_entry_table, build_table_size;
static BuiltGroup *build_groups;
static uint32_t build_group_count, build_group_capacity;
static uint32_t *build_bucket_first, *build_bucket_last, build_bucket_capacity;
static uint32_t *build_packet_entry, build_packet_capacity;
static uint32_t *build_indices, build_index_capacity;
static const RecordPacket **build_order;
static uint32_t build_order_capacity;

// Each buffer is in record order with rising queue numbers, and each queue was recorded whole by one thread, so merging by
// queue gives the serial record's order.
static uint32_t merge_packets(const CascadeRecord *record) {
    const PacketBuffer *a = &record->worker_packets, *b = &record->main_packets;
    uint32_t total = a->count + b->count, ia = 0, ib = 0;
    if (total > build_order_capacity) {
        build_order_capacity = 2 * total;
        build_order = realloc(build_order, build_order_capacity * sizeof *build_order);
        if (!build_order) {
            api->log_line("overhaul: out of memory merging %u record packets", total);
            __builtin_trap();
        }
    }
    for (uint32_t p = 0; p < total; p++)
        build_order[p] = ib == b->count || (ia < a->count && a->packets[ia].queue <= b->packets[ib].queue) ? &a->packets[ia++] : &b->packets[ib++];
    return total;
}

// The engine's own record of the view, from its packets.
static void replay_packets(BYTE *context, const CascadeRecord *record) {
    uint32_t total = merge_packets(record);
    for (uint32_t p = 0; p < total; p++) original_push_record(context, build_order[p]->effect, build_order[p]->index, build_order[p]->flags);
    replayed_views[ab_mode()]++;
}

static void *grow(void *data, uint32_t *capacity, uint32_t needed, size_t element) {
    if (needed <= *capacity) return data;
    *capacity = needed * 2;
    data = realloc(data, (size_t)*capacity * element);
    if (!data) {
        api->log_line("overhaul: out of memory rebuilding a view's groups (%u x %zu B)", *capacity, element);
        __builtin_trap();
    }
    return data;
}

static uint32_t entry_for(BYTE *recorder, BYTE *item) {
    if (2 * (build_entry_count + 1) > build_table_size) {
        build_table_size = build_table_size ? build_table_size * 2 : 8192;
        free(build_entry_table);
        build_entry_table = malloc(build_table_size * sizeof *build_entry_table);
        if (!build_entry_table) {
            api->log_line("overhaul: out of memory for the recorder table");
            __builtin_trap();
        }
        memset(build_entry_table, 0xff, build_table_size * sizeof *build_entry_table);
        for (uint32_t e = 0; e < build_entry_count; e++) {
            uint32_t slot = (uint32_t)((uintptr_t)build_entries[e].recorder >> 4) * 0x9e3779b9u & (build_table_size - 1);
            while (build_entry_table[slot] != UINT32_MAX) slot = (slot + 1) & (build_table_size - 1);
            build_entry_table[slot] = e;
        }
    }
    uint32_t slot = (uint32_t)((uintptr_t)recorder >> 4) * 0x9e3779b9u & (build_table_size - 1);
    for (; build_entry_table[slot] != UINT32_MAX; slot = (slot + 1) & (build_table_size - 1))
        if (build_entries[build_entry_table[slot]].recorder == recorder) return build_entry_table[slot];
    build_entries = grow(build_entries, &build_entry_capacity, build_entry_count + 1, sizeof *build_entries);
    uint32_t e = build_entry_count++;
    RecordEntry *entry = &build_entries[e];
    uint32_t bucket = *(uint32_t *)(item + 0x40);
    *entry = (RecordEntry){recorder, item, bucket, *(BYTE **)recorder == engine + BATCH_RECORDER_VTABLE_RVA, *(uint32_t *)(recorder + 0x14), 0, 0, 0,
                           UINT32_MAX, UINT32_MAX, UINT32_MAX};
    build_entry_table[slot] = e;
    if (build_bucket_first[bucket] == UINT32_MAX) build_bucket_first[bucket] = e;
    else build_entries[build_bucket_last[bucket]].next = e;
    build_bucket_last[bucket] = e;
    return e;
}

static void open_group(RecordEntry *entry, uint32_t e, BYTE *item, uint32_t index, uint32_t flags) {
    if (entry->used >= entry->capacity) entry->capacity = entry->used + 1;
    build_groups = grow(build_groups, &build_group_capacity, build_group_count + 1, sizeof *build_groups);
    uint32_t g = build_group_count++;
    BuiltGroup *group = &build_groups[g];
    group->item = item;
    group->count = 1;
    group->flags = flags;
    group->ids[0] = index;
    group->entry = e;
    group->previous = entry->last_group;
    group->next = UINT32_MAX;
    if (entry->last_group != UINT32_MAX) build_groups[entry->last_group].next = g;
    else entry->first_group = g;
    entry->last_group = g;
    entry->used++;
}

// BatchRecorder::record (0x1819d2310): a flagged record opens a group; an unflagged one joins the newest earlier group of
// the same item with room, searching back no further than a flagged group, or is dropped when the recorder has no groups or
// none spare.
static void batch_record(uint32_t e, BYTE *item, uint32_t index, uint32_t flags) {
    RecordEntry *entry = &build_entries[e];
    if (flags) {
        open_group(entry, e, item, index, flags);
        return;
    }
    if (!entry->used || entry->used >= entry->capacity) return;
    for (uint32_t g = entry->last_group; g != UINT32_MAX; g = build_groups[g].previous) {
        BuiltGroup *group = &build_groups[g];
        if (group->count < 256 && group->item == item) {
            group->ids[group->count++] = index;
            return;
        }
        if (group->flags) break;
    }
    open_group(entry, e, item, index, 0);
}

static void add_built_job(ViewSlot *slot, BYTE *packet, uint32_t bucket, BYTE *recorder, uint32_t weight, const CascadeRecord *record, BOOL verifying) {
    if (slot->job_count == slot->job_capacity) {
        slot->job_capacity = slot->job_capacity ? slot->job_capacity * 2 : 4096;
        slot->jobs = realloc(slot->jobs, slot->job_capacity * sizeof *slot->jobs);
        if (!slot->jobs) {
            api->log_line("overhaul: out of memory for %u jobs", slot->job_capacity);
            __builtin_trap();
        }
    }
    Job *job = &slot->jobs[slot->job_count++];
    *job = (Job){packet, bucket, JOB_PACKET, weight, recorder, 0, 0};
    BYTE *mesh = *(BYTE **)(((Packet *)packet)->item + 0x10);
    if (*(BYTE **)mesh != engine + GROUP_INSTANCE_MESH_VTABLE_RVA) return;
    job->group_count = *(uint32_t *)(mesh + 0x1f0);
    job->group_bit = *(uint32_t *)(mesh + 0x1f4);
    // The meshes hold the engine's own values under packetverify; the check is of the worker's.
    if (!verifying) return;
    for (uint32_t i = 0; i < record->worker_packets.group_count; i++)
        if (record->worker_packets.groups[i].mesh == mesh) {
            job->group_count = record->worker_packets.groups[i].count;
            job->group_bit = record->worker_packets.groups[i].bit;
            return;
        }
}

// BatchRecorder's group vector grows one group at a time and never shrinks, and an unflagged record finding it full is
// dropped (0x1819d2310, 0x1819ece50): the groups a rebuilt view opened past it are added as the engine would have, so the
// views and frames after see the vector the serial record leaves.
static void grow_batch_groups(BYTE *recorder, uint32_t size) {
    while (*(uint32_t *)(recorder + 0x14) < size) {
        BYTE *group = ((AllocateFn)(engine + ENGINE_ALLOCATE_RVA))(INSTANCE_GROUP_SIZE);
        if (!group) {
            api->log_line("overhaul: the engine's allocator gave no instance group");
            __builtin_trap();
        }
        *(uint64_t *)group = 0;
        *(uint64_t *)(group + 0x408) = 0;
        ((PointerPushBackFn)(engine + POINTER_PUSH_BACK_RVA))(recorder + 0x10, &group);
    }
}

// The jobs execute_pass would have drawn had the engine recorded this view: the two threads' packets merged into record
// order, each bucket's recorders in first-record order (program-sorted where the bucket sorts), a single recorder's indices
// in record order, a batch recorder's groups as BatchRecorder::record builds them.
static void build_packet_jobs(ViewSlot *slot, BYTE *context, const CascadeRecord *record, BOOL verifying) {
    uint32_t bucket_count = *(uint32_t *)(context + 0x1c);
    BYTE *sort_flags = *(BYTE **)(context + 0x30);
    build_bucket_first = grow(build_bucket_first, &build_bucket_capacity, bucket_count, sizeof *build_bucket_first);
    build_bucket_last = realloc(build_bucket_last, (size_t)build_bucket_capacity * sizeof *build_bucket_last);
    if (!build_bucket_last) {
        api->log_line("overhaul: out of memory for bucket lists");
        __builtin_trap();
    }
    memset(build_bucket_first, 0xff, bucket_count * sizeof *build_bucket_first);
    build_entry_count = build_group_count = 0;
    if (build_entry_table) memset(build_entry_table, 0xff, build_table_size * sizeof *build_entry_table);
    uint32_t total = merge_packets(record);
    const RecordPacket **order = build_order;
    build_packet_entry = grow(build_packet_entry, &build_packet_capacity, total, sizeof *build_packet_entry);
    for (uint32_t p = 0; p < total; p++) {
        BYTE *item = *(BYTE **)(order[p]->effect + 0x18), *recorder = *(BYTE **)(item + 0x38);
        uint32_t e = entry_for(recorder, item);
        build_packet_entry[p] = e;
        if (build_entries[e].batch) batch_record(e, item, order[p]->index, order[p]->flags);
        else build_entries[e].index_count++;
    }
    uint32_t indices = 0;
    for (uint32_t e = 0; e < build_entry_count; e++)
        if (!build_entries[e].batch) {
            build_entries[e].index_start = indices;
            indices += build_entries[e].index_count;
            build_entries[e].index_count = 0;
        }
    build_indices = grow(build_indices, &build_index_capacity, indices ? indices : 1, sizeof *build_indices);
    for (uint32_t p = 0; p < total; p++) {
        RecordEntry *entry = &build_entries[build_packet_entry[p]];
        if (!entry->batch) build_indices[entry->index_start + entry->index_count++] = order[p]->index;
    }
    if (!verifying)
        for (uint32_t e = 0; e < build_entry_count; e++)
            if (build_entries[e].batch) grow_batch_groups(build_entries[e].recorder, build_entries[e].capacity);
    size_t bytes = 0;
    for (uint32_t e = 0; e < build_entry_count; e++)
        if (!build_entries[e].batch) bytes += packet_size(build_entries[e].index_count);
    for (uint32_t g = 0; g < build_group_count; g++) bytes += packet_size(build_groups[g].count);
    if (bytes > slot->packet_capacity) {
        free(slot->packet_arena);
        slot->packet_capacity = bytes * 2;
        slot->packet_arena = malloc(slot->packet_capacity);
        if (!slot->packet_arena) {
            api->log_line("overhaul: out of memory for %zu bytes of packets", slot->packet_capacity);
            __builtin_trap();
        }
    }
    BYTE *at = slot->packet_arena;
    slot->job_count = 0;
    BYTE **sorted = NULL;
    uint32_t sorted_capacity = 0;
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++) {
        uint32_t entries = 0;
        for (uint32_t e = build_bucket_first[bucket]; e != UINT32_MAX; e = build_entries[e].next) entries++;
        if (!entries) continue;
        sorted = grow(sorted, &sorted_capacity, entries, sizeof *sorted);
        uint32_t n = 0;
        for (uint32_t e = build_bucket_first[bucket]; e != UINT32_MAX; e = build_entries[e].next) sorted[n++] = build_entries[e].recorder;
        if (sort_flags[bucket]) sort_bucket(sorted, sorted + n, n, 0);
        for (uint32_t i = 0; i < n; i++) {
            RecordEntry *entry = &build_entries[entry_for(sorted[i], NULL)];
            if (!entry->batch) {
                Packet *packet = (Packet *)at;
                *packet = (Packet){*(BYTE **)(entry->recorder + 0x10), entry->index_count, 0};
                memcpy(packet->ids, build_indices + entry->index_start, entry->index_count * sizeof(uint32_t));
                at += packet_size(entry->index_count);
                add_built_job(slot, (BYTE *)packet, bucket, entry->recorder, 1 + entry->index_count / 256, record, verifying);
                continue;
            }
            for (uint32_t g = entry->first_group; g != UINT32_MAX; g = build_groups[g].next) {
                const BuiltGroup *group = &build_groups[g];
                Packet *packet = (Packet *)at;
                *packet = (Packet){group->item, group->count, group->flags};
                memcpy(packet->ids, group->ids, group->count * sizeof(uint32_t));
                at += packet_size(group->count);
                add_built_job(slot, (BYTE *)packet, bucket, entry->recorder, 1, record, verifying);
            }
        }
    }
    free(sorted);
}

static ViewSlot *verify_slots[2];

// packetverify: the packets' jobs against those the engine recorded for the same view, field by field.
static void verify_packet_jobs(BYTE *context, const CascadeRecord *record) {
    for (int s = 0; s < 2; s++)
        if (!verify_slots[s] && !(verify_slots[s] = calloc(1, sizeof *verify_slots[s]))) {
            api->log_line("overhaul: out of memory for packetverify");
            __builtin_trap();
        }
    ViewSlot *built = verify_slots[0], *engine_jobs = verify_slots[1];
    build_packet_jobs(built, context, record, TRUE);
    uint32_t bucket_count = *(uint32_t *)(context + 0x1c), saved = 0;
    BYTE *sort_flags = *(BYTE **)(context + 0x30), *recorder_vectors = *(BYTE **)(context + 0x40);
    // The engine's execute sorts these buckets again, and the program sort is not a strict weak ordering: their order goes back.
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++)
        if (sort_flags[bucket]) saved += *(uint32_t *)(recorder_vectors + 16 * bucket + 4);
    BYTE **order = malloc((saved ? saved : 1) * sizeof *order), **at = order;
    if (!order) {
        api->log_line("overhaul: out of memory for packetverify");
        __builtin_trap();
    }
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++)
        if (sort_flags[bucket]) {
            uint32_t count = *(uint32_t *)(recorder_vectors + 16 * bucket + 4);
            memcpy(at, *(BYTE ***)(recorder_vectors + 16 * bucket + 8), count * sizeof *at);
            at += count;
        }
    snapshot_jobs(engine_jobs, context);
    at = order;
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++)
        if (sort_flags[bucket]) {
            uint32_t count = *(uint32_t *)(recorder_vectors + 16 * bucket + 4);
            memcpy(*(BYTE ***)(recorder_vectors + 16 * bucket + 8), at, count * sizeof *at);
            at += count;
        }
    free(order);
    verified_views++;
    uint32_t count = built->job_count > engine_jobs->job_count ? built->job_count : engine_jobs->job_count;
    for (uint32_t i = 0; i < count; i++) {
        const Job *x = i < built->job_count ? &built->jobs[i] : NULL, *y = i < engine_jobs->job_count ? &engine_jobs->jobs[i] : NULL;
        BOOL same = x && y && x->bucket == y->bucket && job_item(x) == job_item(y) && job_instances(x) == job_instances(y) &&
                    job_flags(x) == job_flags(y) && x->group_count == y->group_count && x->group_bit == y->group_bit &&
                    !memcmp(job_ids(x), job_ids(y), job_instances(x) * sizeof(uint32_t));
        if (same) continue;
        if (!differing_views++)
            api->log_line("overhaul: %s %u's packets differ from the engine's record at job %u of %u/%u: bucket %u/%u, item %p/%p (effect vtable "
                          "engine+0x%llx/0x%llx), %u/%u instances, flags 0x%x/0x%x, group %u,%u/%u,%u",
                          record->cascade == UINT32_MAX ? "the effects view of type" : "cascade",
                          record->cascade == UINT32_MAX ? ((ViewTypeFn)method(record->real_view, 0x78))(record->real_view) : record->cascade, i, built->job_count, engine_jobs->job_count, x ? x->bucket : 0, y ? y->bucket : 0, x ? (void *)job_item(x) : NULL,
                          y ? (void *)job_item(y) : NULL, x ? (unsigned long long)(**(BYTE ***)(job_item(x) + 0x10) - engine) : 0,
                          y ? (unsigned long long)(**(BYTE ***)(job_item(y) + 0x10) - engine) : 0, x ? job_instances(x) : 0, y ? job_instances(y) : 0,
                          x ? job_flags(x) : 0, y ? job_flags(y) : 0, x ? x->group_count : 0, x ? x->group_bit : 0, y ? y->group_count : 0, y ? y->group_bit : 0);
        return;
    }
}

// Anything that reads or resets the engine's buckets waits for the record worker first.
static void wait_record(void) {
    Worker *worker = ahead.worker;
    if (!worker) return;
    LONGLONG started = now_ticks();
    while (worker->task) _mm_pause();
    ahead.worker = NULL;
    timers[ab_mode()].ahead_wait += now_ticks() - started;
}

static void append_record(int which, size_t *size, const void *data, size_t bytes) {
    if (*size + bytes > verify_capacity[which]) {
        verify_capacity[which] = 2 * (*size + bytes);
        verify_records[which] = realloc(verify_records[which], verify_capacity[which]);
        if (!verify_records[which]) {
            api->log_line("overhaul: out of memory for %zu bytes of recorded buckets", verify_capacity[which]);
            __builtin_trap();
        }
    }
    memcpy(verify_records[which] + *size, data, bytes);
    *size += bytes;
}

// What a view's record left in the buckets, in bucket and record order: counts, callbacks, each recorder's groups.
static size_t serialize_records(int which, BYTE *context) {
    size_t size = 0;
    uint32_t bucket_count = *(uint32_t *)(context + 0x1c);
    BYTE *recorder_vectors = *(BYTE **)(context + 0x40);
    uint32_t *record_counts = *(uint32_t **)(context + 0x70);
    void **callbacks = *(void ***)(context + 0x80);
    for (uint32_t bucket = 0; bucket < bucket_count; bucket++) {
        if (!record_counts[bucket]) continue;
        BYTE *vector = recorder_vectors + 16 * bucket;
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
            uint32_t used = *(uint32_t *)(recorder + 0x20);
            BYTE **groups = *(BYTE ***)(recorder + 0x18);
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

// At the view's own turn: its record runs again into emptied buckets, and the two records must match.
static void verify_record_ahead(BYTE *view, Timers *timer) {
    BYTE *context = draw_context();
    size_t ahead_size = serialize_records(0, context);
    original_reset_buckets(context);
    original_record_queues(view);
    size_t size = serialize_records(1, context);
    timer->ahead_verified++;
    if (size == ahead_size && !memcmp(verify_records[0], verify_records[1], size)) return;
    if (!timer->ahead_mismatched++)
        api->log_line("overhaul: the record ahead of a view of type 0x%x differs from its own record (%zu vs %zu bytes)",
                      ((ViewTypeFn)method(view, 0x78))(view), ahead_size, size);
}

static void cancel_record_ahead(Timers *timer) {
    wait_record();
    timer->ahead_misses++;
    ahead.view = NULL;
    original_reset_buckets(draw_context());
}

static BOOL wants_parallel_cascades(BYTE *view) {
    return overhaul_parallel_cascades && overhaul_group_gather && overhaul_enabled && !overhaul_record_worker && !overlap_frame && enabled_frames >= WARMUP_FRAMES && !packets_active &&
           (overhaul_views >> 1 & 1) && ((ViewTypeFn)method(view, 0x78))(view) == 1 && *(uint32_t *)(view + 0x94) == 0;
}

static void record_queues_hook(BYTE *view) {
    wait_record();
    CascadeRecord *cascade = kicked_cascade(view);
    if (cascade) {
        finish_cascade_record(view, cascade);
        return;
    }
    if (ahead.view) {
        Timers *timer = &timers[ab_mode()];
        if (ahead.skip_reset || view != ahead.view || (ahead.cascade != UINT32_MAX && *(uint32_t *)(view + 0x94) != ahead.cascade)) {
            cancel_record_ahead(timer);
        } else {
            ahead.view = NULL;
            timer->ahead_hits++;
            if (overhaul_record_verify) verify_record_ahead(view, timer);
            return;
        }
    }
    original_record_queues(view);
    if (wants_parallel_cascades(view)) kick_cascade_records(view);
}

static void reset_buckets_hook(BYTE *context) {
    if (ahead.view && ahead.skip_reset) {
        ahead.skip_reset = FALSE;
        // The cascade loop moves the index on right after this reset; the record restores it when it ends.
        if (ahead.cascade != UINT32_MAX) wait_record();
        return;
    }
    wait_record();
    original_reset_buckets(context);
}

static void effects_record_task(BYTE *argument) {
    CascadeRecord *record = (CascadeRecord *)argument;
    LONGLONG started = now_ticks();
    // The gather's float maths as on main.
    uint32_t mxcsr = _mm_getcsr();
    _mm_setcsr(effects_mxcsr);
    record->worker_packets.queue = 0;
    record->worker_packets.thread = GetCurrentThreadId();
    original_effects_record(record->real_view);
    record->worker_packets.thread = 0;
    _mm_setcsr(mxcsr);
    record->worker_ticks = now_ticks() - started;
}

// As the group's views begin: its active effects views, which the record would reach, on the workers after the drawing
// ones and the record worker.
static void kick_early_effects(BYTE *context) {
    if (!overhaul_early_effects || !overhaul_enabled || overlap_frame || overhaul_parallel_cascades || enabled_frames < WARMUP_FRAMES || effects_pending ||
        early.pending)
        return;
    BYTE *list = *(BYTE **)(context + 0x28), *found[EARLY_EFFECTS];
    uint32_t count = *(uint32_t *)(list + 0x14);
    BYTE **views = *(BYTE ***)(list + 0x18);
    int n = 0;
    for (uint32_t i = 0; i < count && n < EARLY_EFFECTS; i++) {
        BYTE *view = views[i];
        if (!view[0x60] || !view[0x18] || method(view, 0xe0) != engine + EFFECTS_RECORD_RVA) continue;
        if (*(BYTE *)(context + 0x21) && !(*(uint32_t *)((BYTE *(*)(BYTE *))method(view, 0x90))(view) & 8)) continue;
        found[n++] = view;
    }
    int first = (overhaul_workers < 0 ? 0 : overhaul_workers) + (overhaul_record_worker ? 1 : 0);
    if (!n || first + n > MAX_WORKERS || (worker_count < first + n && !start_workers(first + n, engine_immediate_context()))) return;
    for (int e = 0; e < n; e++)
        if (workers[first + e]->task) return;
    for (int e = 0; e < n; e++) {
        if (!effects_records[e] && !(effects_records[e] = calloc(1, sizeof *effects_records[e]))) {
            api->log_line("overhaul: out of memory for an early effects record");
            __builtin_trap();
        }
        CascadeRecord *record = effects_records[e];
        record->real_view = found[e];
        record->cascade = UINT32_MAX;
        record->worker = workers[first + e];
        record->worker_packets.count = record->worker_packets.group_count = record->main_packets.count = record->main_packets.group_count = 0;
        record->worker_packets.unknown_recorder = record->main_packets.unknown_recorder = FALSE;
        record->worker_packets.forward = FALSE;
        record->kicked = TRUE;
    }
    effects_pending = n;
    effects_mxcsr = _mm_getcsr();
    for (int e = 0; e < n; e++) kick_task(effects_records[e]->worker, effects_record_task, (BYTE *)effects_records[e]);
}

// EffectsView::record (0x181b0e980) at the view's turn: what the worker recorded goes into the buckets as the record would
// have put it there.
static void effects_record_hook(BYTE *view) {
    CascadeRecord *record = NULL;
    for (int e = 0; e < EARLY_EFFECTS && !record; e++)
        if (effects_records[e] && effects_records[e]->kicked && effects_records[e]->real_view == view) record = effects_records[e];
    if (!record) {
        original_effects_record(view);
        return;
    }
    int mode = ab_mode();
    LONGLONG started = now_ticks();
    while (record->worker->task) _mm_pause();
    LONGLONG waited = now_ticks();
    record->kicked = FALSE;
    effects_pending--;
    early_effect_views[mode]++;
    early_effect_packets[mode] += record->worker_packets.count;
    early_effect_worker_ticks[mode] += record->worker_ticks;
    early_effect_wait_ticks[mode] += waited - started;
    BYTE *context = draw_context();
    if (overhaul_effects_verify) {
        original_effects_record(view);
        verify_packet_jobs(context, record);
    } else {
        for (uint32_t p = 0; p < record->worker_packets.count; p++) {
            const RecordPacket *packet = &record->worker_packets.packets[p];
            original_push_record(context, packet->effect, packet->index, packet->flags);
        }
    }
    early_effect_replay_ticks[mode] += now_ticks() - waited;
}

static void finish_early_effects(void) {
    for (int e = 0; e < EARLY_EFFECTS; e++)
        if (effects_records[e] && effects_records[e]->kicked) {
            while (effects_records[e]->worker->task) _mm_pause();
            effects_records[e]->kicked = FALSE;
        }
    effects_pending = 0;
}

static void render_views_hook(BYTE *context, BYTE *renderer, BYTE *options, uint64_t face) {
    BYTE *outer = render_context;
    render_context = context;
    kick_early_effects(context);
    original_render_views(context, renderer, options, face);
    render_context = outer;
}

// With the custom loop off, begin_bucket only stores the view type that the view's record already stored. A worker
// skips the store, and so does main while the record worker runs: the next view's record may be walking, and its walks
// read the field.
static uint64_t begin_bucket_hook(BYTE *callbacks, BYTE *argument, BYTE *view) {
    if ((current_worker() || ahead.worker) && !*(BYTE *)(engine + CUSTOM_LOOP_RVA)) return ((ViewTypeFn)method(view, 0x78))(view);
    return original_begin_bucket(callbacks, argument, view);
}

static void render_view_hook(BYTE *view, BYTE *renderer, BYTE *options, BYTE *binding, uint64_t face, uint64_t flag8_only, uint64_t pass_arg) {
    if (ahead.view && view != ahead.view) cancel_record_ahead(&timers[ab_mode()]);
    if (overlap_active) {
        uint32_t type = ((ViewTypeFn)method(view, 0x78))(view);
        if (type >= VIEW_TYPES || !(overhaul_overlap_views >> type & 1)) drain();
    }
    original_render_view(view, renderer, options, binding, face, flag8_only, pass_arg);
}

// end_frame resets the renderer and the context before present; an overlap never spans frames.
static BYTE end_frame_hook(BYTE *render_device) {
    wait_record();
    finish_early_effects();
    finish_all_cascade_records();
    drain();
    render_thread = GetCurrentThreadId();
    overlap_frame = overhaul_overlap && overhaul_enabled;
    return original_end_frame(render_device);
}

static BYTE split_execute(BYTE *context, BYTE *renderer, BYTE *view, int timed_view, int64_t groups, const CascadeRecord *packets);
static BYTE execute_view(BYTE *context, BYTE *renderer, BYTE *view, uint32_t type, uint32_t cascade, int64_t groups, const CascadeRecord *packets);

// The parallel cascade this execute draws from its packets, or NULL once the engine's buckets hold its groups: under
// packetverify (the engine recorded it too), or replayed when the overhaul was switched off since the kick, the engine
// runs it itself, or a recorder kind is not one build_packet_jobs rebuilds.
static const CascadeRecord *take_packets(BYTE *context, BYTE *view) {
    CascadeRecord *record = packet_view;
    if (!record || view != record->real_view || *(uint32_t *)(view + 0x94) != record->cascade) return NULL;
    packet_view = NULL;
    if (record->verify) {
        verify_packet_jobs(context, record);
        return NULL;
    }
    if (!overhaul_enabled || *(BYTE *)(engine + OPTIMIZE_CYCLED_LOCKS_RVA) || record->worker_packets.unknown_recorder || record->main_packets.unknown_recorder) {
        replay_packets(context, record);
        return NULL;
    }
    return record;
}

static BYTE execute_hook(BYTE *context, BYTE *renderer, BYTE *view) {
    if (current_worker()) return original_execute(context, renderer, view);
    const CascadeRecord *packets = take_packets(context, view);
    if (*(BYTE *)(engine + OPTIMIZE_CYCLED_LOCKS_RVA)) return original_execute(context, renderer, view);
    wait_record();
    uint32_t type = ((ViewTypeFn)method(view, 0x78))(view);
    uint32_t cascade = type == 1 ? *(uint32_t *)(view + 0x94) : 0;
    uint32_t type_index = type < VIEW_TYPES ? type : VIEW_TYPES - 1, cascade_index = cascade < STATS_CASCADES ? cascade : STATS_CASCADES - 1;
    ViewStats *stats = &view_stats[ab_mode()][type_index][cascade_index];
    // Something between two views recorded into the buckets the next view's record ahead already filled.
    if (ahead.view && view != ahead.view) cancel_record_ahead(&timers[ab_mode()]);
    LONGLONG started = now_ticks();
    int64_t groups = packets ? 0 : count_jobs(context);
    main_view_type = type_index;
    main_view_cascade = cascade_index;
    current_balance = &balances[type_index][cascade_index];
    BYTE result = execute_view(context, renderer, view, type, cascade, groups, packets);
    main_view_type = VIEW_TYPES;
    current_balance = NULL;
    stats->calls++;
    if (groups > 0) stats->groups += groups;
    stats->ticks += now_ticks() - started;
    return result;
}

static BYTE execute_view(BYTE *context, BYTE *renderer, BYTE *view, uint32_t type, uint32_t cascade, int64_t groups, const CascadeRecord *packets) {
    if (!packets && (type >= VIEW_TYPES || !(overhaul_views >> type & 1) || (type == 1 && (cascade >= 32 || !(overhaul_cascades >> cascade & 1)))))
        return original_execute(context, renderer, view);
    ID3D11DeviceContext *immediate = *(ID3D11DeviceContext **)(renderer + 0x10);
    immediate_context = immediate;
    // An overlapped view's queries would land in main's deferred segments.
    int timed_view = overlap_frame ? -1 : gpu_view_begin(immediate, type, cascade);
    BYTE result;
    if (overhaul_enabled || packets) {
        result = split_execute(context, renderer, view, timed_view, groups, packets);
    } else {
        result = original_execute(context, renderer, view);
        gpu_view_mark(immediate, timed_view, FALSE);
    }
    gpu_view_mark(immediate, timed_view, TRUE);
    return result;
}

static BYTE split_execute(BYTE *context, BYTE *renderer, BYTE *view, int timed_view, int64_t groups, const CascadeRecord *packets) {
    ID3D11DeviceContext *immediate = *(ID3D11DeviceContext **)(renderer + 0x10);
    LONGLONG started = now_ticks();
    Timers *timer = &timers[ab_mode()];
    if (!packets && groups < overhaul_min_jobs) {
        BYTE result = original_execute(context, renderer, view);
        gpu_view_mark(immediate, timed_view, FALSE);
        return result;
    }
    ViewSlot *slot = take_slot();
    slot->timed_frame = NULL;
    slot->group_pool = NULL;
    if (packets) build_packet_jobs(slot, context, packets, FALSE);
    else snapshot_jobs(slot, context);
    // No ranges split from no groups; the engine's empty buckets draw what an empty record would.
    if (!slot->job_count) {
        free_slot();
        BYTE result = original_execute(context, renderer, view);
        gpu_view_mark(immediate, timed_view, FALSE);
        return result;
    }
    int wanted = overhaul_workers < 0 ? 0 : overhaul_workers > MAX_WORKERS ? MAX_WORKERS : overhaul_workers;
    // The record worker is the one after the drawing workers; parallel cascade records take the ones after those.
    int record_worker = overhaul_record_worker && overhaul_record_ahead && wanted > 0 && wanted < MAX_WORKERS && !overlap_frame;
    int spare = overhaul_parallel_cascades && wanted + STATS_CASCADES - 1 <= MAX_WORKERS ? STATS_CASCADES - 1 : record_worker;
    if (spare < record_worker) spare = record_worker;
    if (!overhaul_enabled || enabled_frames < WARMUP_FRAMES || (wanted && !start_workers(wanted + spare, immediate))) wanted = 0;
    if (!wanted) record_worker = 0;
    if (overlap_frame && wanted && !packets && overlap_ready(renderer)) {
        copy_packets(slot, timer);
        return overlap_execute(slot, context, renderer, view, wanted, timer, started);
    }
    uint32_t next_cascade = UINT32_MAX;
    BYTE *next = overhaul_record_ahead && wanted ? record_ahead_target(view, main_view_type, &next_cascade) : NULL;
    // The record ahead empties the buckets that the ranges would otherwise read their groups from.
    if ((overhaul_packets || next) && !packets) copy_packets(slot, timer);
    uint32_t bucket_count = *(uint32_t *)(context + 0x1c);
    double ends[MAX_WORKERS + 1];
    if (overhaul_autobalance) balanced_ends(current_balance, wanted + 1, overhaul_main_share, ends);
    else share_ends(wanted + 1, overhaul_main_share, ends);
    int range_count = slot->range_count = split_ranges(slot, wanted + 1, bucket_count, ends);
    slot->group_pool = current_group_pool;
    for (int k = 0; k < range_count; k++) build_range_context(slot, &slot->ranges[k], context, k ? workers[k - 1] : NULL);
    BOOL values = range_count > 1 && overhaul_value_copies;
    if (values) {
        wrap_writers(slot);
        contexts_split = overhaul_param_contexts && !overlap_frame && prepare_contexts();
        begin_values(&main_values);
        values_active = 1;
    }
    if (range_count > 1) {
        split_thread = GetCurrentThreadId();
        capture_targets(&slot->targets, immediate);
        memcpy(slot->renderer_template, *(BYTE **)(engine + RENDER_DEVICE_RVA) + RENDERER_OFFSET, RENDERER_SIZE);
        slot->view = view;
        slot->view_type = main_view_type;
        slot->overlapped = FALSE;
        slot->timed_frame = gpu_detail_frame(timed_view);
        slot->timed_view = timed_view;
        if (slot->timed_frame) log_targets(main_view_type, main_view_cascade, &slot->targets);
        slot->remaining = range_count - 1;
        for (int k = 1; k < range_count; k++) push_range(workers[k - 1], slot, &slot->ranges[k]);
        wake_workers();
        if (next && record_worker) kick_record_ahead(next, next_cascade, workers[wanted], timer);
    }
    BOOL serial = overhaul_serial_ranges;
    if (serial)
        while (slot->remaining) _mm_pause();
    LONGLONG main_started = now_ticks();
    timer->snapshot += main_started - started;
    BYTE result = original_execute(slot->ranges[0].context, renderer, view);
    main_values.drawing = FALSE;
    main_group.mesh_of = NULL;
    LONGLONG main_done = now_ticks();
    timer->main_range += main_done - main_started;
    gpu_view_mark(immediate, timed_view, FALSE);
    if (next && range_count > 1 && !record_worker) record_ahead(next, next_cascade, timer);
    LONGLONG main_busy = now_ticks();
    timer->main_fraction += slot->ends[0];
    if (range_count > 1) {
        while (slot->remaining) _mm_pause();
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
        BOOL restore = overhaul_restore_state != 0;
        result &= execute_lists(slot, 1, immediate, restore);
        if (slot->timed_frame) ID3D11DeviceContext_End(immediate, (ID3D11Asynchronous *)slot->timed_frame->lists_end[slot->timed_view]);
        if (overhaul_probe_lists) execute_probe_lists(slot, immediate, restore);
        for (int k = 1; k < range_count; k++) forget_uploads(&workers[k - 1]->uploads);
        // FALSE leaves the immediate context in its default state, so the engine's cache must start cold and the
        // targets it set before execute must come back.
        if (!restore) {
            make_cold(renderer);
            bind_targets(immediate, &slot->targets);
        }
        split_thread = 0;
        run_kicks(&main_kicks, renderer);
        for (int k = 1; k < range_count; k++) run_kicks(&workers[k - 1]->kicks, renderer);
        release_targets(&slot->targets);
        timer->replay += now_ticks() - joined;
    }
    free_slot();
    timer->views++;
    timer->jobs += slot->job_count;
    timer->ranges += range_count;
    return result;
}

static double per_frame_ms(LONGLONG ticks, double frames) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return 1000.0 * ticks / frequency.QuadPart / frames;
}

// D3D call counts per context method, per mode: main's immediate context and the workers' deferred contexts, so a
// worker list that differs from what main would have issued shows up method by method. Main's own deferred segments
// (overlap) are counted on the proxy's context, which forwards them. The thunks go in only for an
// A/B run (report start to end), because the runtime may rewrite its own vtable entries.
__attribute__((used)) volatile uint64_t main_calls[2][CONTEXT_METHODS];
__attribute__((used)) void *original_immediate_methods[CONTEXT_METHODS], *original_deferred_methods[CONTEXT_METHODS];
__attribute__((used)) const uint64_t worker_calls_offset = offsetof(Worker, d3d_calls);
static const char *const context_method_names[CONTEXT_METHODS] = {"QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "VSSetConstantBuffers", "PSSetShaderResources", "PSSetShader", "PSSetSamplers", "VSSetShader", "DrawIndexed", "Draw", "Map", "Unmap", "PSSetConstantBuffers", "IASetInputLayout", "IASetVertexBuffers", "IASetIndexBuffer", "DrawIndexedInstanced", "DrawInstanced", "GSSetConstantBuffers", "GSSetShader", "IASetPrimitiveTopology", "VSSetShaderResources", "VSSetSamplers", "Begin", "End", "GetData", "SetPredication", "GSSetShaderResources", "GSSetSamplers", "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews", "OMSetBlendState", "OMSetDepthStencilState", "SOSetTargets", "DrawAuto", "DrawIndexedInstancedIndirect", "DrawInstancedIndirect", "Dispatch", "DispatchIndirect", "RSSetState", "RSSetViewports", "RSSetScissorRects", "CopySubresourceRegion", "CopyResource", "UpdateSubresource", "CopyStructureCount", "ClearRenderTargetView", "ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat", "ClearDepthStencilView", "GenerateMips", "SetResourceMinLOD", "GetResourceMinLOD", "ResolveSubresource", "ExecuteCommandList", "HSSetShaderResources", "HSSetShader", "HSSetSamplers", "HSSetConstantBuffers", "DSSetShaderResources", "DSSetShader", "DSSetSamplers", "DSSetConstantBuffers", "CSSetShaderResources", "CSSetUnorderedAccessViews", "CSSetShader", "CSSetSamplers", "CSSetConstantBuffers", "VSGetConstantBuffers", "PSGetShaderResources", "PSGetShader", "PSGetSamplers", "VSGetShader", "PSGetConstantBuffers", "IAGetInputLayout", "IAGetVertexBuffers", "IAGetIndexBuffer", "GSGetConstantBuffers", "GSGetShader", "IAGetPrimitiveTopology", "VSGetShaderResources", "VSGetSamplers", "GetPredication", "GSGetShaderResources", "GSGetSamplers", "OMGetRenderTargets", "OMGetRenderTargetsAndUnorderedAccessViews", "OMGetBlendState", "OMGetDepthStencilState", "SOGetTargets", "RSGetState", "RSGetViewports", "RSGetScissorRects", "HSGetShaderResources", "HSGetShader", "HSGetSamplers", "HSGetConstantBuffers", "DSGetShaderResources", "DSGetShader", "DSGetSamplers", "DSGetConstantBuffers", "CSGetShaderResources", "CSGetUnorderedAccessViews", "CSGetShader", "CSGetSamplers", "CSGetConstantBuffers", "ClearState", "Flush", "GetType", "GetContextFlags", "FinishCommandList"};

__asm__(".text\n.intel_syntax noprefix\n"
        "count_deferred_call:\n"
        "  mov eax, dword ptr [rip+tls_index]\n"
        "  cmp qword ptr gs:[rax*8+0x1480], 0\n"
        "  je 4f\n"
        "count_immediate_call:\n"
        "  mov eax, dword ptr [rip+tls_index]\n"
        "  mov rax, qword ptr gs:[rax*8+0x1480]\n"
        "  test rax, rax\n"
        "  jz 1f\n"
        "  add rax, qword ptr [rip+worker_calls_offset]\n"
        "  jmp 2f\n"
        "1:\n"
        "  lea rax, [rip+main_calls]\n"
        "2:\n"
        "  mov r11, qword ptr [rip+ab_flag]\n"
        "  cmp dword ptr [r11], 0\n"
        "  je 3f\n"
        "  add rax, 115*8\n"
        "3:\n"
        "  inc qword ptr [rax+r10*8]\n"
        "4:\n"
        "  ret\n"
        ".att_syntax\n");

#define CALL_THUNK(n, kind)                                                                                            \
    __attribute__((naked)) static void kind##_thunk_##n(void) {                                                        \
        __asm__("mov $" #n ", %r10d\n\tcall count_" #kind "_call\n\tmov $" #n ", %r10d\n\tlea original_" #kind "_methods(%rip), %r11\n\tjmp *(%r11,%r10,8)"); \
    }
CALL_THUNK(0, immediate)
CALL_THUNK(1, immediate)
CALL_THUNK(2, immediate)
CALL_THUNK(3, immediate)
CALL_THUNK(4, immediate)
CALL_THUNK(5, immediate)
CALL_THUNK(6, immediate)
CALL_THUNK(7, immediate)
CALL_THUNK(8, immediate)
CALL_THUNK(9, immediate)
CALL_THUNK(10, immediate)
CALL_THUNK(11, immediate)
CALL_THUNK(12, immediate)
CALL_THUNK(13, immediate)
CALL_THUNK(14, immediate)
CALL_THUNK(15, immediate)
CALL_THUNK(16, immediate)
CALL_THUNK(17, immediate)
CALL_THUNK(18, immediate)
CALL_THUNK(19, immediate)
CALL_THUNK(20, immediate)
CALL_THUNK(21, immediate)
CALL_THUNK(22, immediate)
CALL_THUNK(23, immediate)
CALL_THUNK(24, immediate)
CALL_THUNK(25, immediate)
CALL_THUNK(26, immediate)
CALL_THUNK(27, immediate)
CALL_THUNK(28, immediate)
CALL_THUNK(29, immediate)
CALL_THUNK(30, immediate)
CALL_THUNK(31, immediate)
CALL_THUNK(32, immediate)
CALL_THUNK(33, immediate)
CALL_THUNK(34, immediate)
CALL_THUNK(35, immediate)
CALL_THUNK(36, immediate)
CALL_THUNK(37, immediate)
CALL_THUNK(38, immediate)
CALL_THUNK(39, immediate)
CALL_THUNK(40, immediate)
CALL_THUNK(41, immediate)
CALL_THUNK(42, immediate)
CALL_THUNK(43, immediate)
CALL_THUNK(44, immediate)
CALL_THUNK(45, immediate)
CALL_THUNK(46, immediate)
CALL_THUNK(47, immediate)
CALL_THUNK(48, immediate)
CALL_THUNK(49, immediate)
CALL_THUNK(50, immediate)
CALL_THUNK(51, immediate)
CALL_THUNK(52, immediate)
CALL_THUNK(53, immediate)
CALL_THUNK(54, immediate)
CALL_THUNK(55, immediate)
CALL_THUNK(56, immediate)
CALL_THUNK(57, immediate)
CALL_THUNK(58, immediate)
CALL_THUNK(59, immediate)
CALL_THUNK(60, immediate)
CALL_THUNK(61, immediate)
CALL_THUNK(62, immediate)
CALL_THUNK(63, immediate)
CALL_THUNK(64, immediate)
CALL_THUNK(65, immediate)
CALL_THUNK(66, immediate)
CALL_THUNK(67, immediate)
CALL_THUNK(68, immediate)
CALL_THUNK(69, immediate)
CALL_THUNK(70, immediate)
CALL_THUNK(71, immediate)
CALL_THUNK(72, immediate)
CALL_THUNK(73, immediate)
CALL_THUNK(74, immediate)
CALL_THUNK(75, immediate)
CALL_THUNK(76, immediate)
CALL_THUNK(77, immediate)
CALL_THUNK(78, immediate)
CALL_THUNK(79, immediate)
CALL_THUNK(80, immediate)
CALL_THUNK(81, immediate)
CALL_THUNK(82, immediate)
CALL_THUNK(83, immediate)
CALL_THUNK(84, immediate)
CALL_THUNK(85, immediate)
CALL_THUNK(86, immediate)
CALL_THUNK(87, immediate)
CALL_THUNK(88, immediate)
CALL_THUNK(89, immediate)
CALL_THUNK(90, immediate)
CALL_THUNK(91, immediate)
CALL_THUNK(92, immediate)
CALL_THUNK(93, immediate)
CALL_THUNK(94, immediate)
CALL_THUNK(95, immediate)
CALL_THUNK(96, immediate)
CALL_THUNK(97, immediate)
CALL_THUNK(98, immediate)
CALL_THUNK(99, immediate)
CALL_THUNK(100, immediate)
CALL_THUNK(101, immediate)
CALL_THUNK(102, immediate)
CALL_THUNK(103, immediate)
CALL_THUNK(104, immediate)
CALL_THUNK(105, immediate)
CALL_THUNK(106, immediate)
CALL_THUNK(107, immediate)
CALL_THUNK(108, immediate)
CALL_THUNK(109, immediate)
CALL_THUNK(110, immediate)
CALL_THUNK(111, immediate)
CALL_THUNK(112, immediate)
CALL_THUNK(113, immediate)
CALL_THUNK(114, immediate)
CALL_THUNK(0, deferred)
CALL_THUNK(1, deferred)
CALL_THUNK(2, deferred)
CALL_THUNK(3, deferred)
CALL_THUNK(4, deferred)
CALL_THUNK(5, deferred)
CALL_THUNK(6, deferred)
CALL_THUNK(7, deferred)
CALL_THUNK(8, deferred)
CALL_THUNK(9, deferred)
CALL_THUNK(10, deferred)
CALL_THUNK(11, deferred)
CALL_THUNK(12, deferred)
CALL_THUNK(13, deferred)
CALL_THUNK(14, deferred)
CALL_THUNK(15, deferred)
CALL_THUNK(16, deferred)
CALL_THUNK(17, deferred)
CALL_THUNK(18, deferred)
CALL_THUNK(19, deferred)
CALL_THUNK(20, deferred)
CALL_THUNK(21, deferred)
CALL_THUNK(22, deferred)
CALL_THUNK(23, deferred)
CALL_THUNK(24, deferred)
CALL_THUNK(25, deferred)
CALL_THUNK(26, deferred)
CALL_THUNK(27, deferred)
CALL_THUNK(28, deferred)
CALL_THUNK(29, deferred)
CALL_THUNK(30, deferred)
CALL_THUNK(31, deferred)
CALL_THUNK(32, deferred)
CALL_THUNK(33, deferred)
CALL_THUNK(34, deferred)
CALL_THUNK(35, deferred)
CALL_THUNK(36, deferred)
CALL_THUNK(37, deferred)
CALL_THUNK(38, deferred)
CALL_THUNK(39, deferred)
CALL_THUNK(40, deferred)
CALL_THUNK(41, deferred)
CALL_THUNK(42, deferred)
CALL_THUNK(43, deferred)
CALL_THUNK(44, deferred)
CALL_THUNK(45, deferred)
CALL_THUNK(46, deferred)
CALL_THUNK(47, deferred)
CALL_THUNK(48, deferred)
CALL_THUNK(49, deferred)
CALL_THUNK(50, deferred)
CALL_THUNK(51, deferred)
CALL_THUNK(52, deferred)
CALL_THUNK(53, deferred)
CALL_THUNK(54, deferred)
CALL_THUNK(55, deferred)
CALL_THUNK(56, deferred)
CALL_THUNK(57, deferred)
CALL_THUNK(58, deferred)
CALL_THUNK(59, deferred)
CALL_THUNK(60, deferred)
CALL_THUNK(61, deferred)
CALL_THUNK(62, deferred)
CALL_THUNK(63, deferred)
CALL_THUNK(64, deferred)
CALL_THUNK(65, deferred)
CALL_THUNK(66, deferred)
CALL_THUNK(67, deferred)
CALL_THUNK(68, deferred)
CALL_THUNK(69, deferred)
CALL_THUNK(70, deferred)
CALL_THUNK(71, deferred)
CALL_THUNK(72, deferred)
CALL_THUNK(73, deferred)
CALL_THUNK(74, deferred)
CALL_THUNK(75, deferred)
CALL_THUNK(76, deferred)
CALL_THUNK(77, deferred)
CALL_THUNK(78, deferred)
CALL_THUNK(79, deferred)
CALL_THUNK(80, deferred)
CALL_THUNK(81, deferred)
CALL_THUNK(82, deferred)
CALL_THUNK(83, deferred)
CALL_THUNK(84, deferred)
CALL_THUNK(85, deferred)
CALL_THUNK(86, deferred)
CALL_THUNK(87, deferred)
CALL_THUNK(88, deferred)
CALL_THUNK(89, deferred)
CALL_THUNK(90, deferred)
CALL_THUNK(91, deferred)
CALL_THUNK(92, deferred)
CALL_THUNK(93, deferred)
CALL_THUNK(94, deferred)
CALL_THUNK(95, deferred)
CALL_THUNK(96, deferred)
CALL_THUNK(97, deferred)
CALL_THUNK(98, deferred)
CALL_THUNK(99, deferred)
CALL_THUNK(100, deferred)
CALL_THUNK(101, deferred)
CALL_THUNK(102, deferred)
CALL_THUNK(103, deferred)
CALL_THUNK(104, deferred)
CALL_THUNK(105, deferred)
CALL_THUNK(106, deferred)
CALL_THUNK(107, deferred)
CALL_THUNK(108, deferred)
CALL_THUNK(109, deferred)
CALL_THUNK(110, deferred)
CALL_THUNK(111, deferred)
CALL_THUNK(112, deferred)
CALL_THUNK(113, deferred)
CALL_THUNK(114, deferred)

static void *const immediate_thunks[CONTEXT_METHODS] = {(void *)immediate_thunk_0, (void *)immediate_thunk_1, (void *)immediate_thunk_2, (void *)immediate_thunk_3, (void *)immediate_thunk_4, (void *)immediate_thunk_5, (void *)immediate_thunk_6, (void *)immediate_thunk_7, (void *)immediate_thunk_8, (void *)immediate_thunk_9, (void *)immediate_thunk_10, (void *)immediate_thunk_11, (void *)immediate_thunk_12, (void *)immediate_thunk_13, (void *)immediate_thunk_14, (void *)immediate_thunk_15, (void *)immediate_thunk_16, (void *)immediate_thunk_17, (void *)immediate_thunk_18, (void *)immediate_thunk_19, (void *)immediate_thunk_20, (void *)immediate_thunk_21, (void *)immediate_thunk_22, (void *)immediate_thunk_23, (void *)immediate_thunk_24, (void *)immediate_thunk_25, (void *)immediate_thunk_26, (void *)immediate_thunk_27, (void *)immediate_thunk_28, (void *)immediate_thunk_29, (void *)immediate_thunk_30, (void *)immediate_thunk_31, (void *)immediate_thunk_32, (void *)immediate_thunk_33, (void *)immediate_thunk_34, (void *)immediate_thunk_35, (void *)immediate_thunk_36, (void *)immediate_thunk_37, (void *)immediate_thunk_38, (void *)immediate_thunk_39, (void *)immediate_thunk_40, (void *)immediate_thunk_41, (void *)immediate_thunk_42, (void *)immediate_thunk_43, (void *)immediate_thunk_44, (void *)immediate_thunk_45, (void *)immediate_thunk_46, (void *)immediate_thunk_47, (void *)immediate_thunk_48, (void *)immediate_thunk_49, (void *)immediate_thunk_50, (void *)immediate_thunk_51, (void *)immediate_thunk_52, (void *)immediate_thunk_53, (void *)immediate_thunk_54, (void *)immediate_thunk_55, (void *)immediate_thunk_56, (void *)immediate_thunk_57, (void *)immediate_thunk_58, (void *)immediate_thunk_59, (void *)immediate_thunk_60, (void *)immediate_thunk_61, (void *)immediate_thunk_62, (void *)immediate_thunk_63, (void *)immediate_thunk_64, (void *)immediate_thunk_65, (void *)immediate_thunk_66, (void *)immediate_thunk_67, (void *)immediate_thunk_68, (void *)immediate_thunk_69, (void *)immediate_thunk_70, (void *)immediate_thunk_71, (void *)immediate_thunk_72, (void *)immediate_thunk_73, (void *)immediate_thunk_74, (void *)immediate_thunk_75, (void *)immediate_thunk_76, (void *)immediate_thunk_77, (void *)immediate_thunk_78, (void *)immediate_thunk_79, (void *)immediate_thunk_80, (void *)immediate_thunk_81, (void *)immediate_thunk_82, (void *)immediate_thunk_83, (void *)immediate_thunk_84, (void *)immediate_thunk_85, (void *)immediate_thunk_86, (void *)immediate_thunk_87, (void *)immediate_thunk_88, (void *)immediate_thunk_89, (void *)immediate_thunk_90, (void *)immediate_thunk_91, (void *)immediate_thunk_92, (void *)immediate_thunk_93, (void *)immediate_thunk_94, (void *)immediate_thunk_95, (void *)immediate_thunk_96, (void *)immediate_thunk_97, (void *)immediate_thunk_98, (void *)immediate_thunk_99, (void *)immediate_thunk_100, (void *)immediate_thunk_101, (void *)immediate_thunk_102, (void *)immediate_thunk_103, (void *)immediate_thunk_104, (void *)immediate_thunk_105, (void *)immediate_thunk_106, (void *)immediate_thunk_107, (void *)immediate_thunk_108, (void *)immediate_thunk_109, (void *)immediate_thunk_110, (void *)immediate_thunk_111, (void *)immediate_thunk_112, (void *)immediate_thunk_113, (void *)immediate_thunk_114};
static void *const deferred_thunks[CONTEXT_METHODS] = {(void *)deferred_thunk_0, (void *)deferred_thunk_1, (void *)deferred_thunk_2, (void *)deferred_thunk_3, (void *)deferred_thunk_4, (void *)deferred_thunk_5, (void *)deferred_thunk_6, (void *)deferred_thunk_7, (void *)deferred_thunk_8, (void *)deferred_thunk_9, (void *)deferred_thunk_10, (void *)deferred_thunk_11, (void *)deferred_thunk_12, (void *)deferred_thunk_13, (void *)deferred_thunk_14, (void *)deferred_thunk_15, (void *)deferred_thunk_16, (void *)deferred_thunk_17, (void *)deferred_thunk_18, (void *)deferred_thunk_19, (void *)deferred_thunk_20, (void *)deferred_thunk_21, (void *)deferred_thunk_22, (void *)deferred_thunk_23, (void *)deferred_thunk_24, (void *)deferred_thunk_25, (void *)deferred_thunk_26, (void *)deferred_thunk_27, (void *)deferred_thunk_28, (void *)deferred_thunk_29, (void *)deferred_thunk_30, (void *)deferred_thunk_31, (void *)deferred_thunk_32, (void *)deferred_thunk_33, (void *)deferred_thunk_34, (void *)deferred_thunk_35, (void *)deferred_thunk_36, (void *)deferred_thunk_37, (void *)deferred_thunk_38, (void *)deferred_thunk_39, (void *)deferred_thunk_40, (void *)deferred_thunk_41, (void *)deferred_thunk_42, (void *)deferred_thunk_43, (void *)deferred_thunk_44, (void *)deferred_thunk_45, (void *)deferred_thunk_46, (void *)deferred_thunk_47, (void *)deferred_thunk_48, (void *)deferred_thunk_49, (void *)deferred_thunk_50, (void *)deferred_thunk_51, (void *)deferred_thunk_52, (void *)deferred_thunk_53, (void *)deferred_thunk_54, (void *)deferred_thunk_55, (void *)deferred_thunk_56, (void *)deferred_thunk_57, (void *)deferred_thunk_58, (void *)deferred_thunk_59, (void *)deferred_thunk_60, (void *)deferred_thunk_61, (void *)deferred_thunk_62, (void *)deferred_thunk_63, (void *)deferred_thunk_64, (void *)deferred_thunk_65, (void *)deferred_thunk_66, (void *)deferred_thunk_67, (void *)deferred_thunk_68, (void *)deferred_thunk_69, (void *)deferred_thunk_70, (void *)deferred_thunk_71, (void *)deferred_thunk_72, (void *)deferred_thunk_73, (void *)deferred_thunk_74, (void *)deferred_thunk_75, (void *)deferred_thunk_76, (void *)deferred_thunk_77, (void *)deferred_thunk_78, (void *)deferred_thunk_79, (void *)deferred_thunk_80, (void *)deferred_thunk_81, (void *)deferred_thunk_82, (void *)deferred_thunk_83, (void *)deferred_thunk_84, (void *)deferred_thunk_85, (void *)deferred_thunk_86, (void *)deferred_thunk_87, (void *)deferred_thunk_88, (void *)deferred_thunk_89, (void *)deferred_thunk_90, (void *)deferred_thunk_91, (void *)deferred_thunk_92, (void *)deferred_thunk_93, (void *)deferred_thunk_94, (void *)deferred_thunk_95, (void *)deferred_thunk_96, (void *)deferred_thunk_97, (void *)deferred_thunk_98, (void *)deferred_thunk_99, (void *)deferred_thunk_100, (void *)deferred_thunk_101, (void *)deferred_thunk_102, (void *)deferred_thunk_103, (void *)deferred_thunk_104, (void *)deferred_thunk_105, (void *)deferred_thunk_106, (void *)deferred_thunk_107, (void *)deferred_thunk_108, (void *)deferred_thunk_109, (void *)deferred_thunk_110, (void *)deferred_thunk_111, (void *)deferred_thunk_112, (void *)deferred_thunk_113, (void *)deferred_thunk_114};
static void **counted_vtables[MAX_WORKERS + 1];
static int counted_vtable_count;
static const int draw_methods[] = {12, 13, 20, 21, 39, 40};

static void swap_vtable(void **vtable, void *const *entries, void **saved) {
    DWORD protection;
    VirtualProtect(vtable, CONTEXT_METHODS * sizeof *vtable, PAGE_READWRITE, &protection);
    for (int i = 0; i < CONTEXT_METHODS; i++) {
        if (saved) saved[i] = vtable[i];
        InterlockedExchangePointer(&vtable[i], entries[i]);
    }
    VirtualProtect(vtable, CONTEXT_METHODS * sizeof *vtable, protection, &protection);
}

// An entry the runtime replaced during the run keeps its replacement; the calls through it went uncounted.
static int restore_vtable(void **vtable, void *const *thunks, void *const *originals) {
    DWORD protection;
    int replaced = 0;
    VirtualProtect(vtable, CONTEXT_METHODS * sizeof *vtable, PAGE_READWRITE, &protection);
    for (int i = 0; i < CONTEXT_METHODS; i++) {
        if (vtable[i] == thunks[i]) InterlockedExchangePointer(&vtable[i], originals[i]);
        else replaced++;
    }
    VirtualProtect(vtable, CONTEXT_METHODS * sizeof *vtable, protection, &protection);
    return replaced;
}

static BOOL counted(void **vtable) {
    for (int c = 0; c < counted_vtable_count; c++)
        if (counted_vtables[c] == vtable) return TRUE;
    return FALSE;
}

// Every deferred context has a vtable of its own; the deferred thunks jump through one table of originals.
static void start_call_counts(void) {
    memset((void *)main_calls, 0, sizeof main_calls);
    for (int i = 0; i < worker_count; i++) memset(workers[i]->d3d_calls, 0, sizeof workers[i]->d3d_calls);
    if (!immediate_context) return;
    counted_vtables[0] = *(void ***)immediate_context;
    swap_vtable(counted_vtables[0], immediate_thunks, original_immediate_methods);
    counted_vtable_count = 1;
    for (int i = 0; i < worker_count; i++) {
        void **vtable = *(void ***)workers[i]->context;
        if (counted(vtable)) continue;
        if (counted_vtable_count > 1 && memcmp(vtable, original_deferred_methods, sizeof original_deferred_methods)) {
            api->log_line("  D3D call counts: worker %d's context methods differ from the first worker's; its calls go uncounted", workers[i]->number);
            continue;
        }
        swap_vtable(vtable, deferred_thunks, counted_vtable_count == 1 ? original_deferred_methods : NULL);
        counted_vtables[counted_vtable_count++] = vtable;
    }
}

static void stop_call_counts(double frames_off, double frames_on, const double *engine_draws) {
    if (!counted_vtable_count) return;
    for (int i = 0; i < worker_count; i++)
        if (!counted(*(void ***)workers[i]->context))
            api->log_line("  D3D call counts: worker %d started during the run; its calls went uncounted", workers[i]->number);
    for (int c = 0; c < counted_vtable_count; c++) {
        int replaced = restore_vtable(counted_vtables[c], c ? deferred_thunks : immediate_thunks, c ? original_deferred_methods : original_immediate_methods);
        if (replaced) api->log_line("  D3D call counts: the runtime replaced %d of %d methods of context vtable %d during the run", replaced, CONTEXT_METHODS, c);
    }
    counted_vtable_count = 0;
    double worker_calls[2][CONTEXT_METHODS] = {{0}};
    for (int i = 0; i < worker_count; i++)
        for (int mode = 0; mode < 2; mode++)
            for (int m = 0; m < CONTEXT_METHODS; m++) worker_calls[mode][m] += workers[i]->d3d_calls[mode][m];
    double d3d_draws[2] = {0, 0}, frames[2] = {frames_off, frames_on};
    for (int mode = 0; mode < 2; mode++)
        for (size_t d = 0; d < sizeof draw_methods / sizeof *draw_methods; d++)
            d3d_draws[mode] += (main_calls[mode][draw_methods[d]] + worker_calls[mode][draw_methods[d]]) / frames[mode];
    api->log_line("  D3D draws counted per frame: off %.0f of the engine's %.0f | on %.0f of %.0f", d3d_draws[0], engine_draws[0], d3d_draws[1], engine_draws[1]);
    api->log_line("  D3D calls per frame that differ (off -> on: main + workers):");
    for (int m = 0; m < CONTEXT_METHODS; m++) {
        double off = (main_calls[0][m] + worker_calls[0][m]) / frames_off, main_on = main_calls[1][m] / frames_on, workers_on = worker_calls[1][m] / frames_on;
        double on = main_on + workers_on, difference = on - off;
        if (fabs(difference) > 5 && fabs(difference) > 0.05 * off)
            api->log_line("    %-36s %9.1f -> %9.1f (%.1f + %.1f)", context_method_names[m], off, on, main_on, workers_on);
    }
}

static void log_view_stats(double frames_off, double frames_on) {
    api->log_line("  main ms per frame inside execute, by view (off | on):");
    BOOL shown[VIEW_TYPES][STATS_CASCADES] = {{0}};
    for (int row = 0; row < 16; row++) {
        int best_type = -1, best_cascade = 0;
        LONGLONG best = 0;
        for (int type = 0; type < VIEW_TYPES; type++)
            for (int cascade = 0; cascade < STATS_CASCADES; cascade++) {
                LONGLONG ticks = view_stats[0][type][cascade].ticks + view_stats[1][type][cascade].ticks;
                if (!shown[type][cascade] && ticks > best) best = ticks, best_type = type, best_cascade = cascade;
            }
        if (best_type < 0) break;
        shown[best_type][best_cascade] = TRUE;
        const ViewStats *off = &view_stats[0][best_type][best_cascade], *on = &view_stats[1][best_type][best_cascade];
        char name[48];
        view_name(name, sizeof name, best_type, best_cascade);
        api->log_line("    %-34s %7.3f | %7.3f   %5.1f calls, %6.0f groups per frame", name, per_frame_ms(off->ticks, frames_off), per_frame_ms(on->ticks, frames_on),
                      off->calls / frames_off, off->groups / frames_off);
    }
}

// The views whose draws or instances differ between the modes, beyond the parity tolerance.
static void log_view_work(const double *frames) {
    api->log_line("  overhaul work by view that differs (draws, instances per frame, off | on):");
    for (int type = 0; type <= VIEW_TYPES; type++) {
        double draws[2] = {0, 0}, instances[2] = {0, 0};
        for (int mode = 0; mode < 2; mode++) {
            uint64_t view_draws = view_work[mode][type].draws, view_instances = view_work[mode][type].instances;
            for (int i = 0; i < worker_count; i++) {
                view_draws += workers[i]->view_work[mode][type].draws;
                view_instances += workers[i]->view_work[mode][type].instances;
            }
            draws[mode] = view_draws / frames[mode];
            instances[mode] = view_instances / frames[mode];
        }
        if (fabs(draws[1] - draws[0]) <= PARITY_TOLERANCE * draws[0] + 1 && fabs(instances[1] - instances[0]) <= PARITY_TOLERANCE * instances[0] + 1) continue;
        const char *name = type == VIEW_TYPES ? "outside execute" : view_type_names[type] ? view_type_names[type] : "?";
        api->log_line("    %-26s %7.0f, %8.0f | %7.0f, %8.0f", name, draws[0], instances[0], draws[1], instances[1]);
    }
}

// The view's GPU time per frame by mode; for its split frames, main's part, the wait before the first list, the lists and
// what follows them (probe lists, rebinding, texture kicks), and per list the time from before its execute to its first
// command, inside it, and from its last command to the next list. Pipeline statistics per frame for the whole view.
static void log_gpu_detail(void) {
    if (!gpu_detail_frames[0] && !gpu_detail_frames[1]) return;
    double frames[2], detail_frames[2];
    for (int mode = 0; mode < 2; mode++) {
        frames[mode] = gpu_timed_frames[mode] ? gpu_timed_frames[mode] : 1;
        detail_frames[mode] = gpu_detail_frames[mode] ? gpu_detail_frames[mode] : 1;
    }
    api->log_line("  GPU ms per frame by view (%s off | on); on, split: main's part + wait + lists + after | per list: before its first command, inside, after "
                  "its last (%llu+%llu frames with detail)",
                  ab_name, (unsigned long long)gpu_detail_frames[0], (unsigned long long)gpu_detail_frames[1]);
    for (int type = 0; type < VIEW_TYPES; type++)
        for (int cascade = 0; cascade < STATS_CASCADES; cascade++) {
            const GpuDetail *off = &gpu_detail[0][type][cascade], *on = &gpu_detail[1][type][cascade];
            if (!off->views && !on->views) continue;
            char name[48], split[128] = "", per_list[96] = "";
            view_name(name, sizeof name, type, cascade);
            if (on->split_views)
                snprintf(split, sizeof split, " | %.3f + %.3f + %.1f lists %.3f + %.3f", on->main_ms / frames[1], on->gap_ms / detail_frames[1],
                         (double)on->lists / on->split_views, on->lists_ms / detail_frames[1], on->after_ms / detail_frames[1]);
            if (on->inside_lists)
                snprintf(per_list, sizeof per_list, " | %.3f, %.3f, %.3f", on->entry_ms / on->inside_lists, on->inside_ms / on->inside_lists,
                         on->exit_ms / on->inside_lists);
            api->log_line("    %-26s %6.3f | %6.3f%s%s", name, off->view_ms / frames[0], on->view_ms / frames[1], split, per_list);
        }
    api->log_line("  GPU pipeline statistics per frame by view (%s off | on), thousands: vertices shaded, primitives rasterized, pixels shaded", ab_name);
    for (int type = 0; type < VIEW_TYPES; type++)
        for (int cascade = 0; cascade < STATS_CASCADES; cascade++) {
            const D3D11_QUERY_DATA_PIPELINE_STATISTICS *off = &gpu_detail[0][type][cascade].statistics, *on = &gpu_detail[1][type][cascade].statistics;
            if (!off->VSInvocations && !on->VSInvocations) continue;
            char name[48];
            view_name(name, sizeof name, type, cascade);
            api->log_line("    %-26s VS %8.1f | %8.1f, raster %7.1f | %7.1f, PS %9.1f | %9.1f", name, off->VSInvocations / detail_frames[0] / 1000,
                          on->VSInvocations / detail_frames[1] / 1000, off->CPrimitives / detail_frames[0] / 1000, on->CPrimitives / detail_frames[1] / 1000,
                          off->PSInvocations / detail_frames[0] / 1000, on->PSInvocations / detail_frames[1] / 1000);
        }
    if (gpu_detail_lost || gpu_inside_lost)
        api->log_line("  GPU detail left out: %llu views' queries not ready, %llu views' in-list timestamps not ready", (unsigned long long)gpu_detail_lost,
                      (unsigned long long)gpu_inside_lost);
}

static uint64_t vtable_rva(const BYTE *vtable) {
    return vtable >= engine && vtable < engine + 0x4000000 ? (uint64_t)(vtable - engine) : (uint64_t)(uintptr_t)vtable;
}

#define TOP_GROUPS 24
#define TOP_CLASSES 12

// Per draw item and view: how often a sampled frame drew it in main's range and on a worker, and the pixels, vertices
// and instances per draw there. An item that shades far more pixels per instance on a worker than in main's range is
// drawn with state the worker lacks.
static void log_group_stats(void) {
    if (!group_frames[0] && !group_frames[1]) return;
    api->log_line("  groups by pixels shaded per sampled frame (groupstats; %s off %llu | on %llu sampled frames; %llu samples over a pool, %llu over the "
                  "table, %llu pools unread):",
                  ab_name, (unsigned long long)group_frames[0], (unsigned long long)group_frames[1], (unsigned long long)group_overflow,
                  (unsigned long long)group_dropped, (unsigned long long)group_lost);
    const GroupStat *top[TOP_GROUPS] = {0};
    double top_pixels[TOP_GROUPS] = {0};
    for (int i = 0; i < GROUP_TABLE_SIZE; i++) {
        const GroupStat *stat = &group_stats[i];
        if (!stat->item) continue;
        double pixels = (stat->pixels[0] + stat->pixels[1]) / (group_frames[stat->mode] ? group_frames[stat->mode] : 1);
        int at = TOP_GROUPS;
        while (at > 0 && pixels > top_pixels[at - 1]) at--;
        if (at == TOP_GROUPS) continue;
        memmove(&top[at + 1], &top[at], (TOP_GROUPS - at - 1) * sizeof *top);
        memmove(&top_pixels[at + 1], &top_pixels[at], (TOP_GROUPS - at - 1) * sizeof *top_pixels);
        top[at] = stat;
        top_pixels[at] = pixels;
    }
    api->log_line("    %-3s %-22s %-8s %-14s %5s | main: per frame, px, vertices, instances | workers: per frame, px, vertices, instances, forced, first", "mode", "view",
                  "effect", "item", "flags");
    for (int t = 0; t < TOP_GROUPS && top[t]; t++) {
        const GroupStat *stat = top[t];
        double frames = group_frames[stat->mode] ? group_frames[stat->mode] : 1, per[2][4] = {{0}};
        for (int w = 0; w < 2; w++)
            if (stat->samples[w]) {
                per[w][0] = stat->samples[w] / frames;
                per[w][1] = stat->pixels[w] / stat->samples[w];
                per[w][2] = stat->vertices[w] / stat->samples[w];
                per[w][3] = stat->instances[w] / stat->samples[w];
            }
        char name[48];
        view_name(name, sizeof name, stat->view_key / STATS_CASCADES, stat->view_key % STATS_CASCADES);
        api->log_line("    %-3s %-22s %08llx %p 0x%03x | %4.2f, %10.0f, %8.0f, %5.0f | %4.2f, %10.0f, %8.0f, %5.0f, %3.0f%%, %3.0f%%", stat->mode ? "on" : "off", name,
                      (unsigned long long)vtable_rva(stat->effect_vtable), (void *)stat->item, stat->flags, per[0][0], per[0][1], per[0][2], per[0][3], per[1][0],
                      per[1][1], per[1][2], per[1][3], stat->samples[1] ? 100.0 * stat->forced[1] / stat->samples[1] : 0,
                      stat->samples[1] ? 100.0 * stat->first[1] / stat->samples[1] : 0);
    }
    // The same by effect class (vtable) and view type, pixels per sampled frame and per instance.
    static struct {
        const BYTE *vtable;
        uint32_t type, mode;
        double pixels[2], instances[2], groups[2];
    } classes[512];
    int class_count = 0;
    for (int i = 0; i < GROUP_TABLE_SIZE; i++) {
        const GroupStat *stat = &group_stats[i];
        if (!stat->item) continue;
        uint32_t type = stat->view_key / STATS_CASCADES;
        int c = 0;
        while (c < class_count && !(classes[c].vtable == stat->effect_vtable && classes[c].type == type && classes[c].mode == stat->mode)) c++;
        if (c == class_count) {
            if (class_count == 512) continue;
            memset(&classes[class_count], 0, sizeof classes[0]);
            classes[class_count].vtable = stat->effect_vtable;
            classes[class_count].type = type;
            classes[class_count].mode = stat->mode;
            class_count++;
        }
        double frames = group_frames[stat->mode] ? group_frames[stat->mode] : 1;
        for (int w = 0; w < 2; w++) {
            classes[c].pixels[w] += stat->pixels[w] / frames;
            classes[c].instances[w] += stat->instances[w] / frames;
            classes[c].groups[w] += stat->samples[w] / frames;
        }
    }
    api->log_line("    by effect class and view type: pixels per frame (groups, pixels per instance) | main | workers");
    for (int t = 0; t < TOP_CLASSES; t++) {
        int best = -1;
        for (int c = 0; c < class_count; c++)
            if (best < 0 || classes[c].pixels[0] + classes[c].pixels[1] > classes[best].pixels[0] + classes[best].pixels[1]) best = c;
        if (best < 0 || classes[best].pixels[0] + classes[best].pixels[1] <= 0) break;
        char name[48];
        view_name(name, sizeof name, classes[best].type, 0);
        api->log_line("    %-3s %-22s %08llx | %10.0f (%6.1f, %7.1f) | %10.0f (%6.1f, %7.1f)", classes[best].mode ? "on" : "off", classes[best].type == 1 ? "SHADOW_MAP" : name,
                      (unsigned long long)vtable_rva(classes[best].vtable), classes[best].pixels[0], classes[best].groups[0],
                      classes[best].instances[0] ? classes[best].pixels[0] / classes[best].instances[0] : 0, classes[best].pixels[1], classes[best].groups[1],
                      classes[best].instances[1] ? classes[best].pixels[1] / classes[best].instances[1] : 0);
        classes[best].pixels[0] = classes[best].pixels[1] = -1;
    }
}

static void report_switch(const char *name, volatile LONG *flag, BOOL start) {
    if (start) {
        ab_flag = flag;
        ab_name = name;
        memset(work, 0, sizeof work);
        memset(view_work, 0, sizeof view_work);
        memset(timers, 0, sizeof timers);
        memset(view_stats, 0, sizeof view_stats);
        commit_wait_ticks = 0;
        main_values.calls = main_values.unlocked_calls = main_values.restores = main_values.seeds = main_values.copies_out = main_values.unwrapped_touches =
            main_values.full = 0;
        main_values.wait_ticks = 0;
        gpu_view_ms[0] = gpu_view_ms[1] = gpu_main_ms = gpu_lists_ms = 0;
        gpu_timed_frames[0] = gpu_timed_frames[1] = gpu_lost_frames = 0;
        memset(gpu_detail, 0, sizeof gpu_detail);
        gpu_detail_frames[0] = gpu_detail_frames[1] = gpu_detail_lost = gpu_inside_lost = 0;
        probe_lists_run[0] = probe_lists_run[1] = 0;
        memset(group_stats, 0, sizeof group_stats);
        record_worker_ticks[0] = record_worker_ticks[1] = 0;
        memset(parallel_cascade_count, 0, sizeof parallel_cascade_count);
        memset(packet_count, 0, sizeof packet_count);
        memset(replayed_views, 0, sizeof replayed_views);
        memset(parallel_worker_ticks, 0, sizeof parallel_worker_ticks);
        memset(parallel_wait_ticks, 0, sizeof parallel_wait_ticks);
        memset(parallel_main_ticks, 0, sizeof parallel_main_ticks);
        verified_views = differing_views = 0;
        memset(early_effect_views, 0, sizeof early_effect_views);
        memset(early_effect_packets, 0, sizeof early_effect_packets);
        memset(early_effect_worker_ticks, 0, sizeof early_effect_worker_ticks);
        memset(early_effect_wait_ticks, 0, sizeof early_effect_wait_ticks);
        memset(early_effect_replay_ticks, 0, sizeof early_effect_replay_ticks);
        escaped_touches = 0;
        main_values.context_calls = main_values.leaks = main_values.demotions = 0;
        main_values.seed_ticks = 0;
        group_frames[0] = group_frames[1] = group_dropped = group_lost = group_overflow = 0;
        start_call_counts();
        for (int i = 0; i < worker_count; i++) {
            memset(workers[i]->work, 0, sizeof workers[i]->work);
            memset(workers[i]->view_work, 0, sizeof workers[i]->view_work);
            memset(workers[i]->renames, 0, sizeof workers[i]->renames);
            workers[i]->busy_ticks = 0;
            workers[i]->texture_sets = 0;
            ValueState *values = &workers[i]->values;
            values->calls = values->unlocked_calls = values->restores = values->seeds = values->copies_out = values->unwrapped_touches = values->full = 0;
            values->wait_ticks = 0;
            values->context_calls = values->leaks = values->demotions = 0;
            values->seed_ticks = 0;
        }
        return;
    }
    WorkCounts totals[2] = {work[0], work[1]};
    for (int i = 0; i < worker_count; i++)
        for (int mode = 0; mode < 2; mode++) {
            totals[mode].draws += workers[i]->work[mode].draws;
            totals[mode].instances += workers[i]->work[mode].instances;
            totals[mode].elements += workers[i]->work[mode].elements;
        }
    double draws[2], instances[2], elements[2], frames[2];
    for (int mode = 0; mode < 2; mode++) {
        frames[mode] = work[mode].frames ? work[mode].frames : 1;
        draws[mode] = totals[mode].draws / frames[mode];
        instances[mode] = totals[mode].instances / frames[mode];
        elements[mode] = totals[mode].elements / frames[mode];
    }
    api->log_line("  overhaul work per frame, %s off: %.0f draws, %.0f instances (%llu frames) | on: %.0f draws, %.0f instances (%llu frames)", ab_name, draws[0],
                  instances[0], (unsigned long long)work[0].frames, draws[1], instances[1], (unsigned long long)work[1].frames);
    api->log_line("  overhaul indices/vertices x instances per frame: off %.3g | on %.3g", elements[0], elements[1]);
    BOOL same = work[0].frames && work[1].frames && draws[0] && instances[0] && fabs(draws[1] / draws[0] - 1) <= PARITY_TOLERANCE &&
                fabs(instances[1] / instances[0] - 1) <= PARITY_TOLERANCE;
    api->log_line("  overhaul work parity: %s", same ? "OK" : "PARITY FAIL (the fps difference does not count)");
    log_view_work(frames);
    RingRenames worker_renames[2] = {{0}};
    for (int i = 0; i < worker_count; i++)
        for (int mode = 0; mode < 2; mode++) {
            worker_renames[mode].renames += workers[i]->renames[mode].renames;
            worker_renames[mode].bytes += workers[i]->renames[mode].bytes;
        }
    api->log_line("  ring DISCARDs per frame (each hands out a whole buffer): main off %.1f, %.1f MB | on %.1f, %.1f MB; workers off %.1f, %.1f MB | on %.1f, %.1f MB",
                  timers[0].renames.renames / frames[0], timers[0].renames.bytes / frames[0] / 1048576, timers[1].renames.renames / frames[1],
                  timers[1].renames.bytes / frames[1] / 1048576, worker_renames[0].renames / frames[0], worker_renames[0].bytes / frames[0] / 1048576,
                  worker_renames[1].renames / frames[1], worker_renames[1].bytes / frames[1] / 1048576);
    for (int mode = 0; mode < 2; mode++) {
        const Timers *timer = &timers[mode];
        if (!timer->views) continue;
        api->log_line("  overhaul per %s-%s frame: %.1f views split, %.0f groups, %.1f ranges, %.0f KB packets | main ms: snapshot %.3f, own range %.3f, wait %.3f, "
                      "replay %.3f | range spread %.3f",
                      ab_name, mode ? "on" : "off", timer->views / frames[mode], timer->jobs / frames[mode], (double)timer->ranges / timer->views,
                      timer->packet_bytes / frames[mode] / 1024, per_frame_ms(timer->snapshot, frames[mode]), per_frame_ms(timer->main_range, frames[mode]),
                      per_frame_ms(timer->wait, frames[mode]), per_frame_ms(timer->replay, frames[mode]), per_frame_ms(timer->spread, frames[mode]));
    }
    for (int mode = 0; mode < 2; mode++) {
        const Timers *timer = &timers[mode];
        if (!timer->views) continue;
        api->log_line("  split views per %s-%s frame: main's share of their weight %.1f%% | recorded ahead %.1f views (%.1f used, %.1f cancelled), main %.3f ms, "
                      "record worker %.3f ms, main waiting for it %.3f ms; %llu checked, %llu differed",
                      ab_name, mode ? "on" : "off", 100 * timer->main_fraction / timer->views, timer->ahead_records / frames[mode],
                      timer->ahead_hits / frames[mode], timer->ahead_misses / frames[mode], per_frame_ms(timer->ahead, frames[mode]),
                      per_frame_ms(record_worker_ticks[mode], frames[mode]), per_frame_ms(timer->ahead_wait, frames[mode]),
                      (unsigned long long)timer->ahead_verified, (unsigned long long)timer->ahead_mismatched);
    }
    for (int mode = 0; mode < 2; mode++) {
        const Timers *timer = &timers[mode];
        if (!timer->overlap_views) continue;
        api->log_line("  overlap per %s-%s frame: %.1f views kicked, %.1f main segments, %.1f drains, at most %llu views in flight | main ms: kick %.3f, "
                      "submit %.3f, drain %.3f",
                      ab_name, mode ? "on" : "off", timer->overlap_views / frames[mode], timer->segments / frames[mode], timer->drains / frames[mode],
                      (unsigned long long)timer->peak_in_flight, per_frame_ms(timer->kick, frames[mode]), per_frame_ms(timer->submit, frames[mode]),
                      per_frame_ms(timer->drain, frames[mode]));
    }
    api->log_line("  main's constant buffer uploads on the immediate context, ms per frame (where the driver stalled): off %.3f | on %.3f",
                  per_frame_ms(timers[0].cb_upload, frames[0]), per_frame_ms(timers[1].cb_upload, frames[1]));
    log_view_stats(frames[0], frames[1]);
    double timed_off = gpu_timed_frames[0] ? gpu_timed_frames[0] : 1, timed_on = gpu_timed_frames[1] ? gpu_timed_frames[1] : 1;
    api->log_line("  overhaul GPU ms per frame in splittable views: off %.3f | on %.3f (main's range %.3f, workers' lists %.3f) | %llu+%llu frames timed, %llu lost",
                  gpu_view_ms[0] / timed_off, gpu_view_ms[1] / timed_on, gpu_main_ms / timed_on, gpu_lists_ms / timed_on,
                  (unsigned long long)gpu_timed_frames[0], (unsigned long long)gpu_timed_frames[1], (unsigned long long)gpu_lost_frames);
    log_gpu_detail();
    log_group_stats();
    for (int mode = 0; mode < 2; mode++)
        if (parallel_cascade_count[mode])
            api->log_line("  parallel cascades per %s-%s frame: %.2f recorded on workers (%.0f packets), worker record %.3f ms, main waited %.3f ms for them "
                          "and recorded its own queues %.3f ms",
                          ab_name, mode ? "on" : "off", parallel_cascade_count[mode] / frames[mode], packet_count[mode] / frames[mode],
                          per_frame_ms(parallel_worker_ticks[mode], frames[mode]), per_frame_ms(parallel_wait_ticks[mode], frames[mode]),
                          per_frame_ms(parallel_main_ticks[mode], frames[mode]));
    for (int mode = 0; mode < 2; mode++)
        if (replayed_views[mode]) api->log_line("  parallel cascades replayed into the engine on %s-%s: %llu", ab_name, mode ? "on" : "off", (unsigned long long)replayed_views[mode]);
    for (int mode = 0; mode < 2; mode++)
        if (early_effect_views[mode])
            api->log_line("  early effects per %s-%s frame: %.2f views recorded on workers (%.0f packets), worker record %.3f ms, main waited %.3f ms and "
                          "replayed %.3f ms",
                          ab_name, mode ? "on" : "off", early_effect_views[mode] / frames[mode], early_effect_packets[mode] / frames[mode],
                          per_frame_ms(early_effect_worker_ticks[mode], frames[mode]), per_frame_ms(early_effect_wait_ticks[mode], frames[mode]),
                          per_frame_ms(early_effect_replay_ticks[mode], frames[mode]));
    if (verified_views)
        api->log_line("  packetverify / effectsverify: %llu views checked, %llu differed", (unsigned long long)verified_views, (unsigned long long)differing_views);
    if (probe_lists_run[0] || probe_lists_run[1])
        api->log_line("  probe lists per frame (%s): off %.1f | on %.1f, %s", overhaul_probe_bind ? "each binding the view's targets" : "empty", probe_lists_run[0] / frames[0],
                      probe_lists_run[1] / frames[1], probe_error ? "stopped by an error" : "all ran");
    // Workers split only in on-frames of an overhaul A/B, and in every frame of the others.
    double busy_frames = flag == &overhaul_enabled ? frames[1] : frames[0] + frames[1];
    if (overhaul_commit_mode)
        api->log_line("  overhaul commit lock: %.3f ms waiting per frame, all threads", per_frame_ms(commit_wait_ticks, busy_frames));
    for (int i = 0; i < worker_count; i++) {
        uint32_t wraps = 0;
        char sizes[MAX_CLONES * 24] = "";
        for (int c = 0; c < workers[i]->clone_count; c++) {
            CloneBuffers *buffers = clone_buffers(workers[i]->clones[c].copy + RING_HOLDER_SIZE);
            wraps += buffers->wraps;
            snprintf(sizes + strlen(sizes), sizeof sizes - strlen(sizes), " %u KB (%u grows)", buffers->small_bytes / 1024, buffers->grows);
        }
        if (wraps) api->log_line("  overhaul worker %d: %u staging wraps (draws read data written after them)", workers[i]->number, wraps);
        api->log_line("  overhaul worker %d: ring clones of%s", workers[i]->number, sizes);
        api->log_line("  overhaul worker %d: busy %.3f ms per splitting frame, %d ring clones, %u program copies, %u constant buffer copies, %llu shared holders, "
                      "%llu dropped texture kicks, %.0f texture sets in its own slots per frame",
                      workers[i]->number, per_frame_ms(workers[i]->busy_ticks, busy_frames), workers[i]->clone_count, workers[i]->program_count,
                      workers[i]->uploads.copy_count, (unsigned long long)workers[i]->shared_holders, (unsigned long long)workers[i]->kicks.dropped,
                      workers[i]->texture_sets / busy_frames);
    }
    for (int i = -1; i < worker_count; i++) {
        const ValueState *state = i < 0 ? &main_values : &workers[i]->values;
        if (!state->calls && !state->unwrapped_touches) continue;
        api->log_line("  valuecopies, %s %d: %.0f locked writer calls (%.0f unlocked), %.0f restores, %.0f seeds, %.0f buffers copied out, %.0f touches "
                      "outside a wrapped writer per frame, %.3f ms waiting for the lock, %llu buffers past the table",
                      i < 0 ? "main" : "worker", i < 0 ? 0 : workers[i]->number, state->calls / busy_frames, state->unlocked_calls / busy_frames,
                      state->restores / busy_frames, state->seeds / busy_frames,
                      state->copies_out / busy_frames, state->unwrapped_touches / busy_frames, per_frame_ms(state->wait_ticks, busy_frames),
                      (unsigned long long)state->full);
    }
    if (unwrapped_caller_count) {
        char callers[MAX_UNWRAPPED_CALLERS * 24] = "";
        size_t used = 0;
        for (LONG i = 0; i < unwrapped_caller_count && i < MAX_UNWRAPPED_CALLERS; i++)
            used += (size_t)snprintf(callers + used, sizeof callers - used, " engine+0x%llx", (unsigned long long)((BYTE *)unwrapped_callers[i] - engine));
        api->log_line("  valuecopies: %ld effect classes wrapped; touches outside a wrapped writer came from%s", wrapped_class_count, callers);
    } else if (wrapped_class_count) {
        api->log_line("  valuecopies: %ld effect classes wrapped; every touch on a drawing thread came from a wrapped writer", wrapped_class_count);
    }
    if (binding_info_count) {
        LONG varying = 0;
        for (int i = 0; i < BINDING_INFOS; i++) varying += binding_infos[i].binding && binding_infos[i].varies;
        api->log_line("  valuecopies: %ld constant buffers written in split views, %ld of them varying", binding_info_count, varying);
    }
    log_contexts(busy_frames);
    if (main_uploads.copy_count) api->log_line("  overhaul main: %u constant buffer copies for its deferred segments", main_uploads.copy_count);
    if (main_kicks.dropped) api->log_line("  overhaul main: %llu dropped texture kicks", (unsigned long long)main_kicks.dropped);
    if (worker_error) api->log_line("  overhaul workers unavailable: %s", worker_error);
    stop_call_counts(frames[0], frames[1], draws);
    ab_flag = &overhaul_enabled;
    ab_name = "overhaul";
}

static void log_callers(const char *what, void *const *callers, LONG count) {
    char text[MAX_LEAK_CALLERS * 24] = "";
    size_t used = 0;
    for (LONG i = 0; i < count && i < MAX_LEAK_CALLERS; i++)
        used += (size_t)snprintf(text + used, sizeof text - used, " engine+0x%llx", (unsigned long long)((BYTE *)callers[i] - engine));
    api->log_line("  paramcontexts: %s%s", what, text);
}

static void log_contexts(double busy_frames) {
    for (int i = -1; i < worker_count; i++) {
        const ValueState *state = i < 0 ? &main_values : &workers[i]->values;
        if (!state->context_calls && !state->leaks) continue;
        api->log_line("  paramcontexts, %s %d: %.0f writer calls in its library copy, %.1f stores taken from the shared values, %llu slots moved to the "
                      "lock, seeding %.3f ms per frame",
                      i < 0 ? "main" : "worker", i < 0 ? 0 : workers[i]->number, state->context_calls / busy_frames, state->leaks / busy_frames,
                      (unsigned long long)state->demotions, per_frame_ms(state->seed_ticks, busy_frames));
    }
    if (!contexts.record_count || !main_values.library || main_values.library_epoch != contexts.epoch) return;
    // A copy is made once per library build: what else in the library changes since then shows here.
    uint32_t differing = 0, shown = 0;
    char offsets[8 * 12] = "";
    for (uint32_t q = 0; q < LIBRARY_SIZE / 8; q++) {
        if (contexts.volatile_qwords[q] || ((uint64_t *)contexts.shared)[q] == ((uint64_t *)main_values.library)[q]) continue;
        differing++;
        if (shown < 8) shown++, snprintf(offsets + strlen(offsets), sizeof offsets - strlen(offsets), " +0x%x", q * 8);
    }
    api->log_line("  paramcontexts: %u parameters in %u buffers (%u bytes) and %ld texture slots copied per drawing thread; %u qwords of the library "
                  "outside them differ from main's copy%s; %ld touches of a copy's parameter outside its writers",
                  contexts.record_count, contexts.buffer_count, contexts.data_bytes, contexts.slot_count, differing, offsets, escaped_touches);
    if (leak_caller_count) log_callers("stores into the shared values came from", leak_callers, leak_caller_count);
    if (escape_caller_count) log_callers("touches of a copy outside its writers came from", escape_callers, escape_caller_count);
}

static void overhaul_report(BOOL start) {
    report_switch("overhaul", &overhaul_enabled, start);
}

static void packets_report(BOOL start) {
    report_switch("packets", &overhaul_packets, start);
}

static void cb_copies_report(BOOL start) {
    report_switch("cbcopies", &overhaul_cb_copies, start);
}

static void autobalance_report(BOOL start) {
    report_switch("autobalance", &overhaul_autobalance, start);
}

static void record_ahead_report(BOOL start) {
    report_switch("recordahead", &overhaul_record_ahead, start);
}

static void record_verify_report(BOOL start) {
    report_switch("recordverify", &overhaul_record_verify, start);
}

static void overlap_report(BOOL start) {
    report_switch("overlap", &overhaul_overlap, start);
}

static void gpu_detail_report(BOOL start) {
    report_switch("gpudetail", &overhaul_gpu_detail, start);
}

static void probe_lists_report(BOOL start) {
    report_switch("probelists", &overhaul_probe_lists, start);
}

static void force_recorders_report(BOOL start) {
    report_switch("forcerecorders", &overhaul_force_recorders, start);
}

static void force_all_report(BOOL start) {
    report_switch("forceall", &overhaul_force_all, start);
}

static void force_bones_report(BOOL start) {
    report_switch("forcebones", &overhaul_force_bones, start);
}

static void program_refresh_report(BOOL start) {
    report_switch("programrefresh", &overhaul_program_refresh, start);
}

static void texture_copies_report(BOOL start) {
    report_switch("texturecopies", &overhaul_texture_copies, start);
}

static void value_copies_report(BOOL start) {
    report_switch("valuecopies", &overhaul_value_copies, start);
}

static void group_stats_report(BOOL start) {
    report_switch("groupstats", &overhaul_group_stats, start);
}

static void group_gather_report(BOOL start) {
    report_switch("groupgather", &overhaul_group_gather, start);
}

static void record_worker_report(BOOL start) {
    report_switch("recordworker", &overhaul_record_worker, start);
}

static void record_cascades_report(BOOL start) {
    report_switch("recordcascades", &overhaul_record_cascades, start);
}

static void parallel_cascades_report(BOOL start) {
    report_switch("parallelcascades", &overhaul_parallel_cascades, start);
}

static void packet_verify_report(BOOL start) {
    report_switch("packetverify", &overhaul_packet_verify, start);
}

static void prepare_switch_report(const char *name, volatile LONG *flag, BOOL start) {
    if (start) {
        ab_flag = flag;
        ab_name = name;
        memset(prepare_stats, 0, sizeof prepare_stats);
        for (int i = 0; i < worker_count; i++) memset(workers[i]->task_ticks, 0, sizeof workers[i]->task_ticks);
        return;
    }
    for (int mode = 0; mode < 2; mode++) {
        const PrepareStats *stats = &prepare_stats[mode];
        double frames = stats->frames ? stats->frames : 1;
        char tasks[MAX_WORKERS * 32] = "";
        for (int i = 0; i < worker_count; i++)
            snprintf(tasks + strlen(tasks), sizeof tasks - strlen(tasks), " worker %d %.3f ms,", workers[i]->number, per_frame_ms(workers[i]->task_ticks[mode], frames));
        api->log_line("  prepare per %s-%s frame: main %.3f ms, %.3f of it waiting for workers | %.0f%% prepared early, %llu early fallbacks, "
                      "%.1f textures split | tasks:%s (%llu frames)",
                      name, mode ? "on" : "off", per_frame_ms(stats->ticks, frames), per_frame_ms(stats->join, frames), 100.0 * stats->early_used / frames,
                      (unsigned long long)stats->early_fallbacks, stats->texture_fills / frames, tasks, (unsigned long long)stats->frames);
        if (stats->sorts)
            api->log_line("  list A's parallel sort per %s-%s frame: %.3f ms, %.0f%% of its chunks moved by helpers", name, mode ? "on" : "off",
                          per_frame_ms(stats->sort, frames), stats->sort_chunks ? 100.0 * stats->helped_chunks / stats->sort_chunks : 0.0);
    }
    if (worker_error) api->log_line("  prepare workers unavailable: %s", worker_error);
    ab_flag = &overhaul_enabled;
    ab_name = "overhaul";
}

static void prepare_report(BOOL start) {
    prepare_switch_report("prepare", &overhaul_prepare_split, start);
}

static void early_prepare_report(BOOL start) {
    prepare_switch_report("earlyprepare", &overhaul_early_prepare, start);
}

static void split_fill_report(BOOL start) {
    prepare_switch_report("splitfill", &overhaul_split_fill, start);
}

static void parallel_sort_report(BOOL start) {
    prepare_switch_report("parallelsort", &overhaul_parallel_sort, start);
}

static void param_contexts_report(BOOL start) {
    report_switch("paramcontexts", &overhaul_param_contexts, start);
}

static void early_effects_report(BOOL start) {
    report_switch("earlyeffects", &overhaul_early_effects, start);
}

static void effects_verify_report(BOOL start) {
    report_switch("effectsverify", &overhaul_effects_verify, start);
}

static const char *patch_slots(void **slots, int count, void *const *replacements, void **originals) {
    DWORD protection;
    if (!VirtualProtect(slots, count * sizeof *slots, PAGE_READWRITE, &protection)) return "VirtualProtect on an engine vtable failed";
    for (int i = 0; i < count; i++) {
        if (originals) originals[i] = slots[i];
        InterlockedExchangePointer(&slots[i], replacements[i]);
    }
    VirtualProtect(slots, count * sizeof *slots, protection, &protection);
    return NULL;
}

__declspec(dllexport) const char *s2_module_main(const S2Api *s2) {
    api = s2;
    engine = api->engine;
    frame_stamp = (volatile uint64_t *)(engine + FRAME_STAMP_RVA);
    sort_bucket = (SortBucketFn)(engine + SORT_BUCKET_RVA);
    stream_in = (StreamInFn)(engine + STREAM_IN_RVA);
    tls_index = TlsAlloc();
    if (tls_index >= 64) return "TlsAlloc gave no slot in the TEB's first 64";
    void **draw_slots = (void **)(engine + RENDERER_VTABLE_RVA) + FIRST_DRAW_SLOT, **get_renderer_slot = (void **)(engine + GET_RENDERER_SLOT_RVA);
    for (int i = 0; i < DRAW_SLOTS; i++)
        if (draw_slots[i] != engine + expected_draw_rvas[i]) return "RENDERER draw slots differ from the analysed build";
    if (*get_renderer_slot != engine + GET_RENDERER_RVA) return "the device's get_renderer slot differs from the analysed build";
    for (int stage = 0; stage < STAGES; stage++) original_set_shaders[stage] = (SetShaderFn)((void **)(engine + RENDERER_VTABLE_RVA))[set_shader_slots[stage] / 8];
    const char *error = api->hook(engine + EXECUTE_RVA, execute_prologue, sizeof execute_prologue, (const void *)execute_hook, (void **)&original_execute);
    if (!error)
        error = api->hook(engine + BIND_PASS_STATE_RVA, bind_pass_state_prologue, sizeof bind_pass_state_prologue, (const void *)bind_pass_state_hook,
                          (void **)&original_bind_pass_state);
    if (!error)
        error = api->hook(engine + UPLOAD_CONSTANT_BUFFERS_RVA, upload_constant_buffers_prologue, sizeof upload_constant_buffers_prologue,
                          (const void *)upload_constant_buffers_hook, (void **)&original_upload_constant_buffers);
    if (!error)
        error = api->hook(engine + KICK_STREAM_RVA, kick_stream_prologue, sizeof kick_stream_prologue, (const void *)kick_stream_hook, &ignored_original);
    if (!error) error = api->hook(engine + PREPARE_RVA, prepare_prologue, sizeof prepare_prologue, (const void *)prepare_hook, (void **)&original_prepare);
    if (!error)
        error = api->hook(engine + COMPUTE_KEYS_RVA, compute_keys_prologue, sizeof compute_keys_prologue, (const void *)compute_keys_hook,
                          (void **)&original_compute_keys);
    if (!error)
        error = api->hook(engine + FILL_INSTANCE_TEXTURES_RVA, fill_instance_textures_prologue, sizeof fill_instance_textures_prologue,
                          (const void *)fill_instance_textures_hook, (void **)&original_fill_instance_textures);
    if (!error && (*(BYTE **)(engine + INSTANCE_RING_VTABLE_RVA + RING_ALLOCATE_SLOT) != engine + RING_ALLOCATE_RVA ||
                   *(BYTE **)(engine + GEOMETRY_RING_VTABLE_RVA + RING_ALLOCATE_SLOT) != engine + RING_ALLOCATE_RVA))
        error = "the rings' allocate slot differs from the analysed build";
    if (!error)
        error = api->hook(engine + RING_ALLOCATE_RVA, ring_allocate_prologue, sizeof ring_allocate_prologue, (const void *)ring_allocate_hook,
                          (void **)&original_ring_allocate);
    if (!error)
        error = api->hook(engine + RENDER_VIEW_RVA, render_view_prologue, sizeof render_view_prologue, (const void *)render_view_hook,
                          (void **)&original_render_view);
    if (!error) error = api->hook(engine + END_FRAME_RVA, end_frame_prologue, sizeof end_frame_prologue, (const void *)end_frame_hook, (void **)&original_end_frame);
    if (!error)
        error = api->hook(engine + RECORD_QUEUES_RVA, record_queues_prologue, sizeof record_queues_prologue, (const void *)record_queues_hook,
                          (void **)&original_record_queues);
    if (!error)
        error = api->hook(engine + RESET_BUCKETS_RVA, reset_buckets_prologue, sizeof reset_buckets_prologue, (const void *)reset_buckets_hook,
                          (void **)&original_reset_buckets);
    if (!error)
        error = api->hook(engine + RENDER_VIEWS_RVA, render_views_prologue, sizeof render_views_prologue, (const void *)render_views_hook,
                          (void **)&original_render_views);
    if (!error)
        error = api->hook(engine + BATTLE_VIEW_UPDATE_RVA, battle_view_update_prologue, sizeof battle_view_update_prologue,
                          (const void *)battle_view_update_hook, (void **)&original_battle_view_update);
    if (!error)
        error = api->hook(engine + FILL_TEXTURE_RVA, fill_texture_prologue, sizeof fill_texture_prologue, (const void *)fill_texture_hook,
                          (void **)&original_fill_texture);
    if (!error)
        error = api->hook(engine + TEXTURE_SET_RVA, texture_set_prologue, sizeof texture_set_prologue, (const void *)texture_set_hook,
                          (void **)&original_texture_set);
    if (!error)
        error = api->hook(engine + TOUCH_RVA, touch_prologue, sizeof touch_prologue, (const void *)touch_hook, (void **)&original_touch);
    if (!error)
        error = api->hook(engine + BUILD_SRV_LIST_RVA, build_srv_list_prologue, sizeof build_srv_list_prologue, (const void *)build_srv_list_hook,
                          (void **)&original_build_srv_list);
    if (!error)
        error = api->hook(engine + GROUP_GATHER_RVA, group_gather_prologue, sizeof group_gather_prologue, (const void *)group_gather_hook,
                          (void **)&original_group_gather);
    if (!error)
        error = api->hook(engine + PUSH_RECORD_RVA, push_record_prologue, sizeof push_record_prologue, (const void *)push_record_hook,
                          (void **)&original_push_record);
    if (!error)
        error = api->hook(engine + RECORD_WHOLE_RVA, record_whole_prologue, sizeof record_whole_prologue, (const void *)record_whole_hook,
                          (void **)&original_record_whole);
    if (!error)
        error = api->hook(engine + LIBRARY_GETTER_RVA, library_getter_prologue, sizeof library_getter_prologue, (const void *)library_getter_hook,
                          (void **)&original_library_getter);
    if (!error)
        error = api->hook(engine + NODE_TEXTURE_RVA, node_texture_prologue, sizeof node_texture_prologue, (const void *)node_texture_hook,
                          (void **)&original_node_texture);
    if (!error)
        error = api->hook(engine + EFFECTS_RECORD_RVA, effects_record_prologue, sizeof effects_record_prologue, (const void *)effects_record_hook,
                          (void **)&original_effects_record);
    void **begin_bucket_slot = (void **)(engine + BUCKET_CALLBACK_VTABLE_RVA);
    if (!error && *begin_bucket_slot != engine + BEGIN_BUCKET_RVA) error = "the Variant node's bucket callback differs from the analysed build";
    original_begin_bucket = (BeginBucketFn)(engine + BEGIN_BUCKET_RVA);
    void *const begin_bucket_hooks[1] = {(void *)begin_bucket_hook};
    if (!error) error = patch_slots(begin_bucket_slot, 1, begin_bucket_hooks, NULL);
    void *const draw_hooks[DRAW_SLOTS] = {(void *)draw_hook, (void *)draw_indexed_hook, (void *)draw_indexed_instanced_hook, (void *)draw_instanced_hook,
                                          (void *)draw_indirect_hook};
    void *const renderer_hook[1] = {(void *)get_renderer_hook};
    if (!error) error = patch_slots(draw_slots, DRAW_SLOTS, draw_hooks, original_draws);
    if (!error) error = patch_slots(get_renderer_slot, 1, renderer_hook, NULL);
    if (error) return error;
    api->register_switch("overhaul", &overhaul_enabled, overhaul_report);
    api->register_switch("prepare", &overhaul_prepare_split, prepare_report);
    api->register_switch("packets", &overhaul_packets, packets_report);
    api->register_switch("cbcopies", &overhaul_cb_copies, cb_copies_report);
    api->register_switch("autobalance", &overhaul_autobalance, autobalance_report);
    api->register_switch("earlyprepare", &overhaul_early_prepare, early_prepare_report);
    api->register_switch("splitfill", &overhaul_split_fill, split_fill_report);
    api->register_switch("recordahead", &overhaul_record_ahead, record_ahead_report);
    api->register_switch("recordverify", &overhaul_record_verify, record_verify_report);
    api->register_switch("overlap", &overhaul_overlap, overlap_report);
    api->register_switch("gpudetail", &overhaul_gpu_detail, gpu_detail_report);
    api->register_switch("probelists", &overhaul_probe_lists, probe_lists_report);
    api->register_switch("forcerecorders", &overhaul_force_recorders, force_recorders_report);
    api->register_switch("forceall", &overhaul_force_all, force_all_report);
    api->register_switch("forcebones", &overhaul_force_bones, force_bones_report);
    api->register_switch("programrefresh", &overhaul_program_refresh, program_refresh_report);
    api->register_switch("texturecopies", &overhaul_texture_copies, texture_copies_report);
    api->register_switch("valuecopies", &overhaul_value_copies, value_copies_report);
    api->register_switch("groupstats", &overhaul_group_stats, group_stats_report);
    api->register_switch("groupgather", &overhaul_group_gather, group_gather_report);
    api->register_switch("recordworker", &overhaul_record_worker, record_worker_report);
    api->register_switch("recordcascades", &overhaul_record_cascades, record_cascades_report);
    api->register_switch("parallelcascades", &overhaul_parallel_cascades, parallel_cascades_report);
    api->register_switch("packetverify", &overhaul_packet_verify, packet_verify_report);
    api->register_switch("paramcontexts", &overhaul_param_contexts, param_contexts_report);
    api->register_switch("parallelsort", &overhaul_parallel_sort, parallel_sort_report);
    api->register_switch("earlyeffects", &overhaul_early_effects, early_effects_report);
    api->register_switch("effectsverify", &overhaul_effects_verify, effects_verify_report);
    return NULL;
}
