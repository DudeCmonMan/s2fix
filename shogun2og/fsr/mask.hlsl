// FSR's reactive mask: the larger of the generated one (where the frame's transparent effects changed the colour) and the
// engine's own mask (rain and snow mark themselves 1).
cbuffer Combine : register(b0) {
    uint2 size;
    uint generated;
};
Texture2D<float> reactive : register(t0);
Texture2D<float> mask : register(t1);
RWTexture2D<unorm float> combined : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    combined[id.xy] = max(generated ? reactive[id.xy] : 0, mask[id.xy]);
}
