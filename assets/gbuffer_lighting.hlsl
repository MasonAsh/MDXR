struct PerPrimitiveData {
    float4x4 MVP;
    float4x4 MV;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

ConstantBuffer<PerPrimitiveData> PerPrimitive : register(b0);

#define DIFFUSE_BUFFER 0
#define NORMAL_BUFFER 1

Texture2D GBuffer[2] : register(t0);
SamplerState samp : register(s0);

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput result;
    result.uv = float2(id % 2, (id % 4) >> 1);
    result.pos = float4((result.uv.x - 0.5f) * 2, -(result.uv.y - 0.5f) * 2, 0, 1);
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return GBuffer[DIFFUSE_BUFFER].Sample(samp, input.uv);
}