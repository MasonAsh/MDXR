#include "common.hlsli"
#include "pbr.hlsli"

struct PSInput 
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

TextureCube GetSkyboxTexture() 
{
    return ResourceDescriptorHeap[g_MiscDescriptorIndex];
}

TextureCube GetIrradianceMap()
{
    return ResourceDescriptorHeap[g_LightIndex];
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

RaytracingAccelerationStructure GetSceneAccelStruct()
{
    return ResourceDescriptorHeap[g_PrimitiveDataIndex];
}

float ComputeShadow(float3 R, float3 worldPos)
{
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES>
        rayQuery;

    float3 hitToLight = R;

    RayDesc ray;
    ray.Origin = worldPos;
    ray.Direction = normalize(hitToLight);
    ray.TMin = 0.05;
    ray.TMax = 1000.0f;

    rayQuery.TraceRayInline(
        GetSceneAccelStruct(),
        0,
        0xFF,
        ray
    );

    rayQuery.Proceed();

    return rayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1.0f : 0.0f;
}

float3 GetSpecularLight(TextureCube skybox, float3 R, float roughness, float3 envIntensity)
{
    float mip = roughness * ((float)PREFILTER_MAP_MIPCOUNT - 1);
    float4 prefilterSample = skybox.SampleLevel(g_sampler, R, mip);
    float3 prefilterColor = prefilterSample.rgb * envIntensity;
    return prefilterColor;
}

float3 GetDiffuseLight(TextureCube diffuseMap, float3 N, float3 envIntensity)
{
    float4 irradianceSample = diffuseMap.Sample(g_sampler, N);
    float3 irradiance = irradianceSample.rgb * envIntensity;
    return irradiance;
}

// P = position of point being shaded in view space
// N = normal of point being shaded in view space
[earlydepthstencil]
float4 PSMain(PSInput input) : SV_TARGET
{
    ConstantBuffer<LightPassConstantData> passData = GetLightPassData();

    TextureCube skybox = GetSkyboxTexture();
    TextureCube irradianceMap = GetIrradianceMap();
    Texture2D lut = GetBRDFLUT();

    Texture2D baseColorTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_BASE_COLOR];
    Texture2D normalTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_NORMAL];
    Texture2D metalRoughnessTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_METAL_ROUGHNESS];
    Texture2D depthTexture = ResourceDescriptorHeap[passData.baseGBufferIdx + GBUFFER_DEPTH];

    float3 baseColor = baseColorTexture.Sample(g_sampler, input.uv).rgb;
    float depth = depthTexture.Sample(g_sampler, input.uv).r;
    float4 normal = normalTexture.Sample(g_sampler, input.uv);
    float4 metalRoughness = metalRoughnessTexture.Sample(g_sampler, input.uv);

    //baseColor = pow(baseColor, 2.2);

    // Don't apply the effect to any unlit pixels
    if (length(normal.rgb) < 0.001) {
        discard;
    }

    float roughness = metalRoughness.g;
    float metallic = metalRoughness.b;
    roughness = max(roughness, 0.04);

    float ao = metalRoughness.r;

    float4 viewPos = ScreenToView(float4(input.uv, depth, 1.0f));
    float3 worldPos = mul(passData.inverseView, viewPos).xyz;

    float attenuation = 1.0f;

    float3 N = normal.xyz;

    //float3 worldSpaceNormal = mul(passData.inverseView, float4(N, 0.0)).xyz;

    // Direction from fragment to eye
    float3 V = normalize(-viewPos.xyz);

    float3 worldSpaceView = mul(passData.inverseView, float4(V, 0.0)).xyz;

    float3 R = reflect(-worldSpaceView, N);


    float3 diffuseLight = GetDiffuseLight(irradianceMap, N, passData.environmentIntensity.rgb);
    float3 specularLight = GetSpecularLight(skybox, R, roughness, passData.environmentIntensity.rgb);

    float3 F0 = 0.04;
    float3 diffuseColor = baseColor.rgb * (1.0 - F0) * (1.0 - metallic);

    float NdotV = saturate(dot(N, worldSpaceView));
    
    float2 envUV = float2(NdotV, 1.0 - roughness);
    float2 envBRDF = lut.Sample(g_sampler, envUV).rg;

    F0 = lerp(F0, baseColor.rgb, metallic);
    float3 Fr = max((float3)(1.0 - roughness), F0) - F0;
    float3 kS = F0 + Fr * pow(1.0 - NdotV, 5.0);
    float3 FssEss = kS * envBRDF.x + envBRDF.y;

    float Ems = (1.0 - (envBRDF.x + envBRDF.y));
    float3 fAvg = F0 + (1.0 - F0) / 21.0;
    float3 FmsEms = Ems * FssEss * fAvg / (1.0 - fAvg * Ems);
    float3 kD = diffuseColor * (1.0 - FssEss - FmsEms);
    float3 shade = FssEss * specularLight + (FmsEms + kD) * diffuseLight;

    float4 color = float4(shade, 1.0);

    return color;
}
