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

RaytracingAccelerationStructure GetSceneAccelStruct()
{
    return ResourceDescriptorHeap[g_MaterialDataIndex];
}

float ComputeShadow(LightConstantData light, float3 worldPos)
{
    if (!light.castsShadow) {
        return 0.0f;
    }

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES>
        rayQuery;

    float3 hitToLight = -light.direction.xyz;

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

    float3 baseColor = baseColorTexture.Sample(g_sampler, input.uv).rgb;
    float depth = depthTexture.Sample(g_sampler, input.uv).r;
    float4 normal = normalTexture.Sample(g_sampler, input.uv);
    float4 metalRoughness = metalRoughnessTexture.Sample(g_sampler, input.uv);

    float roughness = metalRoughness.g;
    float metallic = metalRoughness.b;

    float4 viewPos = ScreenToView(float4(input.uv, depth, 1.0f));

    float3 worldPos = mul(passData.inverseView, viewPos).xyz;
    float shadow = ComputeShadow(light, worldPos);

    float attenuation = 1.0f;

    float3 N = normal.xyz;

    // Light direction to fragment
    float3 L = normalize(-light.direction.xyz);
    // Direction from fragment to eye
    float3 V = normalize(passData.eyePosWorld.xyz - worldPos.xyz);

    float4 color = ShadePBR(
        light.colorIntensity.xyz,
        float4(baseColor, 1.0f),
        N,
        roughness,
        metallic,
        L,
        V,
        attenuation
    );
    color.rgb *=  (1.0f - shadow);

    return color;
}
