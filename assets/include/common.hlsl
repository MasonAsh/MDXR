struct PrimitiveData {
    float4x4 MVP;
    float4x4 MV;
};

struct MaterialData {
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
    uint baseGBufferIdx;
    uint debug;
};

cbuffer Indices : register(b0) {
    const uint g_PrimitiveDataIndex;
    const uint g_MaterialDataIndex;
    const uint g_LightIndex;
    const uint g_LightPassDataIndex;
    const uint g_MiscDescriptorIndex;
};

static const float PI = 3.14159265f;
static const float Epsilon = 0.00001;

SamplerState g_sampler : register(s0);

ConstantBuffer<PrimitiveData> GetPrimitiveData() {
    ConstantBuffer<PrimitiveData> data = ResourceDescriptorHeap[g_PrimitiveDataIndex];
    return data;
}

ConstantBuffer<MaterialData> GetMaterial() {
    ConstantBuffer<MaterialData> data = ResourceDescriptorHeap[g_MaterialDataIndex];
    return data;
}

ConstantBuffer<LightConstantData> GetLight() {
    ConstantBuffer<LightConstantData> data = ResourceDescriptorHeap[g_LightIndex];
    return data;
}

ConstantBuffer<LightPassConstantData> GetLightPassData() {
    ConstantBuffer<LightPassConstantData> data = ResourceDescriptorHeap[g_LightPassDataIndex];
    return data;
}

Texture2D GetBaseColorTexture(MaterialData material) {
    return ResourceDescriptorHeap[material.baseColorTextureIdx];
}

Texture2D GetNormalTexture(MaterialData material) {
    return ResourceDescriptorHeap[material.normalTextureIdx];
}

Texture2D GetMetalRoughnessTexture(MaterialData material) {
    return ResourceDescriptorHeap[material.metalRoughnessIdx];
}

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

float4 DoNormalMap(Texture2D normalMap, float3x3 TBN, float2 uv)
{
    float3 normal = normalMap.Sample(g_sampler, uv).xyz;
    normal = ExpandNormal(normal);
    normal = mul(normal, TBN);
    return normalize(float4(normal, 0.0f));
}