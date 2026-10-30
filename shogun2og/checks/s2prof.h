// Generated from the targeted engine by the development tools; regenerated there, not edited.
#pragma once
#include "../s2module.h"

static const S2Check s2prof_checks[] = {
    {0x1382f30, 0x7, 0xc1d873913beb178aull, 0, "reference to the render device global"},
    {0x143b6ba, 0x7, 0xbe43fc6cbb16c6f2ull, 0, "reference to the D3D11 device vtable"},
    {0x143be8d, 0x7, 0x550d2ac44e53f2dcull, 0, "reference to the D3D9 device vtable"},
};

static const S2Check device_checks[] = {
    {0x145fdb0, 0x140, 0x7852f0f2776cb3fbull, 0, "D3D11 device creation"},
};

static const S2Check reset_checks[] = {
    {0x13f86d0, 0xa5, 0x02693bd3ca4360cdull, 0, "graphics recovery (rebuilds every GPU resource when the device is flagged)"},
    {0x14012f0, 0x17f, 0x6f240ec196600d48ull, 0, "frame start (calls the graphics recovery)"},
    {0x140fcc0, 0x75, 0x2a6373398693312full, 0, "GPU resource release"},
    {0x140fbc0, 0xe5, 0xc9ec03c2d5a3a0a1ull, 0, "GPU resource creation"},
    {0x13fcb10, 0x7, 0x6511682cd78b1efcull, 0, "loading screen getter"},
    {0x141fa00, 0x37, 0x4e7592750ce5a666ull, 0, "loading screen show"},
    {0x13ecde0, 0x5, 0xe6ef986dcb06453full, 0, "loading screen hide"},
    {0x14a1650, 0x2c, 0x02f18f84d05ecf21ull, 0, "device status"},
    {0x137dbed, 0x7, 0x56213c590a130a1aull, 0, "reference to the engine global"},
    {0x143b6ba, 0x7, 0xbe43fc6cbb16c6f2ull, 0, "reference to the D3D11 device vtable"},
};
