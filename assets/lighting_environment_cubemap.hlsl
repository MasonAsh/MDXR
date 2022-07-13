#include "common.hlsli"
#include "pbr.hlsli"

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

#define BASE_COLOR_BUFFER 0
#define NORMAL_BUFFER 1
#define METAL_ROUGHNESS_BUFFER 2
#define DEPTH_BUFFER 3

TextureCube GetSkyboxTexture() 
{
    return ResourceDescriptorHeap[g_MiscDescriptorIndex];
}

Texture2D GetBRDFLUT()
{
    return ResourceDescriptorHeap[g_MaterialDataIndex];
}

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

    TextureCube skybox = GetSkyboxTexture();
    Texture2D lut = GetBRDFLUT();

    Texture2D baseColorTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + BASE_COLOR_BUFFER];
    Texture2D normalTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + NORMAL_BUFFER];
    Texture2D metalRoughnessTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + METAL_ROUGHNESS_BUFFER];
    Texture2D depthTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + DEPTH_BUFFER];

    float3 baseColor = pow(baseColorTexture.Sample(g_sampler, input.uv).rgb, 2.2);
    float depth = depthTexture.Sample(g_sampler, input.uv).r;
    float4 normal = normalTexture.Sample(g_sampler, input.uv);
    float4 metalRoughness = metalRoughnessTexture.Sample(g_sampler, input.uv);

    // Don't apply the effect any unlit pixels
    if (length(normal.rgb) < 0.001) {
        discard;
    }

    float roughness = metalRoughness.g;
    float metallic = metalRoughness.b;

    float4 viewPos = ScreenToView(float4(input.uv, depth, 1.0f));

    float attenuation = 1.0f;

    float3 N = normal.xyz;

    float3 worldSpaceNormal = mul(passData.inverseView, float4(N, 0.0)).xyz;

    // Direction from fragment to eye
    float3 V = normalize(-viewPos.xyz);
    float3 Wi = N;

    float3 worldSpaceView = mul(passData.inverseView, float4(V, 0.0)).xyz;

    float3 R = reflect(-worldSpaceView, worldSpaceNormal);

    float3 radiance = skybox.SampleLevel(g_sampler, R, roughness * (PREFILTER_MAP_MIPCOUNT - 1)).rgb * passData.environmentIntensity.rgb;
    
    float2 envUV = float2(max(dot(N, -V), 0.0f), roughness);
    float2 envBRDF = lut.Sample(g_sampler, envUV).rg;

    float3 H = normalize(Wi + V);

    float3 F0 = 0.04;
    F0 = lerp(F0, baseColor.rgb, metallic);

    float3 kS = FSchlickRoughness(max(0.0, dot(N, V)), F0, roughness);
    float3 kD = 1.0f - kS;
    float4 diffuseBRDF = float4(kD, 1.0f) * float4(baseColor, 1.0f);
    float3 specular = radiance * (kS * envBRDF.x + envBRDF.y);

    float4 Lo = (diffuseBRDF + float4(specular, 1.0f)) * float4(radiance, 1);

    float4 color = float4(Lo);

    color = color / (color + 1.0f);
    color = pow(color, 1.0/2.2);

    return color;
}