#include "common.hlsli"
#include "pbr.hlsli"

struct VSInput {
    uint instanceID : SV_InstanceID;
    float3 position : POSITION;
};

struct PSInput {
    float4 position : SV_POSITION;
};

PSInput VSMain(VSInput input)
{
    PSInput result;

    ConstantBuffer<PrimitiveInstanceData> primitive = GetPrimitiveInstanceData(input.instanceID);
    ConstantBuffer<LightConstantData> light = GetLight();

    //float4x4 MVP = light.directionalLightViewProjection * primitive.M;
    float4x4 M = primitive.M;
    float4x4 VP = light.MVP;
    //float4x4 MVP = mul(VP, M);
    float4x4 MVP = mul(VP, M);
    float4 aPos = float4(input.position, 1.0f);
    result.position = mul(MVP, float4(input.position, 1.0f));

    return result;
}
