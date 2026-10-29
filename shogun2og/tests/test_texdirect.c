#define COBJMACROS
#include "texdirect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Checks texdirect_plan on DDS files laid out as the engine hands them to D3DX (DX10-wrapped or legacy, random blocks
// with opaque 4-colour endpoints, as encoded opaque art has): each request it serves must give the file's blocks at every
// level the file holds and, when d3dx11_42.dll is present, a texture described as D3DX describes its own, with every
// texel of each level it adds within D3DX's re-encoding error of D3DX's; each request asking for a change goes to D3DX.
// (D3DX re-encodes the whole chain when it adds levels, and pulls BC1 blocks with transparent texels toward black on
// every pass; the direct path copies those blocks, so transparency is covered by the block comparison.)
#define TOLERANCE 24
typedef HRESULT(WINAPI *CreateTextureFn)(ID3D11Device *device, const void *data, SIZE_T size, const TexLoadInfo *info, void *pump,
                                         ID3D11Resource **texture, HRESULT *result);

static ID3D11Device *device;
static ID3D11DeviceContext *context;
static CreateTextureFn d3dx_create;
static int failures, checks;

typedef struct {
    BYTE *data;
    SIZE_T size;
    UINT mips;
} Dds;

static Dds make_dds(UINT format, UINT width, UINT height, UINT mips, BOOL cube, BOOL legacy, int cut) {
    UINT block_bytes = format <= 72 ? 8 : 16, header_bytes = legacy ? 128 : 148;
    SIZE_T blocks = 0;
    for (UINT level = 0; level < mips; level++)
        blocks += (SIZE_T)(((width >> level ? width >> level : 1) + 3) / 4) * (((height >> level ? height >> level : 1) + 3) / 4) * block_bytes;
    Dds dds = {calloc(1, header_bytes + blocks), header_bytes + blocks - cut, mips};
    DWORD *h = (DWORD *)dds.data;
    h[0] = TEXDIRECT_DDS_MAGIC, h[1] = 124, h[2] = 0x1007 | TEXDIRECT_DDSD_MIPMAPCOUNT, h[3] = height, h[4] = width, h[7] = mips;
    h[19] = 32, h[20] = 4, h[27] = 0x401008;
    if (legacy) {
        h[21] = format <= 72 ? TEXDIRECT_FOURCC_DXT1 : format <= 75 ? TEXDIRECT_FOURCC_DXT3 : TEXDIRECT_FOURCC_DXT5;
        h[28] = cube ? TEXDIRECT_DDSCAPS2_CUBEMAP : 0;
    } else {
        h[21] = TEXDIRECT_FOURCC_DX10, h[32] = format, h[33] = TEXDIRECT_TEXTURE2D, h[34] = cube ? 4 : 0, h[35] = 1, h[36] = 0;
    }
    BYTE *block = dds.data + header_bytes;
    for (SIZE_T i = 0; i < blocks; i++) block[i] = (BYTE)rand();
    for (SIZE_T at = format <= 72 ? 0 : 8; at < blocks; at += block_bytes) {
        WORD c0 = (WORD)(block[at] | block[at + 1] << 8), c1 = (WORD)(block[at + 2] | block[at + 3] << 8);
        if (c0 == c1) c0 == 0xffff ? c1-- : c0++;
        if (c0 < c1) c0 ^= c1, c1 ^= c0, c0 ^= c1;
        block[at] = (BYTE)c0, block[at + 1] = (BYTE)(c0 >> 8), block[at + 2] = (BYTE)c1, block[at + 3] = (BYTE)(c1 >> 8);
    }
    return dds;
}

static TexLoadInfo request(UINT format, UINT width, UINT height, UINT first, UINT mips) {
    return (TexLoadInfo){width >> first, height >> first, 0xffffffffu, first, mips, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, format,
                         TEXDIRECT_FILTER, TEXDIRECT_FILTER, NULL};
}

// The engine's other request: the file's own format, no filtering.
static TexLoadInfo plain(UINT format, UINT width, UINT height, UINT mips) {
    TexLoadInfo info = request(format, width, height, 0, mips);
    info.filter = info.mip_filter = TEXDIRECT_FILTER_NONE;
    return info;
}

static ID3D11Texture2D *readable_copy(ID3D11Resource *texture) {
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *staging;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING, desc.BindFlags = 0, desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging))) return NULL;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, texture);
    return staging;
}

// Levels of the texture made from the plan whose blocks differ from the plan's source.
static int differing_levels(const TexPlan *plan) {
    ID3D11Texture2D *texture, *staging;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &plan->desc, plan->levels, &texture))) return -1;
    if (!(staging = readable_copy((ID3D11Resource *)texture))) return -1;
    UINT block_bytes = plan->desc.Format <= 72 ? 8 : 16;
    int differing = 0;
    for (UINT level = 0; level < plan->desc.MipLevels; level++) {
        UINT across = (((plan->desc.Width >> level) ? plan->desc.Width >> level : 1) + 3) / 4;
        UINT down = (((plan->desc.Height >> level) ? plan->desc.Height >> level : 1) + 3) / 4;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, level, D3D11_MAP_READ, 0, &mapped))) return -1;
        int same = 1;
        for (UINT row = 0; row < down; row++)
            same &= !memcmp((BYTE *)mapped.pData + row * mapped.RowPitch, (const BYTE *)plan->levels[level].pSysMem + row * plan->levels[level].SysMemPitch,
                            across * block_bytes);
        differing += !same;
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, level);
    }
    ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(texture);
    return differing;
}

static void endpoint(int v, int *rgb) {
    rgb[0] = (v >> 11 & 31) * 255 / 31, rgb[1] = (v >> 5 & 63) * 255 / 63, rgb[2] = (v & 31) * 255 / 31;
}

// Texel (x, y) of a level as 8-bit RGBA, per the D3D spec.
static void texel(UINT format, const BYTE *level, UINT pitch, UINT x, UINT y, int *rgba) {
    BOOL bc1 = format <= 72;
    const BYTE *block = level + (y / 4) * pitch + (x / 4) * (bc1 ? 8 : 16), *c = block + (bc1 ? 0 : 8);
    x &= 3, y &= 3;
    int c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8, e[4][3], index = c[4 + y] >> (2 * x) & 3;
    BOOL three = bc1 && c0 <= c1;
    endpoint(c0, e[0]), endpoint(c1, e[1]);
    for (int k = 0; k < 3; k++) {
        e[2][k] = three ? (e[0][k] + e[1][k]) / 2 : (2 * e[0][k] + e[1][k]) / 3;
        e[3][k] = three ? 0 : (e[0][k] + 2 * e[1][k]) / 3;
        rgba[k] = e[index][k];
    }
    rgba[3] = three && index == 3 ? 0 : 255;
    if (format == 74 || format == 75) rgba[3] = (block[2 * y + x / 2] >> (4 * (x & 1)) & 15) * 17;
    if (format == 77 || format == 78) {
        unsigned long long bits = 0;
        for (int i = 0; i < 6; i++) bits |= (unsigned long long)block[2 + i] << (8 * i);
        int a0 = block[0], a1 = block[1], i = bits >> (3 * (4 * y + x)) & 7;
        rgba[3] = i == 0 ? a0 : i == 1 ? a1 : a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7 : i == 6 ? 0 : i == 7 ? 255 : ((6 - i) * a0 + (i - 1) * a1) / 5;
    }
}

// The largest channel difference between the levels the plan adds and D3DX's for the same request; -1 without D3DX.
static int added_levels_difference(const Dds *dds, const TexLoadInfo *info, const TexPlan *plan, UINT stored) {
    ID3D11Resource *resource;
    if (!d3dx_create || FAILED(d3dx_create(device, dds->data, dds->size, info, NULL, &resource, NULL))) return -1;
    ID3D11Texture2D *staging = readable_copy(resource);
    ID3D11Resource_Release(resource);
    if (!staging) return -1;
    int worst = 0;
    for (UINT level = stored; level < plan->desc.MipLevels; level++) {
        UINT width = plan->desc.Width >> level ? plan->desc.Width >> level : 1, height = plan->desc.Height >> level ? plan->desc.Height >> level : 1;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, level, D3D11_MAP_READ, 0, &mapped))) return -1;
        for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++) {
                int ours[4], theirs[4];
                texel(plan->desc.Format, plan->levels[level].pSysMem, plan->levels[level].SysMemPitch, x, y, ours);
                texel(plan->desc.Format, mapped.pData, mapped.RowPitch, x, y, theirs);
                for (int k = 0; k < 4; k++)
                    if (abs(ours[k] - theirs[k]) > worst) worst = abs(ours[k] - theirs[k]);
            }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, level);
    }
    ID3D11Texture2D_Release(staging);
    return worst;
}

static void expect(const char *name, Dds dds, TexLoadInfo info, int served) {
    TexPlan plan;
    int planned = texdirect_plan(dds.data, dds.size, &info, &plan);
    checks++;
    if (planned != served) {
        failures++;
        printf("FAIL %s: %s, expected %s\n", name, planned ? "served" : "left to D3DX", served ? "served" : "left to D3DX");
        free(dds.data);
        return;
    }
    if (!served) {
        printf("ok %s: left to D3DX\n", name);
        free(dds.data);
        return;
    }
    int differing = differing_levels(&plan);
    char matches[64] = "D3DX not present", added[96] = "";
    BOOL d3dx_differs = FALSE, added_differs = FALSE;
    ID3D11Resource *resource;
    if (d3dx_create && SUCCEEDED(d3dx_create(device, dds.data, dds.size, &info, NULL, &resource, NULL))) {
        D3D11_TEXTURE2D_DESC theirs;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)resource, &theirs);
        ID3D11Resource_Release(resource);
        d3dx_differs = theirs.Width != plan.desc.Width || theirs.Height != plan.desc.Height || theirs.MipLevels != plan.desc.MipLevels ||
                       theirs.ArraySize != plan.desc.ArraySize || theirs.Format != plan.desc.Format || theirs.Usage != plan.desc.Usage ||
                       theirs.BindFlags != plan.desc.BindFlags || theirs.CPUAccessFlags != plan.desc.CPUAccessFlags ||
                       theirs.MiscFlags != plan.desc.MiscFlags;
        snprintf(matches, sizeof matches, d3dx_differs ? "D3DX made %ux%u, %u mips" : "as D3DX describes it", theirs.Width, theirs.Height,
                 theirs.MipLevels);
    }
    UINT stored = dds.mips - info.first_mip;
    if (plan.desc.MipLevels > stored) {
        int worst = added_levels_difference(&dds, &info, &plan, stored);
        added_differs = worst > TOLERANCE;
        snprintf(added, sizeof added, "; %u levels added, %s %d", plan.desc.MipLevels - stored, worst < 0 ? "not compared" : "differing from D3DX's by",
                 worst);
    }
    if (differing || d3dx_differs || added_differs) failures++;
    printf("%s %s: %ux%u, %u mips, %d levels differ from the file, %s%s\n", differing || d3dx_differs || added_differs ? "FAIL" : "ok", name,
           plan.desc.Width, plan.desc.Height, plan.desc.MipLevels, differing, matches, added);
    free(dds.data);
}

int main(void) {
    HMODULE d3dx = LoadLibraryW(L"d3dx11_42.dll");
    d3dx_create = d3dx ? (CreateTextureFn)(void *)GetProcAddress(d3dx, "D3DX11CreateTextureFromMemory") : NULL;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context)) &&
        FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context)))
        return printf("D3D11CreateDevice failed\n"), 1;
    srand(1);
    expect("BC1 sRGB 256x256, all mips", make_dds(72, 256, 256, 9, FALSE, FALSE, 0), request(72, 256, 256, 0, 0), 1);
    expect("BC3 sRGB 1024x512, all mips", make_dds(78, 1024, 512, 11, FALSE, FALSE, 0), request(78, 1024, 512, 0, 0), 1);
    expect("BC2 sRGB 100x60, all mips", make_dds(75, 100, 60, 7, FALSE, FALSE, 0), request(75, 100, 60, 0, 0), 1);
    expect("BC3 sRGB 512x512 from mip 1", make_dds(78, 512, 512, 10, FALSE, FALSE, 0), request(78, 512, 512, 1, 0), 1);
    expect("BC1 sRGB 64x64, one level asked", make_dds(72, 64, 64, 7, FALSE, FALSE, 0), request(72, 64, 64, 0, 1), 1);
    expect("BC3 sRGB 4x4, all mips", make_dds(78, 4, 4, 3, FALSE, FALSE, 0), request(78, 4, 4, 0, 0), 1);
    for (int i = 0; i < 3; i++) {
        expect("BC1 sRGB 256x256 to 2x2, 1x1 added", make_dds(72, 256, 256, 8, FALSE, FALSE, 0), request(72, 256, 256, 0, 0), 1);
        expect("BC3 sRGB 512x512 to 2x2, 1x1 added", make_dds(78, 512, 512, 9, FALSE, FALSE, 0), request(78, 512, 512, 0, 0), 1);
    }
    expect("BC2 sRGB 128x128 to 2x2, 1x1 added", make_dds(75, 128, 128, 7, FALSE, FALSE, 0), request(75, 128, 128, 0, 0), 1);
    expect("BC3 sRGB 512x128 to 2x1, 1x1 added", make_dds(78, 512, 128, 9, FALSE, FALSE, 0), request(78, 512, 128, 0, 0), 1);
    expect("BC1 sRGB 1024x2048 to 1x2, 1x1 added", make_dds(72, 1024, 2048, 11, FALSE, FALSE, 0), request(72, 1024, 2048, 0, 0), 1);
    expect("BC1 sRGB 1024x1024 to 4x4, two levels added", make_dds(72, 1024, 1024, 9, FALSE, FALSE, 0), request(72, 1024, 1024, 0, 0), 1);
    expect("BC3 sRGB 512x128 to 8x2, three levels added", make_dds(78, 512, 128, 7, FALSE, FALSE, 0), request(78, 512, 128, 0, 0), 1);
    expect("BC1 sRGB 192x108, one level, all mips asked", make_dds(72, 192, 108, 1, FALSE, FALSE, 0), request(72, 192, 108, 0, 0), 1);
    expect("BC3 sRGB 64x64, one level, all mips asked", make_dds(78, 64, 64, 1, FALSE, FALSE, 0), request(78, 64, 64, 0, 0), 1);
    expect("legacy DXT1 192x108, one level asked as sRGB", make_dds(72, 192, 108, 1, FALSE, TRUE, 0), request(72, 192, 108, 0, 1), 1);
    expect("legacy DXT5 256x64 to 4x1, all mips asked as sRGB", make_dds(78, 256, 64, 7, FALSE, TRUE, 0), request(78, 256, 64, 0, 0), 1);
    expect("legacy DXT3 64x64 as sRGB", make_dds(75, 64, 64, 7, FALSE, TRUE, 0), request(75, 64, 64, 0, 0), 1);
    expect("cube", make_dds(78, 64, 64, 7, TRUE, FALSE, 0), request(78, 64, 64, 0, 0), 0);
    expect("legacy cube", make_dds(78, 64, 64, 7, TRUE, TRUE, 0), request(78, 64, 64, 0, 0), 0);
    expect("legacy DXT5 as itself, all mips", make_dds(77, 256, 128, 9, FALSE, TRUE, 0), plain(77, 256, 128, 0), 1);
    expect("legacy DXT1 as itself to 2x2, 1x1 added", make_dds(71, 256, 256, 8, FALSE, TRUE, 0), plain(71, 256, 256, 0), 1);
    expect("legacy DXT5 as itself to 4x4, two levels added", make_dds(77, 512, 512, 8, FALSE, TRUE, 0), plain(77, 512, 512, 0), 1);
    expect("legacy DXT3 as itself", make_dds(74, 64, 64, 7, FALSE, TRUE, 0), plain(74, 64, 64, 0), 1);
    expect("BC1 non-sRGB as itself", make_dds(71, 128, 64, 8, FALSE, FALSE, 0), plain(71, 128, 64, 0), 1);
    expect("BC3 non-sRGB asked as sRGB", make_dds(77, 64, 64, 7, FALSE, FALSE, 0), request(78, 64, 64, 0, 0), 1);
    expect("sRGB file as itself without the sRGB flags", make_dds(78, 64, 64, 7, FALSE, FALSE, 0), plain(78, 64, 64, 0), 0);
    expect("filter NONE asking sRGB of a non-sRGB file", make_dds(77, 64, 64, 7, FALSE, TRUE, 0), plain(78, 64, 64, 0), 0);
    expect("sRGB flags asking the non-sRGB format", make_dds(77, 64, 64, 7, FALSE, TRUE, 0), request(77, 64, 64, 0, 0), 0);
    expect("another format asked", make_dds(78, 64, 64, 7, FALSE, FALSE, 0), request(71, 64, 64, 0, 0), 0);
    TexLoadInfo box = plain(77, 64, 64, 0);
    box.filter = box.mip_filter = 2;
    expect("a real filter asked", make_dds(77, 64, 64, 7, FALSE, TRUE, 0), box, 0);
    TexLoadInfo resized = request(78, 64, 64, 0, 0);
    resized.width = 32;
    expect("resize asked", make_dds(78, 64, 64, 7, FALSE, FALSE, 0), resized, 0);
    expect("more levels asked than a full chain", make_dds(78, 64, 64, 7, FALSE, FALSE, 0), request(78, 64, 64, 0, 8), 0);
    expect("truncated file", make_dds(78, 64, 64, 7, FALSE, FALSE, 16), request(78, 64, 64, 0, 0), 0);
    printf("test_texdirect: %d checks, %d failures%s\n", checks, failures, d3dx_create ? "" : " (d3dx11_42.dll not found: not compared with D3DX)");
    return failures != 0;
}
