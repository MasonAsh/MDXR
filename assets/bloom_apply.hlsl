#define NO_COMMON_RESOURCES
#include "common.hlsli"

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

cbuffer Indices : register(b0) {
    uint g_bloomTextureIdx;
};

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput result;
    result.uv = float2(id % 2, (id % 4) >> 1);
    result.pos = float4((result.uv.x - 0.5f) * 2, -(result.uv.y - 0.5f) * 2, 0, 1);
    return result;
}

// Writes bloom texture into the radiance buffer
float4 PSMain(PSInput input) : SV_TARGET0
{
    Texture2D<float4> bloomTexture = ResourceDescriptorHeap[g_bloomTextureIdx];
    float4 bloomColor = bloomTexture.SampleLevel(g_sampler, input.uv, 0);
    return bloomColor;
}