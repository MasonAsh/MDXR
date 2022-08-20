#include <glm/glm.hpp>

struct PrimitiveInstanceConstantData
{
    // MVP & MV are PerMesh, but most meshes only have one primitive.
    glm::mat4 MVP;
    glm::mat4 MV;
    glm::mat4 M;
    float padding[16];
};
static_assert((sizeof(PrimitiveInstanceConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct MaterialConstantData
{
    glm::vec4 baseColorFactor;
    glm::vec4 metalRoughnessFactor;

    UINT baseColorTextureIdx;
    UINT normalTextureIdx;
    UINT metalRoughnessTextureIdx;

    UINT materialType;

    float padding[52];
};
static_assert((sizeof(MaterialConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct LightPassConstantData
{
    glm::mat4 inverseProjectionMatrix;
    glm::mat4 inverseViewMatrix;
    glm::vec4 environmentIntensity;
    UINT baseGBufferIndex;
    UINT debug;
    float pad[26];
};
static_assert((sizeof(LightPassConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct LightConstantData
{
    glm::vec4 position;
    glm::vec4 direction;

    glm::vec4 positionViewSpace;
    glm::vec4 directionViewSpace;

    glm::vec4 colorIntensity;

    // MVP used for rendering.
    // For spot lights, this is the spot lights point of view.
    // For point lights, this transforms the sphere into the world.
    glm::mat4 MVP;

    float range;
    UINT shadowMapDescriptorIdx;
    UINT lightType;

    UINT castsShadow;

    float pad[24];
};
static_assert((sizeof(LightConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct RayTraceInfoConstantData
{
    glm::vec3 camPosWorld;
    glm::mat4 projectionToWorld;
    float tMin;
    float tMax;

    float pad[43];
};
static_assert((sizeof(RayTraceInfoConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");