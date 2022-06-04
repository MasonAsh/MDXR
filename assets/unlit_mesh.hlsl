struct Constants
{
    float4x4 MVP;
};

struct VSInput {
    float3 normal : NORMAL;
    float3 position : POSITION;
};

struct PSInput {
    float4 position : SV_POSITION;
};

ConstantBuffer<Constants> Globals : register(b0);

PSInput VSMain(VSInput input)
{
    PSInput result;

    result.position = mul(Globals.MVP, float4(input.position, 1.0f));

    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return float4(1, 1, 1, 1);
}