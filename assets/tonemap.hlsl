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

float3 filmic(float3 x) {
  float3 X = max((float3)0.0, x - 0.004);
  float3 result = (X * (6.2 * X + 0.5)) / (X * (6.2 * X + 1.7) + 0.06);
  return pow(result, (float3)2.2);
}

// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}

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

    //float3 color = (float3)1.0f - exp(-radiance * exposure);

    // float3 color = radiance * exposure;
    // color = pow(color, 1/gamma);

    //float3 color = filmic(radiance);
    float3 color = radiance;

    color = color * exposure;
    color = ACESFilm(color);
    color = pow(color, (float3)1.0f/gamma);

    return float4(color, 1.0f);
}