struct PerMeshData {
    float4x4 MVP;
    float4x4 MV;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

ConstantBuffer<PerMeshData> PerMesh : register(b0);

Texture2D diffuseTexture : register(t0);
SamplerState samp : register(t0);

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput result;
    result.uv = float2(id % 2, (id % 4) >> 1);
    result.pos = float4((result.uv.x - 0.5f) * 2, -(result.uv.y - 0.5f) * 2, 0, 1);
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return diffuseTexture.Sample(samp, input.uv);
}