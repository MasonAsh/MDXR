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
    float3 normalVS : NORMAL;
    float3 tangentVS : TANGENT;
    float3 binormalVS : BINORMAL;
};

struct PSOutput {
    float4 backBuffer : SV_TARGET0;
    float4 diffuse : SV_TARGET1;
    float4 normal : SV_TARGET2;
    float4 metalRoughness : SV_TARGET3;
};

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

PSInput VSMain(VSInput input)
{
    PSInput result;

    ConstantBuffer<PrimitiveData> primitive = GetPrimitiveData();

    result.position = mul(primitive.MVP, float4(input.position, 1.0f));

    float3 binormal = cross(input.normal, input.tangent);

    float3x3 MV3 = (float3x3)primitive.MV;

    result.normalVS = mul(MV3, input.normal);
    result.tangentVS = mul(MV3, input.tangent);
    result.binormalVS = mul(MV3, binormal);

    result.uv = input.uv;

    return result;
}

PSOutput PSMain(PSInput input)
{
    PSOutput result;

    ConstantBuffer<MaterialData> mat = GetMaterial();

    // We can write ambient light straight to the backbuffer.
    float4 ambient = { 0.05, 0.05, 0.05, 1.0 };
    result.backBuffer = ambient;

    // TODO: these checks be done through preprocessor and shader permutations instead
    if (mat.diffuseTextureIdx != -1) {
        Texture2D diffuseTexture = GetDiffuseTexture(mat);
        result.diffuse = diffuseTexture.Sample(g_sampler, input.uv);
    } else {
        result.diffuse = float4(1.0f, 0.07, 0.57, 1.0);
    }

    if (mat.normalTextureIdx != -1) {
        float3x3 TBN = float3x3(
            normalize(input.tangentVS),
            normalize(input.binormalVS),
            normalize(input.normalVS)
        );
        Texture2D normalMap = GetNormalTexture(mat);
        result.normal = DoNormalMap(normalMap, TBN, input.uv);
    } else {
        result.normal = float4(normalize(input.normalVS), 0.0f);
    }

    if (mat.metalRoughnessIdx != -1) {
        Texture2D metalRoughness = GetMetalRoughnessTexture(mat);
        result.metalRoughness = metalRoughness.Sample(g_sampler, input.uv);
    } else {
        // Default to non-metal with 0.5 roughness
        result.metalRoughness = float4(0.0f, 1.0f, 0.0f, 1.0f);
    }

    return result;
}