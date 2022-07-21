#define NO_COMMON_RESOURCES
#include "common.hlsli"

cbuffer Constants : register(b0) {
    uint baseGBufferIdx;
    float gamma = 2.2;
    float exposure = 1.0f;
}

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput result;
    result.uv = float2(id % 2, (id % 4) >> 1);
    result.pos = float4((result.uv.x - 0.5f) * 2, -(result.uv.y - 0.5f) * 2, 0, 1);
    return result;
}

[earlydepthstencil]
float4 PSMain(PSInput input) : SV_TARGET
{
    Texture2D radianceTexture = ResourceDescriptorHeap[baseGBufferIdx + GBUFFER_RADIANCE];
    float3 radiance = radianceTexture.Sample(g_sampler, input.uv).rgb;

    float3 color = (float3)1.0f - exp(-radiance * exposure);

    return float4(color, 1.0f);
}