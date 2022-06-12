struct PerPrimitiveData {
    float4x4 MVP;
    float4x4 MV;
    uint diffuseTextureIdx;
    uint normalTextureIdx;
};

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
    float4 diffuse : SV_TARGET0;
    float4 normal : SV_TARGET1;
};

ConstantBuffer<PerPrimitiveData> g_perPrimitive : register(b0);

Texture2D g_tex2DTable[] : register(t0);
SamplerState g_sampler : register(s0);

float3 ExpandNormal(float3 n)
{
    return n * 2.0f - 1.0f;
}

float4 DoNormalMap(float3x3 TBN, float2 uv)
{
    float3 normal = g_tex2DTable[g_perPrimitive.normalTextureIdx].Sample(g_sampler, uv).xyz;
    normal = ExpandNormal(normal);
    normal = mul(normal, TBN);
    return normalize(float4(normal, 0.0f));
}

PSInput VSMain(VSInput input)
{
    PSInput result;

    result.position = mul(g_perPrimitive.MVP, float4(input.position, 1.0f));

    float3 binormal = cross(input.normal, input.tangent);

    float3x3 MV3 = (float3x3)g_perPrimitive.MV;

    result.normalVS = mul(MV3, input.normal);
    result.tangentVS = mul(MV3, input.tangent);
    result.binormalVS = mul(MV3, binormal);

    result.uv = input.uv;

    return result;
}

PSOutput PSMain(PSInput input)
{
    PSOutput result;

    // TODO: these checks be done through preprocessor and shader permutations instead
    if (g_perPrimitive.diffuseTextureIdx != -1) {
        Texture2D diffuseTexture = g_tex2DTable[g_perPrimitive.diffuseTextureIdx];
        result.diffuse = diffuseTexture.Sample(g_sampler, input.uv);
    } else {
        result.diffuse = float4(1.0f, 0.07, 0.57, 1.0);
    }

    if (g_perPrimitive.normalTextureIdx != -1) {
        float3x3 TBN = float3x3(
            normalize(input.tangentVS),
            normalize(input.binormalVS),
            normalize(input.normalVS)
        );
        result.normal = DoNormalMap(TBN, input.uv);
    } else {
        result.normal = float4(input.normalVS, 1.0f);
    }

    return result;
}