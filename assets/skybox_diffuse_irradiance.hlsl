#include "common.hlsli"

struct Vertex {
    float3 pos : POSITION;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
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
    output.pos.z = output.pos.z * 0.9999f;
    // Sample the texcube with just vertex position
    output.normal = normalize(input.pos);

    return output;
}

float4 PSMain(PSInput input) : SV_TARGET0
{
    TextureCube skybox = GetSkyboxTexture();

    float sampleDelta = 0.02;

    float sampleCount = 0;

    // Not entirely sure why this needs to be flipped...
    float3 N = -input.normal;

    float3 up = {0,1,0};
    float3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    float3x3 tangentSpace = {
        right,
        up,
        N
    };

    float3 irradiance = 0;
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            // float3 tangent = {sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta)};
            // float3 sampleDir = tangent.x * right + tangent.y * up + tangent.z * N;
            // float3 skySample = skybox.Sample(g_sampler, sampleDir).rgb;
            // irradiance +=  skySample * cos(theta) * sin(theta);

            float3 temp = cos(phi) * right + sin(phi) * up;
            float3 sampleDir = cos(theta) * N + sin(theta) * temp;
            irradiance += skybox.Sample(g_sampler, sampleDir).rgb * cos(theta) * sin(theta);

            sampleCount++;
        }
    }

    float3 radiance = PI * irradiance / sampleCount;

    return float4(radiance, 1.0f);
}