#include "common.hlsli"
#include "pbr.hlsli"

struct PSInput 
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

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
float4 PSMain(PSInput input) : SV_TARGET
{
    uint lightCount = g_MiscDescriptorIndex;

    float4 finalColor = (float4)0;

    for (uint i = 0; i < lightCount; i++) {
        ConstantBuffer<LightPassConstantData> passData = GetLightPassData();
        ConstantBuffer<LightConstantData> light = GetLightAtOffset(i);

        Texture2D baseColorTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_BASE_COLOR];
        Texture2D normalTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_NORMAL];
        Texture2D metalRoughnessTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_METAL_ROUGHNESS];
        Texture2D depthTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_DEPTH];

        float3 baseColor = baseColorTexture.Sample(g_sampler, input.uv).rgb;
        float depth = depthTexture.Sample(g_sampler, input.uv).r;
        float4 normal = normalTexture.Sample(g_sampler, input.uv);
        float4 metalRoughness = metalRoughnessTexture.Sample(g_sampler, input.uv);

        float roughness = metalRoughness.g;
        float metallic = metalRoughness.b;

        float4 viewPos = ScreenToView(float4(input.uv, depth, 1.0f));

        float3 lightToFragment = (-viewPos.xyz) -  (-light.positionViewSpace.xyz);
        float distance = length(lightToFragment);
        float attenuation = 1.0f / (distance * distance);

        float3 N = normal.xyz;

        // Light direction to fragment
        float3 Wi = lightToFragment / distance;
        // Direction from fragment to eye
        float3 Wo = normalize(-viewPos.xyz);

        finalColor += ShadePBR(
            light.colorIntensity.xyz,
            float4(baseColor, 1.0f),
            N,
            roughness,
            metallic,
            Wi,
            Wo,
            attenuation
        );
    }

    return finalColor;
}
