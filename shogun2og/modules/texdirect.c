#define COBJMACROS
#include "../s2module.h"
#include "../texdirect.h"
#include <stdio.h>

// D3DX decodes and re-encodes every block of the engine's sRGB texture requests, most of a load; texdirect.h serves
// them from the file's own blocks instead, as the game already shows for its legacy DXT textures.
#define D3DX_CREATE_TEXTURE_IAT_RVA 0x15a17b4

typedef HRESULT(WINAPI *CreateTextureFn)(ID3D11Device *device, const void *data, SIZE_T size, const TexLoadInfo *info, void *pump,
                                         ID3D11Resource **texture, HRESULT *result);

__declspec(dllexport) volatile LONG direct_enabled = 1;

static const S2Api *api;
static CreateTextureFn real_create_texture;

static HRESULT WINAPI create_texture_hook(ID3D11Device *device, const void *data, SIZE_T size, const TexLoadInfo *info, void *pump,
                                          ID3D11Resource **texture, HRESULT *result) {
    TexPlan plan;
    if (direct_enabled && *api->fix_enabled && !pump && texture && texdirect_plan(data, size, info, &plan)) {
        ID3D11Texture2D *created;
        HRESULT status = ID3D11Device_CreateTexture2D(device, &plan.desc, plan.levels, &created);
        if (SUCCEEDED(status)) {
            *texture = (ID3D11Resource *)created;
            if (result) *result = status;
            return status;
        }
    }
    return real_create_texture(device, data, size, info, pump, texture, result);
}

__declspec(dllexport) const char *s2_module_main(const S2Api *host) {
    api = host;
    BYTE *slot = api->engine + D3DX_CREATE_TEXTURE_IAT_RVA;
    HMODULE d3dx = GetModuleHandleW(L"d3dx11_42.dll");
    if (!d3dx || *(void **)slot != (void *)GetProcAddress(d3dx, "D3DX11CreateTextureFromMemory")) return "the import slot does not hold D3DX11CreateTextureFromMemory";
    real_create_texture = *(CreateTextureFn *)slot;
    void *hook = (void *)create_texture_hook;
    const char *error = api->patch(slot, (const BYTE *)&real_create_texture, (const BYTE *)&hook, sizeof hook);
    if (error) return error;
    api->register_switch("texdirect", &direct_enabled, NULL);
    return NULL;
}
