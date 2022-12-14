#include "common.hlsli"

struct VSInput {
    uint instance : SV_InstanceID;
    float3 position : POSITION;
};

struct PSInput {
    float4 position : SV_POSITION;
};

struct PSOutput {
    float4 backBuffer : SV_TARGET;
};

PSInput VSMain(VSInput input)
{
    PSInput result;

    ConstantBuffer<PrimitiveInstanceData> primitiveData = GetPrimitiveInstanceData(input.instance);

    result.position = mul(primitiveData.MVP, float4(input.position, 1.0f));

    return result;
}

PSOutput PSMain(PSInput input)
{
    PSOutput result;

    float4 color = float4(1.0f, 0.07, 0.57, 1.0);

    if (g_MaterialDataIndex != -1) {
        MaterialData mat = GetMaterial();
        color = mat.baseColorFactor;
    }

    result.backBuffer = color;

    return result;
}