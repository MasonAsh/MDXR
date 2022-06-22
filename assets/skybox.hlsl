#include "include/common.hlsl"

struct Vertex {
    float3 pos : POSITION;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float3 texcoord : TEXCOORD;
};

TextureCube GetSkyboxTexture()
{
    return ResourceDescriptorHeap[g_MiscDescriptorIndex];
}

PSInput VSMain(Vertex input)
{
    ConstantBuffer<PrimitiveData> primitive = GetPrimitiveData();

    PSInput output;
    // Keep skybox at the far plane by locking Z to W
    output.pos = mul(primitive.MVP, float4(input.pos, 0.0f)).xyww;
    output.pos.z = output.pos.z * 0.9999999f;
    // Sample the texcube with just vertex position
    output.texcoord = input.pos;

    return output;
}

float4 PSMain(PSInput input) : SV_TARGET0
{
    TextureCube skybox = GetSkyboxTexture();
    return skybox.Sample(g_sampler, input.texcoord);
}