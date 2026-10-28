#pragma once
#include <d3d11.h>

// Replaces a freshly created device, and its immediate context when requested, with forwarding wrappers.
void wrap_device(ID3D11Device **device, ID3D11DeviceContext **context);
