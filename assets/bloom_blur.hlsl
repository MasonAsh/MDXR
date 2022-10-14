// Performs the gausian blur step of bloom

#define NO_DEFAULT_RESOURCES
#include "common.hlsli"

#define BLOCK_SIZE 8

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

cbuffer Constants : register(b0)
{
    uint g_sourceTextureIdx;
    uint g_horizontal;
};

SamplerState g_sampler : register(s0);

#define MATRIX_SIZE 5

static const float weights[MATRIX_SIZE] = {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216};

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

    float2 dimensions;
    sourceTexture.GetDimensions(dimensions.x, dimensions.y);

    float2 direction = g_horizontal ? float2(1.0f, 0.0f) : float2(0.0f, 1.0f);
    float2 uvOffset = direction / dimensions;

    float3 result = sourceTexture.SampleLevel(g_sampler, input.uv, 0).rgb * weights[0];

    for (uint i = 1; i < 5; i++) {
        result += sourceTexture.SampleLevel(g_sampler, input.uv + (uvOffset * i), 0).rgb * weights[i];
        result += sourceTexture.SampleLevel(g_sampler, input.uv - (uvOffset * i), 0).rgb * weights[i];
    }

    return float4(result, 1.0);
}
