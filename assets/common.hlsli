struct PrimitiveInstanceData
{
    float4x4 MVP;
    float4x4 MV;
    float4x4 M;
};

struct MaterialData
{
    float4 baseColorFactor;
    float4 metalRoughnessFactor;
    uint baseColorTextureIdx;
    uint normalTextureIdx;
    uint metalRoughnessIdx;
};

struct LightConstantData
{
    float4 position;
    float4 direction;

    float4 positionViewSpace;
    float4 directionViewSpace;

    float4 colorIntensity;

    float4x4 MVP;

    float range;
    uint shadowMapDescriptorIdx;
    uint type;

    uint castsShadow;

    float pad[24];
};

#define LIGHT_POINT 0
#define LIGHT_DIRECTIONAL 1

struct LightPassConstantData
{
    float4x4 inverseProjection;
    float4x4 inverseView;
    float4 environmentIntensity;
    float4 eyePosWorld;
    uint baseGBufferIdx;
    uint debug;
};

static const float PI = 3.14159265f;
static const float Epsilon = 0.00001;

float3 make_float3(float value)
{
    return float3(value, value, value);
}

float4 make_float4(float value)
{
    return float4(value, value, value, value);
}

float max_float3(float3 v)
{
    return max(v.x, max(v.y, v.z));
}

#define PREFILTER_MAP_MIPCOUNT 6

#define GBUFFER_RADIANCE 0
#define GBUFFER_BASE_COLOR 1
#define GBUFFER_NORMAL 2
#define GBUFFER_METAL_ROUGHNESS 3
#define GBUFFER_DEPTH 4

#ifndef NO_DEFAULT_RESOURCES

cbuffer Indices : register(b0) {
    const uint g_PrimitiveDataIndex;
    const uint g_MaterialDataIndex;
    const uint g_LightIndex;
    const uint g_LightPassDataIndex;
    const uint g_MiscDescriptorIndex;
};

SamplerState g_sampler : register(s0);
SamplerState g_samplerShadowMap : register(s1);

float4 DoNormalMap(Texture2D normalMap, float3x3 TBN, float2 uv)
{
    float3 normal = normalMap.Sample(g_sampler, uv).xyz;
    normal = normal * 2.0f - 1.0f;
    normal = mul(normal, TBN);
    return float4(normalize(normal), 0.0);
}

ConstantBuffer<PrimitiveInstanceData> GetPrimitiveInstanceData(uint instance) 
{
    return ResourceDescriptorHeap[g_PrimitiveDataIndex + instance];
}

ConstantBuffer<PrimitiveInstanceData> GetPrimitiveData()
{
    return GetPrimitiveInstanceData(0);
}

ConstantBuffer<MaterialData> GetMaterial()
{
    return ResourceDescriptorHeap[g_MaterialDataIndex];
}

ConstantBuffer<LightConstantData> GetLight()
{
    return ResourceDescriptorHeap[g_LightIndex];
}

ConstantBuffer<LightConstantData> GetLightAtOffset(uint offset)
{
    return ResourceDescriptorHeap[g_LightIndex + offset];
}

ConstantBuffer<LightPassConstantData> GetLightPassData() 
{
    return ResourceDescriptorHeap[g_LightPassDataIndex];
}

Texture2D GetBaseColorTexture(MaterialData material) 
{
    return ResourceDescriptorHeap[material.baseColorTextureIdx];
}

Texture2D GetNormalTexture(MaterialData material)
{
    return ResourceDescriptorHeap[material.normalTextureIdx];
}

Texture2D GetMetalRoughnessTexture(MaterialData material)
{
    return ResourceDescriptorHeap[material.metalRoughnessIdx];
}

#endif