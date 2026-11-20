"""Writes the fork's D3D11 backend into <out> without ID3D11DeviceContext1: its 11.1 path binds constants from a buffer
it keeps mapped, which D3D11 does not allow. Usage: python -I patch_backend.py <sdk folder> <out folder>
"""
import os
import sys

CHANGES = [
    ("            backendContext->deviceContext->QueryInterface(IID_PPV_ARGS(&backendContext->deviceContext1));\n",
     "            // s2fix: no ID3D11DeviceContext1, so constants take the Map(DISCARD)/Unmap path (patch_backend.py).\n"),
    ("                backendContext->deviceContext->Unmap(backendContext->constantBufferResource[i], 0);\n",
     "                // s2fix: nothing is mapped on the Map(DISCARD)/Unmap path (patch_backend.py).\n"),
]


def main(sdk, out):
    src = os.path.join(sdk, "sdk", "src", "backends", "dx11", "ffx_dx11.cpp")
    text = open(src, encoding="utf-8").read()
    for old, new in CHANGES:
        if text.count(old) != 1:
            print("patch_backend: a line to change is not in %s exactly once: %s" % (src, old.strip()))
            return 1
        text = text.replace(old, new)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "ffx_dx11.cpp"), "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
