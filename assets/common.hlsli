struct PrimitiveData {
    float4x4 MVP;
    float4x4 MV;
};

struct MaterialData {
    float4 baseColorFactor;
    float4 metalRoughnessFactor;
    uint baseColorTextureIdx;
    uint normalTextureIdx;
    uint metalRoughnessIdx;
};

struct LightConstantData {
    float4 position;
    float4 direction;

    float4 positionViewSpace;
    float4 directionViewSpace;

    float4 colorIntensity;

    float range;

    uint type;
};

#define LIGHT_POINT 0
#define LIGHT_DIRECTIONAL 1

struct LightPassConstantData {
    float4x4 inverseProjection;
    float4 environmentIntensity;
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

float3 ExpandNormal(float3 n)
{
    return n * 2.0f - 1.0f;
}

#ifndef NO_DEFAULT_RESOURCES

cbuffer Indices : register(b0) {
    const uint g_PrimitiveDataIndex;
    const uint g_MaterialDataIndex;
    const uint g_LightIndex;
    const uint g_LightPassDataIndex;
    const uint g_MiscDescriptorIndex;
};

SamplerState g_sampler : register(s0);

float4 DoNormalMap(Texture2D normalMap, float3x3 TBN, float2 uv)
{
    float3 normal = normalMap.Sample(g_sampler, uv).xyz;
    normal = ExpandNormal(normal);
    normal = mul(normal, TBN);
    return normalize(float4(normal, 0.0f));
}

ConstantBuffer<PrimitiveData> GetPrimitiveData() 
{
    return ResourceDescriptorHeap[g_PrimitiveDataIndex];
}

ConstantBuffer<MaterialData> GetMaterial()
{
    return ResourceDescriptorHeap[g_MaterialDataIndex];
}

ConstantBuffer<LightConstantData> GetLight()
{
    return ResourceDescriptorHeap[g_LightIndex];
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