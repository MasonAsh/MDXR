#include "common.hlsli"
#include "pbr.hlsli"

struct PSInput {
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
    ConstantBuffer<LightPassConstantData> passData = GetLightPassData();
    ConstantBuffer<LightConstantData> light = GetLight();

    Texture2D baseColorTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_BASE_COLOR];
    Texture2D normalTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_NORMAL];
    Texture2D metalRoughnessTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_METAL_ROUGHNESS];
    Texture2D depthTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_DEPTH];
    Texture2D shadowMap = ResourceDescriptorHeap[light.directionalShadowMapDescriptorIdx];

    float3 baseColor = baseColorTexture.Sample(g_sampler, input.uv).rgb;
    float depth = depthTexture.Sample(g_sampler, input.uv).r;
    float4 normal = normalTexture.Sample(g_sampler, input.uv);
    float4 metalRoughness = metalRoughnessTexture.Sample(g_sampler, input.uv);

    float roughness = metalRoughness.g;
    float metallic = metalRoughness.b;

    float4 viewPos = ScreenToView(float4(input.uv, depth, 1.0f));

    float4 worldPos = mul(passData.inverseView, viewPos);

    float attenuation = 1.0f;

    float3 N = normal.xyz;

    // Light direction to fragment
    float3 Wi = normalize(-light.directionViewSpace.xyz);
    // Direction from fragment to eye
    float3 Wo = normalize(-viewPos.xyz);
    
    float4 lightPos = mul(light.MVP, float4(worldPos.xyz, 1.0f));

    float3 projCoords = lightPos.xyz / lightPos.w;
    projCoords.xy = projCoords.xy * 0.5f + 0.5f;
    // DirectX UVs are vertically flipped from OpenGL
    projCoords.y *= -1;
    float closestDepth = shadowMap.Sample(g_sampler, projCoords.xy).r;
    float currentDepth = projCoords.z;
    float shadow = currentDepth > closestDepth ? 1.0f : 0.0f;

    //if (passData.debug) return float4(projCoords.xy, 0.0f, 1.0f);
    if (passData.debug) return (float4)closestDepth;

    float4 color = ShadePBR(
        light.colorIntensity.xyz,
        float4(baseColor, 1.0f),
        N,
        roughness,
        metallic,
        Wi,
        Wo,
        attenuation
    );
    color.rgb *=  (1.0f - shadow);

    return color;
}
