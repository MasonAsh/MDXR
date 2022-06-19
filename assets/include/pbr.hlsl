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

float4 ShadePBR(
    float3 colorIntensity,
    float3 baseColor,
    float3 N,
    float roughness,
    float metallic,
    float3 Wi,
    float3 Wo,
    float attenuation
)
{
    // halfway vector
    float3 H = normalize(Wi + Wo);

    float3 radiance = colorIntensity.xyz * attenuation;

    float3 F0 = 0.04;
    F0 = lerp(F0, baseColor, metallic);

    float cosWi = max(0.0, dot(N, Wi));
    float cosWh = max(0.0, dot(N, H));
    float cosWo = max(0.0, dot(N, Wo));

    float3 F = FSchlick(max(0.0, dot(H, Wo)), F0);
    float G = DistributionGGX(cosWh, roughness);
    float D = GeometrySmith(cosWi, cosWo, roughness);

    float3 kS = F;
    float3 kD = lerp(float3(1,1,1) - F, float3(0, 0, 0), metallic);
    // float3 kD = 1.0f - kS;
    // kD *= 1.0f - metallic;

    float3 numerator = F * D * G;
    //float denominator = 4.0f * max(dot(N, Wo), 0.0) * max(dot(N, Wi), 0.0) + 0.0001;
    float denominator = max(Epsilon, 4.0 * cosWi * cosWo);
    float3 specular = numerator / denominator;

    float3 diffuseBRDF = kD * baseColor;

    float3 Lo = (diffuseBRDF + specular) * radiance * cosWi;

    float4 color = float4(Lo, 1.0f);

    color = color / (color + 1.0f);
    color = pow(color, 1.0/2.2);

    return color;
}