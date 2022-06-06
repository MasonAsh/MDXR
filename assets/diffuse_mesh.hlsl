struct Constants
{
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

ConstantBuffer<Constants> Globals : register(b0);

Texture2D texture : register(t0);
SamplerState samp : register(t0);

PSInput VSMain(VSInput input)
{
    PSInput result;

    result.position = mul(Globals.MVP, float4(input.position, 1.0f));
    result.uv = input.uv;

    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return texture.Sample(samp, input.uv);
}