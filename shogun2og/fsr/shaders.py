"""Compiles the FSR 3 upscaler's D3D11 passes into the headers the fork's blob accessor includes, with the fork's own
arguments; skipped when <out> already holds this output. Usage: python -I shaders.py <sdk folder> <out folder>
"""
import concurrent.futures
import glob
import hashlib
import os
import subprocess
import sys

BASE = ["-reflection", "-DFFX_GPU=1",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
        "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
        "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
        "-E", "CS", "-DFFX_HLSL=1", "-DFFX_HLSL_5_0=1", "-compiler=fxc", "-DFFX_SPD_NO_WAVE_OPERATIONS",
        "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}",
        "-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}",
        "-disable-logs", "-T", "cs_5_0", "-DFFX_HLSL_SM=50"]
VARIANTS = [("", 0), ("_wave64", 0), ("_16bit", 1), ("_wave64_16bit", 1)]


def main(sdk, out):
    root = os.path.join(sdk, "sdk")
    compiler = os.path.join(root, "tools", "binary_store", "FidelityFX_SC.exe")
    gpu = os.path.join(root, "include", "FidelityFX", "gpu")
    passes = sorted(glob.glob(os.path.join(root, "src", "backends", "dx11", "shaders", "fsr3upscaler", "*.hlsl")))
    key = hashlib.sha256()
    for path in [__file__, compiler] + passes + sorted(glob.glob(os.path.join(gpu, "**", "*.h"), recursive=True)):
        key.update(open(path, "rb").read())
    stamp = os.path.join(out, "stamp")
    if os.path.exists(stamp) and open(stamp).read() == key.hexdigest():
        return 0
    os.makedirs(out, exist_ok=True)
    jobs = [(p, suffix, half) for p in passes for suffix, half in VARIANTS]

    def run(job):
        path, suffix, half = job
        name = os.path.splitext(os.path.basename(path))[0] + suffix
        args = [compiler] + BASE + ["-name=" + name, "-DFFX_HALF=%d" % half, "-I", gpu,
                                    "-I", os.path.join(gpu, "fsr3upscaler"), "-output=" + out, path]
        done = subprocess.run(args, capture_output=True, text=True)
        return name, done.returncode, done.stdout + done.stderr

    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        for name, code, text in pool.map(run, jobs):
            if code:
                print("fsr shaders: %s failed:\n%s" % (name, text))
                return 1
    open(stamp, "w").write(key.hexdigest())
    print("fsr shaders: %d passes, %d headers" % (len(passes), len(jobs)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
