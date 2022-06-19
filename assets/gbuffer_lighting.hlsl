#include "include/common.hlsl"

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

#define BASE_COLOR_BUFFER 0
#define NORMAL_BUFFER 1
#define METAL_ROUGHNESS_BUFFER 2
#define DEPTH_BUFFER 3

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

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float numerator = a2;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = PI * denom * denom;

    return numerator / denom;
}

float GSchlickGGX(float cosTheta, float k)
{
    float denom = cosTheta * (1.0f - k) + k;
    
    return cosTheta / denom;
}

float GeometrySmith(float cosWi, float cosWo, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return GSchlickGGX(cosWi, k) * GSchlickGGX(cosWo, k);
}

float3 FSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
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

    Texture2D baseColorTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + BASE_COLOR_BUFFER];
    Texture2D normalTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + NORMAL_BUFFER];
    Texture2D metalRoughnessTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + METAL_ROUGHNESS_BUFFER];
    Texture2D depthTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + DEPTH_BUFFER];

    float3 baseColor = pow(baseColorTexture.Sample(g_samp, input.uv).rgb, 2.2);
    float depth = depthTexture.Sample(g_samp, input.uv).r;
    float4 normal = normalTexture.Sample(g_samp, input.uv);
    float4 metalRoughness = metalRoughnessTexture.Sample(g_samp, input.uv);

    float roughness = metalRoughness.g;
    float metallic = metalRoughness.b;

    float4 viewPos = ScreenToView(float4(input.uv, depth, 1.0f));

    float3 lightToFragment = (-viewPos.xyz) -  (-light.positionViewSpace.xyz);
    float distance = length(lightToFragment);

    // Light direction to fragment
    float3 Wi = lightToFragment / distance;
    // Direction from fragment to eye
    float3 Wo = normalize(-viewPos.xyz);
    // halfway vector
    float3 H = normalize(Wi + Wo);

    float3 N = normal.xyz;

    float attenuation = 1.0f / (distance * distance);

    float3 radiance = light.colorIntensity.xyz * attenuation;

    float3 F0 = 0.04;
    F0 = lerp(F0, baseColor, metallic);

    float cosWi = max(0.0, dot(N, Wi));
    float cosWh = max(0.0, dot(N, H));
    float cosWo = max(0.0, dot(N, Wo));

    float3 F = FSchlick(max(0.0, dot(H, Wo)), F0);
    float G = DistributionGGX(cosWh, roughness);
    float D = GeometrySmith(cosWi, cosWo, roughness);

    float3 kS = F;
    float3 kD = lerp(float3(1,1,1) - F, float3(0, 0, 0), metallic);
    // float3 kD = 1.0f - kS;
    // kD *= 1.0f - metallic;

    float3 numerator = F * D * G;
    //float denominator = 4.0f * max(dot(N, Wo), 0.0) * max(dot(N, Wi), 0.0) + 0.0001;
    float denominator = max(Epsilon, 4.0 * cosWi * cosWo);
    float3 specular = numerator / denominator;

    float3 diffuseBRDF = kD * baseColor;

    float3 Lo = (diffuseBRDF + specular) * radiance * cosWi;

    float4 color = float4(Lo, 1.0f);

    color = color / (color + 1.0f);
    color = pow(color, 1.0/2.2);

    if (passData.debug) {
        float4 debug;
        debug.a = 1.0f;
        debug.rgb = specular * radiance * cosWi;
        return debug;
    }

    return color;
}
