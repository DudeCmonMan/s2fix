#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Defines the interface IDs here; mingw's import libraries lack the newer device and context versions.
#include <initguid.h>
#include <d3d11_4.h>
#include <stdlib.h>
#include "wrap.h"

// The engine gets our device and immediate context instead of the runtime's. Every call is forwarded as is: queueing the
// context's calls for a render thread needs the engine to hold these objects rather than the real ones. Each slot jumps
// through the real object's current vtable, since the runtime rewrites its context's vtable entries while running.
#define DEVICE_SLOTS 69
#define CONTEXT_SLOTS 149
#define CONTEXT_GET_DEVICE_SLOT 3
#define GET_IMMEDIATE_CONTEXT_SLOTS {40, 43, 50, 61}

// The forwarding thunks read their target right after the vtable pointer. The context's target is the immediate context,
// or a deferred one the overhaul records main into while workers draw; references and queries stay with the immediate.
typedef struct {
    void *const *vtable;
    IUnknown *real, *immediate;
} Wrapper;

typedef struct {
    Wrapper device, context;
} WrappedDevice;

#define FORWARD(n) \
    __attribute__((naked)) static void forward_##n(void) { __asm__("mov 8(%rcx), %rcx\n\tmov (%rcx), %rax\n\tjmp *(" #n "*8)(%rax)"); }
#define FORWARD10(d) FORWARD(d##0) FORWARD(d##1) FORWARD(d##2) FORWARD(d##3) FORWARD(d##4) FORWARD(d##5) FORWARD(d##6) FORWARD(d##7) FORWARD(d##8) FORWARD(d##9)
FORWARD(0) FORWARD(1) FORWARD(2) FORWARD(3) FORWARD(4) FORWARD(5) FORWARD(6) FORWARD(7) FORWARD(8) FORWARD(9)
FORWARD10(1) FORWARD10(2) FORWARD10(3) FORWARD10(4) FORWARD10(5) FORWARD10(6) FORWARD10(7) FORWARD10(8) FORWARD10(9) FORWARD10(10) FORWARD10(11)
FORWARD10(12) FORWARD10(13) FORWARD10(14)
#define ENTRY10(d) forward_##d##0, forward_##d##1, forward_##d##2, forward_##d##3, forward_##d##4, forward_##d##5, forward_##d##6, forward_##d##7, \
                   forward_##d##8, forward_##d##9,
static void (*const forwards[150])(void) = {forward_0, forward_1, forward_2, forward_3, forward_4, forward_5, forward_6, forward_7, forward_8, forward_9,
                                            ENTRY10(1) ENTRY10(2) ENTRY10(3) ENTRY10(4) ENTRY10(5) ENTRY10(6) ENTRY10(7) ENTRY10(8) ENTRY10(9)
                                                ENTRY10(10) ENTRY10(11) ENTRY10(12) ENTRY10(13) ENTRY10(14)};

static void *device_vtable[DEVICE_SLOTS];
// Exported so the overhaul can tell our context from the runtime's.
__declspec(dllexport) void *wrapped_context_vtable[CONTEXT_SLOTS];

static WrappedDevice *owner_of_device(void *self) {
    return (WrappedDevice *)((BYTE *)self - offsetof(WrappedDevice, device));
}

static WrappedDevice *owner_of_context(void *self) {
    return (WrappedDevice *)((BYTE *)self - offsetof(WrappedDevice, context));
}

static BOOL is_device_iid(REFIID riid) {
    return IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID3D11Device) || IsEqualIID(riid, &IID_ID3D11Device1) ||
           IsEqualIID(riid, &IID_ID3D11Device2) || IsEqualIID(riid, &IID_ID3D11Device3) || IsEqualIID(riid, &IID_ID3D11Device4) ||
           IsEqualIID(riid, &IID_ID3D11Device5);
}

static BOOL is_context_iid(REFIID riid) {
    return IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID3D11DeviceChild) || IsEqualIID(riid, &IID_ID3D11DeviceContext) ||
           IsEqualIID(riid, &IID_ID3D11DeviceContext1) || IsEqualIID(riid, &IID_ID3D11DeviceContext2) || IsEqualIID(riid, &IID_ID3D11DeviceContext3) ||
           IsEqualIID(riid, &IID_ID3D11DeviceContext4);
}

// Our object only stands in for interface versions the real one has; the real query adds the reference both share.
static HRESULT STDMETHODCALLTYPE device_query_interface(void *self, REFIID riid, void **object) {
    Wrapper *wrapper = self;
    HRESULT result = IUnknown_QueryInterface(wrapper->real, riid, object);
    if (SUCCEEDED(result) && is_device_iid(riid)) *object = self;
    return result;
}

static HRESULT STDMETHODCALLTYPE context_query_interface(void *self, REFIID riid, void **object) {
    Wrapper *wrapper = self;
    HRESULT result = IUnknown_QueryInterface(wrapper->immediate, riid, object);
    if (SUCCEEDED(result) && is_context_iid(riid)) *object = self;
    return result;
}

static ULONG STDMETHODCALLTYPE context_add_ref(void *self) {
    return IUnknown_AddRef(((Wrapper *)self)->immediate);
}

static ULONG STDMETHODCALLTYPE context_release(void *self) {
    return IUnknown_Release(((Wrapper *)self)->immediate);
}

static void STDMETHODCALLTYPE get_immediate_context(void *self, void **context) {
    WrappedDevice *owner = owner_of_device(self);
    IUnknown_AddRef(owner->context.real);
    *context = &owner->context;
}

static void STDMETHODCALLTYPE context_get_device(void *self, void **device) {
    WrappedDevice *owner = owner_of_context(self);
    IUnknown_AddRef(owner->device.real);
    *device = &owner->device;
}

// The newest interface the real object has, so every slot of it can be forwarded.
static IUnknown *newest_interface(IUnknown *object, const IID *const *iids, int count) {
    for (int i = 0; i < count; i++) {
        IUnknown *newest;
        if (SUCCEEDED(IUnknown_QueryInterface(object, iids[i], (void **)&newest))) {
            IUnknown_Release(newest);
            return newest;
        }
    }
    return object;
}

__attribute__((constructor)) static void build_vtables(void) {
    for (int i = 0; i < DEVICE_SLOTS; i++) device_vtable[i] = (void *)forwards[i];
    for (int i = 0; i < CONTEXT_SLOTS; i++) wrapped_context_vtable[i] = (void *)forwards[i];
    device_vtable[0] = (void *)device_query_interface;
    wrapped_context_vtable[0] = (void *)context_query_interface;
    wrapped_context_vtable[1] = (void *)context_add_ref;
    wrapped_context_vtable[2] = (void *)context_release;
    wrapped_context_vtable[CONTEXT_GET_DEVICE_SLOT] = (void *)context_get_device;
    const int slots[] = GET_IMMEDIATE_CONTEXT_SLOTS;
    for (size_t i = 0; i < sizeof slots / sizeof *slots; i++) device_vtable[slots[i]] = (void *)get_immediate_context;
}

void wrap_device(ID3D11Device **device, ID3D11DeviceContext **context) {
    // A caller that asked only for the device reaches the context through GetImmediateContext, which returns ours.
    ID3D11DeviceContext *immediate = context ? *context : NULL;
    if (!immediate) {
        ID3D11Device_GetImmediateContext(*device, &immediate);
        ID3D11DeviceContext_Release(immediate);
    }
    static const IID *const device_iids[] = {&IID_ID3D11Device5, &IID_ID3D11Device4, &IID_ID3D11Device3, &IID_ID3D11Device2, &IID_ID3D11Device1};
    static const IID *const context_iids[] = {&IID_ID3D11DeviceContext4, &IID_ID3D11DeviceContext3, &IID_ID3D11DeviceContext2, &IID_ID3D11DeviceContext1};
    WrappedDevice *wrapped = malloc(sizeof *wrapped);
    IUnknown *real_device = newest_interface((IUnknown *)*device, device_iids, 5), *real_context = newest_interface((IUnknown *)immediate, context_iids, 4);
    wrapped->device = (Wrapper){device_vtable, real_device, real_device};
    wrapped->context = (Wrapper){wrapped_context_vtable, real_context, real_context};
    *device = (ID3D11Device *)&wrapped->device;
    if (context) *context = (ID3D11DeviceContext *)&wrapped->context;
}
