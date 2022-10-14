// Performs the filtering step of bloom, extracting pixels which
// exceed a certain luminance.

#define NO_DEFAULT_RESOURCES
#include "common.hlsli"

#define BLOCK_SIZE 8

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

cbuffer Constants : register(b0)
{
    float g_threshold;
    uint g_sourceTextureIdx;
};

SamplerState g_sampler : register(s0);

float RGBToLuminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput result;
    result.uv = float2(id % 2, (id % 4) >> 1);
    result.pos = float4((result.uv.x - 0.5f) * 2, -(result.uv.y - 0.5f) * 2, 0, 1);
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET0
{
    Texture2D<float4> sourceTexture = ResourceDescriptorHeap[g_sourceTextureIdx];

    float2 sourceDimensions;
    sourceTexture.GetDimensions(sourceDimensions.x, sourceDimensions.y);

    // float2 sourceUV = dispatchID.xy / sourceDimensions;
    // float2 destUV = dispatchID.xy / bloomDimensions;
    float2 sourceUV = input.uv;

    float4 color = sourceTexture.SampleLevel(g_sampler, input.uv, 0);
    float luma = RGBToLuminance(color.rgb);

    if (luma > g_threshold) {
        return color;
    } else {
        return (float4)0;
    }
}