#include "common.hlsli"

struct VSInput {
    uint instance : SV_InstanceID;
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
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
    float4 baseColor : SV_TARGET1;
    float4 normal : SV_TARGET2;
    float4 metalRoughness : SV_TARGET3;
};

PSInput VSMain(VSInput input)
{
    PSInput result;

    ConstantBuffer<PrimitiveInstanceData> primitive = GetPrimitiveInstanceData(input.instance);

    result.position = mul(primitive.MVP, float4(input.position, 1.0f));

    float3 binormal = cross(input.normal, input.tangent.xyz) * input.tangent.w;

    // Write out world space normals
    float3x3 MV3 = (float3x3)primitive.M;

    result.normalVS = mul(MV3, input.normal);
    result.tangentVS = mul(MV3, input.tangent.xyz);
    result.binormalVS = mul(MV3, binormal);

    result.uv = input.uv;

    return result;
}

PSOutput PSMain(PSInput input)
{
    PSOutput result;

    ConstantBuffer<MaterialData> mat = GetMaterial();

    // We can write ambient light straight to the backbuffer.
    float4 ambient = { 0.0, 0.0, 0.0, 1.0 };
    result.backBuffer = ambient;


    result.baseColor = mat.baseColorFactor;
    // TODO: these checks be done through preprocessor and shader permutations instead
    if (mat.baseColorTextureIdx != -1) {
        Texture2D baseColorTexture = GetBaseColorTexture(mat);
        result.baseColor = result.baseColor * baseColorTexture.Sample(g_sampler, input.uv);
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

    result.metalRoughness = mat.metalRoughnessFactor;
    if (mat.metalRoughnessIdx != -1) {
        Texture2D metalRoughness = GetMetalRoughnessTexture(mat);
        result.metalRoughness = result.metalRoughness * metalRoughness.Sample(g_sampler, input.uv);
    }

    return result;
}