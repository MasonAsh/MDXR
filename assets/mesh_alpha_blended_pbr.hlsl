#include "common.hlsli"
#include "pbr.hlsli"

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 viewPos : POSITION;
    float2 uv : TEXCOORD;
    float3 normalVS : NORMAL;
    float3 tangentVS : TANGENT;
    float3 binormalVS : BINORMAL;
};

struct PSOutput {
    float4 backBuffer : SV_TARGET0;
};

PSInput VSMain(VSInput input)
{
    PSInput result;

    ConstantBuffer<PrimitiveData> primitive = GetPrimitiveData();

    result.position = mul(primitive.MVP, float4(input.position, 1.0f));

    float3 binormal = cross(input.normal, input.tangent);

    result.viewPos = mul(primitive.MV, float4(input.position, 1.0f));

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

    float4 baseColor = float4(0.5, 0.5, 0.5, 1.0);
    float4 normal = float4(normalize(input.normalVS), 0.0f);
    float4 metalRoughness = float4(0.0, 1.0, 0.0, 0.0);

    if (mat.baseColorTextureIdx != -1) {
        Texture2D texture = GetBaseColorTexture(mat);
        baseColor = texture.Sample(g_sampler, input.uv);
    }

    if (mat.normalTextureIdx != -1) {
        float3x3 TBN = float3x3(
            normalize(input.tangentVS),
            normalize(input.binormalVS),
            normalize(input.normalVS)
        );
        Texture2D normalMap = GetNormalTexture(mat);
        normal = DoNormalMap(normalMap, TBN, input.uv);
    }

    if (mat.metalRoughnessIdx != -1) {
        Texture2D texture = GetMetalRoughnessTexture(mat);
        metalRoughness = texture.Sample(g_sampler, input.uv);
    }

    float roughness = metalRoughness.g;
    float metallic = metalRoughness.b;

    float4 viewPos = input.viewPos;

    uint lightCount = g_LightPassDataIndex;

    result.backBuffer = float4(0, 0, 0, 0);

    for (uint i = 0; i < lightCount; i++) {
        ConstantBuffer<LightConstantData> light = ResourceDescriptorHeap[g_LightIndex + i];

        float3 lightToFragment = (-viewPos.xyz) -  (-light.positionViewSpace.xyz);
        float distance = length(lightToFragment);
        float attenuation = 1.0f / (distance * distance);

        float3 N = normal.xyz;

        // Light direction to fragment
        float3 Wi = lightToFragment / distance;
        // Direction from fragment to eye
        float3 Wo = normalize(-viewPos.xyz);

        if (light.type == LIGHT_DIRECTIONAL) {
            attenuation = 1.0f;
            // Light direction to fragment
            Wi = normalize(light.directionViewSpace.xyz);
            // Direction from fragment to eye
            Wo = normalize(-viewPos.xyz);
        }

        result.backBuffer += ShadePBR(
            light.colorIntensity.xyz,
            baseColor,
            N,
            roughness,
            metallic,
            Wi,
            Wo,
            attenuation
        );
    }

    return result;
}