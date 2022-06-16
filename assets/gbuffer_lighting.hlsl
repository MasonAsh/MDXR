#include "include/common.hlsl"

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

#define DIFFUSE_BUFFER 0
#define NORMAL_BUFFER 1
#define DEPTH_BUFFER 2

SamplerState g_samp : register(s0);

float4 ClipToView(float4 clipCoord)
{
    float4 view = mul(GetLightPassData().inverseProjection, clipCoord);
    view = view / view.w;
    return view;
}

float4 ScreenToView(float4 screen)
{
    float2 texcoord = screen.xy;
    float4 clip = float4(float2(texcoord.x, 1.0f - texcoord.y) * 2.0f - 1.0f, screen.z, screen.w);
    return ClipToView(clip);
}

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput result;
    result.uv = float2(id % 2, (id % 4) >> 1);
    result.pos = float4((result.uv.x - 0.5f) * 2, -(result.uv.y - 0.5f) * 2, 0, 1);
    return result;
}

// P = position of point being shaded in view space
// N = normal of point being shaded in view space

[earlydepthstencil]
float4 PSMain(PSInput input) : SV_TARGET
{
    ConstantBuffer<LightPassConstantData> passData = GetLightPassData();
    ConstantBuffer<LightConstantData> light = GetLight();

    Texture2D diffuseTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + DIFFUSE_BUFFER];
    Texture2D normalTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + NORMAL_BUFFER];
    Texture2D depthTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + DEPTH_BUFFER];

    float depth = depthTexture.Sample(g_samp, input.uv).r;
    float4 normal = normalTexture.Sample(g_samp, input.uv);

    float4 P = ScreenToView(float4(input.uv, depth, 1.0f));
    float4 N = float4(normal.xyz, 1.0f);

    float4 L = light.positionViewSpace - P;
    float distance = length(L);
    L = L / distance;

    float attenuation = 1.0f - smoothstep(light.range * 0.75f, light.range, distance);

    float4 diffuseColor = diffuseTexture.Sample(g_samp, input.uv);
    float4 diffuseFactor = light.color * max(dot(N, L), 0);
    float4 diffuse = diffuseFactor * attenuation * light.intensity * diffuseColor;

    if (passData.debug) {
        float2 texcoord = input.uv;
        float4 clip = float4(float2(texcoord.x, 1.0f - texcoord.y) * 2.0f - 1.0f, depth, 1.0f);
        return float4(1.0f, 1.0f, 1.0f,1.0f);
    } else {
        return diffuse;
    }
}