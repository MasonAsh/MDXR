struct PrimitiveData {
    float4x4 MVP;
    float4x4 MV;
};

struct MaterialData {
    uint diffuseTextureIdx;
    uint normalTextureIdx;
    uint metalRoughnessIdx;
};

struct LightConstantData {
    float4 position;
    float4 direction;

    float4 positionViewSpace;
    float4 directionViewSpace;

    float4 color;

    float range;
    float intensity;
};

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
};

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

Texture2D GetDiffuseTexture(MaterialData material) {
    return ResourceDescriptorHeap[material.diffuseTextureIdx];
}

Texture2D GetNormalTexture(MaterialData material) {
    return ResourceDescriptorHeap[material.normalTextureIdx];
}
