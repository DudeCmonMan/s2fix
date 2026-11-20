// The backend's shader lookup for FSR 3 only: the SDK's own pulls in every effect's shaders.
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include "ffx_shader_blobs.h"
#include "blob_accessors/ffx_fsr3upscaler_shaderblobs.h"

FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect, FfxPass passId, FfxBindStage, uint32_t permutationOptions,
                                          FfxShaderBlob *outBlob) {
    return fsr3UpscalerGetPermutationBlobByIndex((FfxFsr3UpscalerPass)passId, permutationOptions, outBlob);
}
