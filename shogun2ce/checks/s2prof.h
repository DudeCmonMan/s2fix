// Generated from the targeted engine by the development tools; regenerated there, not edited.
#pragma once
#include "../s2module.h"

static const S2Check device_checks[] = {
    {0x1ac7e80, 0x3b4, 0x2ab73144d0023895ull, 0, "D3D11 device creation"},
    {0x19c34ce, 0x7, 0x5538dd3703f7fe58ull, 0, "reference to the render device global"},
};

static const S2Check report_checks[] = {
    {0x19c34ce, 0x7, 0x5538dd3703f7fe58ull, 0, "reference to the render device global"},
    {0x1aaa7db, 0x7, 0xb3b465910b80182cull, 0, "reference to the D3D11 render device vtable"},
};

static const S2Check reset_checks[] = {
    {0x19dfe70, 0x8d, 0x4c105a4acbf0a50full, 0, "frame step (calls the frame render)"},
    {0x19ee180, 0x1e9, 0x8b11377b3515f4aeull, 0, "frame render (recovers a lost device)"},
    {0x19ee369, 0x9a, 0xa04b07a01b7862cdull, 0, "frame render (recovers a lost device)"},
    {0x19ee403, 0x84, 0x6e7702e0f19a4db5ull, 0, "frame render (recovers a lost device)"},
    {0x1a85a70, 0x9e, 0x01e0b582294af07cull, 0, "GPU resource release (all)"},
    {0x1a85b10, 0xa8, 0x7d4007310cab914aull, 0, "GPU resource release (by kind)"},
    {0x1a85950, 0x118, 0x380ba3351025a9a9ull, 0, "GPU resource creation"},
    {0x19eb4d0, 0x8, 0x86dfad2a349b6f13ull, 0, "loading screen getter"},
    {0x1a97680, 0x54, 0x13300776fb7fec0full, 0, "loading screen show"},
    {0x1a690c0, 0x8, 0x8e9d2c4b90127c8full, 0, "loading screen hide"},
    {0x1af6a90, 0x3a, 0xae8f07ef6771ada5ull, 0, "device status"},
    {0x19bdb2e, 0x7, 0xf7309eba2279e5b9ull, 0, "reference to the engine global"},
    {0x1aaa7db, 0x7, 0xb3b465910b80182cull, 0, "reference to the D3D11 render device vtable"},
};

static const S2Check sortfix_checks[] = {
    {0x1afee20, 0x127, 0x160b812e92b238e6ull, 0, "render item sort"},
    {0x1afef47, 0x145, 0x6a9dbf5537d40dcdull, 0, "render item sort"},
    {0x1aff08c, 0x8f, 0xcc37bea0ae2fe40aull, 0, "render item sort"},
    {0x1af9410, 0x1a, 0x24cacc92797a603eull, 0, "render item sort helper"},
    {0x1af942a, 0x14, 0xc7a0a2c8645b65d9ull, 0, "render item sort helper"},
    {0x1af943e, 0x8eb, 0x83ae5bea2044103bull, 0, "render item sort helper"},
    {0x1af9d29, 0x14, 0xe5329273cebd7a71ull, 0, "render item sort helper"},
    {0x1af9d3d, 0xc, 0x2e3afafaaf3bebd7ull, 0, "render item sort helper"},
    {0x1afbea0, 0x367, 0xcb6403d9024a9aa5ull, 0, "render item sort helper"},
    {0x1aff420, 0x2e2, 0x88ddf368016d7496ull, 0, "object sort"},
    {0x1afa6b0, 0x55, 0x6aedc1103a70a47bull, 0, "object sort helper"},
    {0x1afa705, 0x6d6, 0xd4a8dbf81533382eull, 0, "object sort helper"},
    {0x1afaddb, 0x24, 0x9184ec060ea96c76ull, 0, "object sort helper"},
    {0x1afc810, 0xef8, 0xe5e9bc47ed47e84aull, 0, "object sort helper"},
    {0x1afde70, 0x2d8, 0x68d2bcd8cd12bfbaull, 0, "object sort helper"},
    {0x1afe850, 0x32, 0xc8a4c992ba5cb145ull, 0, "object sort helper"},
    {0x1afe882, 0x1b4, 0x61610707e1be4c78ull, 0, "object sort helper"},
    {0x1afea36, 0xe, 0x1e45ae9e58169b99ull, 0, "object sort helper"},
};
