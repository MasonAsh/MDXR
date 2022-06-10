struct PerMeshData {
    float4x4 MVP;
};

struct VSInput {
    float3 normal : NORMAL;
    float3 position : POSITION;
    float2 uv : TEXCOORD;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

struct PSOutput {
    float4 diffuse : SV_TARGET0;
};

ConstantBuffer<PerMeshData> PerMesh : register(b0);

Texture2D diffuseTexture : register(t0);
SamplerState samp : register(t0);

PSInput VSMain(VSInput input)
{
    PSInput result;

    result.position = mul(PerMesh.MVP, float4(input.position, 1.0f));
    result.uv = input.uv;

    return result;
}

PSOutput PSMain(PSInput input)
{
    PSOutput result;
    result.diffuse = diffuseTexture.Sample(samp, input.uv);
    return result;
}