float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float numerator = a2;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = PI * denom * denom;

    return numerator / denom;
}

float GSchlickGGX(float cosTheta, float k)
{
    float denom = cosTheta * (1.0f - k) + k;
    
    return cosTheta / denom;
}

float GeometrySmith(float cosWi, float cosWo, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return GSchlickGGX(cosWi, k) * GSchlickGGX(cosWo, k);
}

float3 FSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float3 FSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(make_float3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float4 ShadePBR(
    float3 colorIntensity,
    float4 baseColor,
    float3 N,
    float roughness,
    float metallic,
    float3 Wi,
    float3 Wo,
    float attenuation
)
{
    // 0 roughness means no specular at all, so make sure we have at least a tiny amount
    roughness = max(0.01, roughness);

    // halfway vector
    float3 H = normalize(Wi + Wo);

    float3 radiance = colorIntensity.xyz * attenuation;

    float3 F0 = 0.04;
    F0 = lerp(F0, baseColor.rgb, metallic);

    float cosWi = max(0.0, dot(N, Wi));
    float cosWh = max(0.0, dot(N, H));
    float cosWo = max(0.0, dot(N, Wo));

    float3 F = FSchlick(max(0.0, dot(H, Wo)), F0);
    float G = DistributionGGX(cosWh, roughness);
    float D = GeometrySmith(cosWi, cosWo, roughness);

    float3 kD = lerp(float3(1,1,1) - F, float3(0, 0, 0), metallic);

    float3 numerator = F * D * G;
    float denominator = max(Epsilon, 4.0 * cosWi * cosWo);
    float3 specular = numerator / denominator;

    float4 diffuseBRDF = float4(kD, 1.0f) * baseColor * baseColor.a;

    float4 Lo = (diffuseBRDF + float4(specular, 1.0f)) * float4(radiance, 1) * cosWi;

    float4 color = float4(Lo);

    color = color / (color + 1.0f);
    color = pow(color, 1.0/2.2);

    return color;
}