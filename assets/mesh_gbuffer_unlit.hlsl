#include "include/common.hlsl"

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

struct PSOutput {
    float4 backBuffer : SV_TARGET;
};

PSInput VSMain(VSInput input)
{
    PSInput result;

    PrimitiveData primitiveData = GetPrimitiveData();

    result.position = mul(primitiveData.MVP, float4(input.position, 1.0f));
    result.uv = input.uv;

    return result;
}

PSOutput PSMain(PSInput input)
{
    PSOutput result;

    MaterialData mat = GetMaterial();

    // TODO: these checks be done through preprocessor and shader permutations instead
    if (mat.baseColorTextureIdx != -1) {
        Texture2D baseColorTexture = GetBaseColorTexture(mat);
        result.backBuffer = baseColorTexture.Sample(g_sampler, input.uv);
    } else {
        result.backBuffer = float4(1.0f, 0.07, 0.57, 1.0);
    }

    return result;
}