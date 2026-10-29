#pragma once
#include <windows.h>
#include <d3d11.h>

// The engine loads diffuse textures as sRGB: it wraps a legacy DXT file's blocks in a DX10 DDS header with the sRGB
// format (0x1146b570: BC1/BC2/BC3 become 72/75/78), or passes some legacy files (loading screens) as they are, and asks
// D3DX for that sRGB format at the same size with no filtering but the sRGB flags. Other textures it asks for in the
// file's own format with no filtering at all. D3DX copies such a file block by block, or, when it has to add mip levels,
// decompresses and re-encodes every block on the CPU (about 300 times slower than a copy, and lossy), although the
// request changes nothing. Such requests are served by creating the texture from the file's own blocks; any other goes
// to D3DX.
#define TEXDIRECT_FILTER (1u | 0x600000u)
#define TEXDIRECT_FILTER_NONE 1u
#define TEXDIRECT_MAX_MIPS 16
#define TEXDIRECT_DDS_MAGIC 0x20534444u
#define TEXDIRECT_FOURCC_DX10 0x30315844u
#define TEXDIRECT_FOURCC_DXT1 0x31545844u
#define TEXDIRECT_FOURCC_DXT3 0x33545844u
#define TEXDIRECT_FOURCC_DXT5 0x35545844u
#define TEXDIRECT_DDSD_MIPMAPCOUNT 0x20000u
#define TEXDIRECT_DDSCAPS2_CUBEMAP 0x200u
#define TEXDIRECT_DDSCAPS2_VOLUME 0x200000u
#define TEXDIRECT_TEXTURE2D 3u

// D3DX11_IMAGE_LOAD_INFO.
typedef struct {
    UINT width, height, depth, first_mip, mips, usage, bind, cpu, misc, format, filter, mip_filter;
    void *source_info;
} TexLoadInfo;

typedef struct {
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA levels[TEXDIRECT_MAX_MIPS];
} TexPlan;

static inline UINT texdirect_full_chain(UINT width, UINT height) {
    UINT mips = 1;
    for (UINT size = width > height ? width : height; size > 1; size >>= 1) mips++;
    return mips;
}

// The format a plain 2D BC1/BC2/BC3 DDS file holds (71/74/77, or 72/75/78 for sRGB) and where its blocks start; 0 for
// any other file.
static inline UINT texdirect_source(const BYTE *data, SIZE_T size, SIZE_T *blocks_at) {
    const DWORD *header = (const DWORD *)data;
    if (size < 128 || header[0] != TEXDIRECT_DDS_MAGIC) return 0;
    if (header[21] == TEXDIRECT_FOURCC_DX10) {
        if (size < 148 || header[33] != TEXDIRECT_TEXTURE2D || header[34] || header[35] != 1) return 0;
        *blocks_at = 148;
        return header[32] >= 71 && header[32] <= 78 && header[32] % 3 != 1 ? header[32] : 0;
    }
    if (header[28] & (TEXDIRECT_DDSCAPS2_CUBEMAP | TEXDIRECT_DDSCAPS2_VOLUME)) return 0;
    *blocks_at = 128;
    return header[21] == TEXDIRECT_FOURCC_DXT1 ? 71 : header[21] == TEXDIRECT_FOURCC_DXT3 ? 74 : header[21] == TEXDIRECT_FOURCC_DXT5 ? 77 : 0;
}

// The request changes nothing: the sRGB format of the file's blocks with only the sRGB flags (D3DX then converts
// neither way), or the file's own non-sRGB format with no filtering.
static inline BOOL texdirect_unchanged(UINT file_format, const TexLoadInfo *info) {
    BOOL srgb_file = file_format % 3 == 0;
    if (info->filter == TEXDIRECT_FILTER && info->mip_filter == TEXDIRECT_FILTER) return info->format == (srgb_file ? file_format : file_format + 1);
    return info->filter == TEXDIRECT_FILTER_NONE && info->mip_filter == TEXDIRECT_FILTER_NONE && !srgb_file && info->format == file_format;
}

// The texture D3DX would make from a file it only has to copy, with the file's blocks for each level; 0 when the
// request asks for any change. For a full chain (mips 0) D3DX adds the levels a file lacks, each the top-left corner of
// the file's last level (filter NONE crops; checked on the game's files), so those levels read the last level's
// blocks at its own row pitch.
static inline int texdirect_plan(const BYTE *data, SIZE_T size, const TexLoadInfo *info, TexPlan *plan) {
    SIZE_T blocks_at = 0;
    UINT format = info && data ? texdirect_source(data, size, &blocks_at) : 0;
    if (!format) return 0;
    const DWORD *header = (const DWORD *)data;
    UINT width = header[4], height = header[3], first = info->first_mip;
    UINT mips = header[2] & TEXDIRECT_DDSD_MIPMAPCOUNT && header[7] ? header[7] : 1;
    if (!width || !height || first >= mips || mips >= 32 || info->width != width >> first || info->height != height >> first) return 0;
    if (!texdirect_unchanged(format, info) || info->usage != D3D11_USAGE_DEFAULT || info->bind != D3D11_BIND_SHADER_RESOURCE || info->cpu ||
        info->misc || info->source_info)
        return 0;
    UINT stored = mips - first, full = texdirect_full_chain(width >> first, height >> first);
    UINT levels = info->mips ? info->mips : full;
    if (levels > full || levels > TEXDIRECT_MAX_MIPS) return 0;
    UINT block_bytes = format <= 72 ? 8 : 16;
    const BYTE *blocks = data + blocks_at, *end = data + size;
    for (UINT level = 0; level < mips && level < first + levels; level++) {
        UINT across = ((width >> level ? width >> level : 1) + 3) / 4, down = ((height >> level ? height >> level : 1) + 3) / 4;
        if ((SIZE_T)(end - blocks) < (SIZE_T)across * block_bytes * down) return 0;
        if (level >= first) plan->levels[level - first] = (D3D11_SUBRESOURCE_DATA){blocks, across * block_bytes, 0};
        blocks += (SIZE_T)across * block_bytes * down;
    }
    for (UINT level = stored; level < levels; level++) plan->levels[level] = plan->levels[stored - 1];
    plan->desc = (D3D11_TEXTURE2D_DESC){width >> first, height >> first, levels, 1, info->format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    return 1;
}
