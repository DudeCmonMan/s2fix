// Motion vectors as FSR takes them here: UV units, previous minus current, y down, jitter-free. The camera's from depth;
// an object's own where its depth stamp equals the final depth (it is what the pixel shows).
// One reprojection matrix, made in double on the CPU, keeps a still camera's vectors at 0 within float rounding.
cbuffer Motion : register(b0) {
    row_major float4x4 previous_from_jittered;
    float2 size;
    float2 jitter;
    uint objects;
};
Texture2D<float> depth : register(t0);
Texture2D<float2> velocity : register(t1);
Texture2D<float> stamp : register(t2);
RWTexture2D<float2> motion : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
    float d = depth[id.xy];
    if (objects && stamp[id.xy] == d) {
        motion[id.xy] = velocity[id.xy];
        return;
    }
    float2 uv = (id.xy + 0.5) / size;
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float4 q = mul(previous_from_jittered, float4(ndc, d, 1));
    motion[id.xy] = q.w > 0 ? (q.xy / q.w - (ndc - jitter)) * float2(0.5, -0.5) : float2(0, 0);
}
