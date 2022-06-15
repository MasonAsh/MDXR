struct LightConstantData {
    float4 position;
    float4 direction;

    float4 positionViewSpace;
    float4 directionViewSpace;

    float4 color;

    float range;
    float intensity;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

struct LightingPassConstantData {
    float4x4 inverseProjection;
    uint debug;
};

const uint g_lightIndex : register(b0);
ConstantBuffer<LightingPassConstantData> g_passData : register(b1);
ConstantBuffer<LightConstantData> g_lights[] : register(b2);

#define DIFFUSE_BUFFER 0
#define NORMAL_BUFFER 1
#define DEPTH_BUFFER 2

Texture2D g_inputTextures[] : register(t0);
SamplerState g_samp : register(s0);

float4 ClipToView(float4 clipCoord)
{
    float4 view = mul(g_passData.inverseProjection, clipCoord);
    view = view / view.w;
    return view;
}

float4 ScreenToView(float4 screen)
{
    float2 texcoord = screen.xy;
    float4 clip = float4(float2(texcoord.x, 1.0f - texcoord.y) * 2.0f - 1.0f, screen.z, screen.w);
    return ClipToView(clip);
}

PSInput VSMain(uint id : SV_VertexID)
{
    PSInput result;
    result.uv = float2(id % 2, (id % 4) >> 1);
    result.pos = float4((result.uv.x - 0.5f) * 2, -(result.uv.y - 0.5f) * 2, 0, 1);
    return result;
}

// P = position of point being shaded in view space
// N = normal of point being shaded in view space

[earlydepthstencil]
float4 PSMain(PSInput input) : SV_TARGET
{
    LightConstantData light = g_lights[g_lightIndex];

    float depth = g_inputTextures[DEPTH_BUFFER].Sample(g_samp, input.uv).r;
    float4 normal = g_inputTextures[NORMAL_BUFFER].Sample(g_samp, input.uv);

    float4 P = ScreenToView(float4(input.uv, depth, 1.0f));
    float4 N = float4(normal.xyz, 1.0f);

    float4 L = light.positionViewSpace - P;
    float distance = length(L);
    L = L / distance;

    float attenuation = 1.0f - smoothstep(light.range * 0.75f, light.range, distance);

    float4 diffuseColor = g_inputTextures[DIFFUSE_BUFFER].Sample(g_samp, input.uv);
    float4 diffuseFactor = light.color * max(dot(N, L), 0);
    float4 diffuse = diffuseFactor * attenuation * light.intensity * diffuseColor;

    if (g_passData.debug) {
        float2 texcoord = input.uv;
        float4 clip = float4(float2(texcoord.x, 1.0f - texcoord.y) * 2.0f - 1.0f, depth, 1.0f);
        //return clip;
        return ClipToView(clip);
    } else {
        return diffuse;
    }
}