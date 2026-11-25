// Generated from the targeted engine by the development tools; regenerated there, not edited.
#pragma once
#include "../s2module.h"

static const S2Check fpslimit_checks[] = {
    {0xafaf0, 0xa2, 0x29b0a69752bf6e3full, 0, "a tick's rendering (its one present, last)"},
    {0xb66f0, 0xd9c, 0x806fb92fc1434d3aull, 0, "main tick (its one call of the rendering)"},
    {0x95bde, 0x7, 0x3eeae05026e68b1cull, 0, "reference to the app global (its handler stack)"},
    {0x64c25a, 0x7, 0x43477a8e1751396cull, 0, "reference to the campaign map handler vtable"},
};
