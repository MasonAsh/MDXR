struct PerPrimitiveData {
    float4x4 MVP;
    float4x4 MV;
    uint diffuseTextureIdx;
    uint normalTextureIdx;
};

struct VSInput {
    float3 normal : NORMAL;
    float3 position : POSITION;
    float2 uv : TEXCOORD;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
    float4 normal : NORMAL;
};

struct PSOutput {
    float4 diffuse : SV_TARGET0;
    float4 normal : SV_TARGET1;
};

ConstantBuffer<PerPrimitiveData> g_perPrimitive : register(b0);

Texture2D g_tex2DTable[] : register(t0);
SamplerState g_samp : register(s0);

PSInput VSMain(VSInput input)
{
    PSInput result;
    result.position = mul(g_perPrimitive.MVP, float4(input.position, 1.0f));
    result.uv = input.uv;
    result.normal = float4(input.normal, 1.0f);
    return result;
}

PSOutput PSMain(PSInput input)
{
    PSOutput result;

    // TODO: these checks be done through preprocessor and shader permutations instead
    if (g_perPrimitive.diffuseTextureIdx != -1) {
        Texture2D diffuseTexture = g_tex2DTable[g_perPrimitive.diffuseTextureIdx];
        result.diffuse = diffuseTexture.Sample(g_samp, input.uv);
    } else {
        result.diffuse = float4(1.0f, 0.07, 0.57, 1.0);
    }

    if (g_perPrimitive.normalTextureIdx != -1) {
        Texture2D normalTexture = g_tex2DTable[g_perPrimitive.normalTextureIdx];
        result.normal = normalTexture.Sample(g_samp, input.uv);
    } else {
        result.normal = input.normal;
    }

    return result;
}