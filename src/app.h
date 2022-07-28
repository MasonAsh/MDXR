#pragma once

#include "util.h"
#include "pso.h"
#include "pool.h"
#include "incrementalfence.h"
#include "commandqueue.h"
#include "descriptorpool.h"

#include <SDL.h>

#include <directx/d3dx12.h>
#include <D3D12MemAlloc.h>

#include <DXGItype.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <DXProgrammableCapture.h>

#include <DirectXMath.h>

#include <tiny_gltf.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <span>
#include <string>
#include <deque>

const UINT FrameBufferCount = 2;
const UINT MaxLightCount = 512;
const UINT MaxMaterialCount = 2048;
const UINT MaxDescriptors = 4096;
const DXGI_FORMAT DepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

enum ConstantIndex
{
    ConstantIndex_PrimitiveData,
    ConstantIndex_MaterialData,
    ConstantIndex_Light,
    ConstantIndex_LightPassData,
    ConstantIndex_MiscParameter,
    ConstantIndex_Count
};

enum CubeImageIndex
{
    CubeImage_Right,
    CubeImage_Left,
    CubeImage_Top,
    CubeImage_Bottom,
    CubeImage_Front,
    CubeImage_Back,
    CubeImage_Count, // The amount of faces in a cube may fluctuate in the future
};

struct SkyboxImagePaths
{
    std::array<std::string, CubeImage_Count> paths;
};

struct PerPrimitiveConstantData
{
    // MVP & MV are PerMesh, but most meshes only have one primitive.
    glm::mat4 MVP;
    glm::mat4 MV;
    glm::mat4 M;
    float padding[16];
};
static_assert((sizeof(PerPrimitiveConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

enum MaterialType
{
    MaterialType_Unlit,
    MaterialType_PBR,
    MaterialType_AlphaBlendPBR,
};

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

struct MaterialTextureDescriptors
{
    DescriptorRef baseColor;
    DescriptorRef normal;
    DescriptorRef metalRoughness;
};

struct Material
{
    MaterialConstantData* constantData;
    MaterialTextureDescriptors textureDescriptors;
    UniqueDescriptors cbvDescriptor;

    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    glm::vec4 metalRoughnessFactor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);

    bool castsShadow = true;
    bool receivesShadow = true;

    MaterialType materialType;

    std::string name;

    void UpdateConstantData()
    {
        // CPU only material.
        // For example: Skybox has a material to flag that it doesn't cast or receive shadows,
        // but isn't GPU visible and has no constant data.
        if (!constantData) {
            return;
        }

        constantData->baseColorTextureIdx = textureDescriptors.baseColor.index;
        constantData->normalTextureIdx = textureDescriptors.normal.index;
        constantData->metalRoughnessTextureIdx = textureDescriptors.metalRoughness.index;
        constantData->baseColorFactor = baseColorFactor;
        constantData->metalRoughnessFactor = metalRoughnessFactor;
    }
};

struct Primitive
{
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    std::vector<D3D12_VERTEX_BUFFER_VIEW> vertexBufferViews;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    D3D12_PRIMITIVE_TOPOLOGY primitiveTopology;
    ManagedPSORef PSO;
    UINT indexCount;
    UINT materialIndex;
    DescriptorRef perPrimitiveDescriptor;
    PerPrimitiveConstantData* constantData;

    // Optional custom descriptor for special primitive shaders. Can be anything.
    // For example, the skybox uses this for the texcube shader parameter.
    DescriptorRef miscDescriptorParameter;

    ManagedPSORef directionalShadowPSO;

    SharedPoolItem<Material> material = nullptr;
};

struct Mesh
{
    Mesh() = default;
    Mesh(Mesh&& mesh) = default;
    Mesh(const Mesh& mesh) = delete;

    // TODO: 
    // an array of pool items can be optimized to delete all it's items
    // in one batch. There should be a PoolItemArray structure with a similar
    // interface to std::vector
    std::vector<PoolItem<Primitive>> primitives;

    // This is the base transform as defined in the GLTF model
    glm::mat4 baseModelTransform;

    // These are offsets of baseModelTransform that can be applied live.
    glm::vec3 translation;
    glm::vec3 euler;
    glm::vec3 scale = glm::vec3(1.0f);

    std::string name;

    bool isReadyForRender = false;
};

struct Model
{
    Model() = default;
    Model(Model&& mesh) = default;
    Model(const Model& mesh) = delete;

    std::vector<ComPtr<ID3D12Resource>> buffers;
    std::vector<PoolItem<Mesh>> meshes;

    UniqueDescriptors primitiveDataDescriptors;
    UniqueDescriptors baseTextureDescriptor;
    DescriptorRef baseMaterialDescriptor;

    // All of the child mesh constant buffers stored in this constant buffer
    ComPtr<ID3D12Resource> perPrimitiveConstantBuffer;
    PerPrimitiveConstantData* perPrimitiveBufferPtr;
};

struct Camera
{
    glm::vec3 translation;

    float yaw;
    float pitch;

    float maxPitch = glm::radians(70.0f);

    float targetSpeed = 5.0f;
    float maxSpeed = 20.0f;
    float minSpeed = 0.5f;

    bool locked = true;

    float fovY = glm::pi<float>() * 0.2f;
};

struct MouseState
{
    int xrel, yrel;
    float scrollDelta;
};

struct ControllerState {
    glm::vec2 leftStick;
    glm::vec2 rightStick;

    // X is left trigger and Y is right trigger
    glm::vec2 triggerState;
};

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

    // NOTE: This might be better off in a separate constant buffer.
    // Point lights will potentially need 6 of these.
    glm::mat4 directionalLightViewProjection;

    float range;
    UINT directionalShadowMapDescriptorIdx;
    UINT lightType;

    float pad[25];
};
static_assert((sizeof(LightConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

// Store descriptor sizes globally for convenience
struct IncrementSizes
{
    int CbvSrvUav;
    int Rtv;
};
extern IncrementSizes G_IncrementSizes;

template<class T>
struct ConstantBufferSlice
{
    int index;
    std::span<T> data;
};

// Fixed size constant buffer allocator.
// Crashes if size > capacity
template<class T>
struct ConstantBufferArena
{
    static_assert((sizeof(T) % 256) == 0, "T must be 256-byte aligned");

    void InitializeWithCapacity(D3D12MA::Allocator* allocator, UINT count)
    {
        this->offset = 0;
        this->size = 0;
        this->capacity = count;
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(T) * count);
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        ASSERT_HRESULT(
            allocator->CreateResource(
                &allocDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                &allocation,
                IID_PPV_ARGS(&resource)
            )
        );
        resource->Map(0, nullptr, reinterpret_cast<void**>(&mappedPtr));
    }

    void InitializeWithBuffer(ComPtr<ID3D12Resource> resource, UINT64 offsetInBuffer)
    {
        this->resource = resource;
        this->capacity = static_cast<UINT>(resource->GetDesc().Width / sizeof(T));
        this->offset = offsetInBuffer;
        this->size = 0;
        resource->Map(0, nullptr, reinterpret_cast<void**>(&mappedPtr));
    }

    ConstantBufferSlice<T> Allocate(UINT count)
    {
        UINT index = size;
        size += count;
        if (size > capacity) {
            std::cerr << "Error: constant buffer is not large enough\n";
            abort();
        }

        T* start = mappedPtr + index;
        ConstantBufferSlice<T> result;
        result.data = std::span<T>(start, count);
        result.index = index;

        return result;
    }

    void CreateViews(ID3D12Device* device, ConstantBufferSlice<T>& slice, CD3DX12_CPU_DESCRIPTOR_HANDLE startDescriptor)
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE descriptorHandle(startDescriptor);
        for (int i = 0; i < slice.data.size(); i++) {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = resource->GetGPUVirtualAddress() + offset + ((slice.index + i) * sizeof(T));
            cbvDesc.SizeInBytes = sizeof(T);
            device->CreateConstantBufferView(
                &cbvDesc,
                descriptorHandle
            );
            descriptorHandle.Offset(1, G_IncrementSizes.CbvSrvUav);
        }
    }

    ComPtr<ID3D12Resource> resource;
    ComPtr<D3D12MA::Allocation> allocation;
    T* mappedPtr;
    UINT capacity = 0;
    UINT size = 0;
    UINT64 offset;
};

enum LightType
{
    LightType_Point,
    LightType_Directional,
};

struct Light
{
    LightConstantData* constantData;
    glm::vec3 position;
    glm::vec3 direction;

    glm::vec3 color;
    float intensity;
    float range;

    LightType lightType;

    ComPtr<D3D12MA::Allocation> directionalShadowMap;
    UniqueDescriptors directionalShadowMapSRV;
    UniqueDescriptors directionalShadowMapDSV;

    UINT directionalShadowMapSize = 4096;
    float frustumSize = 30.0f;

    void UpdateConstantData(glm::mat4 viewMatrix)
    {
        glm::vec3 normalizedDirection = glm::normalize(direction);
        constantData->position = glm::vec4(position, 1.0f);
        constantData->direction = glm::vec4(normalizedDirection, 0.0f);
        constantData->positionViewSpace = viewMatrix * constantData->position;
        constantData->directionViewSpace = viewMatrix * glm::normalize(constantData->direction);
        constantData->colorIntensity = glm::vec4(color * intensity, 1.0f);
        constantData->range = range;
        constantData->directionalShadowMapDescriptorIdx = directionalShadowMapSRV.Index();
        constantData->lightType = (UINT)lightType;

        if (lightType == LightType_Directional) {
            DirectX::XMMATRIX mat = DirectX::XMMatrixOrthographicRH(frustumSize, frustumSize, -frustumSize, frustumSize);
            //glm::mat4 directionalProjection = glm::transpose(glm::make_mat4(reinterpret_cast<float*>(&mat.r)));
            glm::mat4 directionalProjection = glm::ortho(-frustumSize, frustumSize, -frustumSize, frustumSize, 0.1f, frustumSize * 2.0f);
            //glm::mat4 directionalProjection = glm::perspective(0.628319f, 1.0f, 0.1f, 10.0f);
            glm::vec3 eye = -normalizedDirection * frustumSize * 1.2f;

            glm::vec3 up(0.0f, 1.0f, 0.0f);
            if (abs(glm::dot(up, normalizedDirection)) > 0.99f) {
                // If the light is pointing straight up or down, we can't use the normal up vector
                up = glm::vec3(1.0f, 0.0f, 0.0f);
            }

            glm::mat4 directionalView = glm::lookAt(eye, glm::vec3(0, 0, 0), up);
            constantData->directionalLightViewProjection = directionalProjection * directionalView;
        }
    }
};

struct AssetLoadProgress
{
    std::string assetName;
    std::string currentTask;
    float overallPercent;

    std::atomic<bool> isFinished{ false };
};

enum RenderThreadType
{
    RenderThread_GBufferPass,
    RenderThread_ShadowPass,
    RenderThread_LightPass,
    RenderThread_AlphaBlendPass,
    RenderThread_Count,
};

struct RenderThread
{
    std::thread thread;

    std::mutex mutex;

    bool workIsAvailable = false;
    std::condition_variable beginWork;
    std::condition_variable workFinished;

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12CommandAllocator> commandAllocator;
};

struct App
{
    // NOTE: DESTRUCTOR ORDER IS IMPORTANT ON THESE. LEAVE AT TOP.
    DescriptorPool descriptorPool;
    DescriptorPool rtvDescriptorPool;
    DescriptorPool dsvDescriptorPool;

    std::string dataDir;
    std::wstring wDataDir;

    SDL_Window* window;
    HWND hwnd;

    HANDLE shaderWatchHandle;

    bool running;
    long long startTick;
    long long lastFrameTick;

    struct {
        long long lastFrameTimeNS = 0;
        long triangleCount = 0;
        std::atomic_uint drawCalls = 0;
    } Stats;

    int windowWidth = 1920;
    int windowHeight = 1080;
    bool borderlessFullscreen = false;
    bool gpuDebug = false;

    PSOManager psoManager;

    Pool<Primitive, 100> primitivePool;
    Pool<Mesh, 32> meshPool;
    Pool<Material, 128> materials;
    ConstantBufferArena<MaterialConstantData> materialConstantBuffer;

    ComPtr<ID3D12Device2> device;
    CommandQueue graphicsQueue;
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12Resource> renderTargets[FrameBufferCount];
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    CD3DX12_VIEWPORT viewport;
    CD3DX12_RECT scissorRect;

    std::array<RenderThread, RenderThread_Count> renderThreads;

    CommandQueue computeQueue;
    ComPtr<ID3D12CommandAllocator> computeCommandAllocator;

    ComPtr<ID3D12Resource> depthStencilBuffer;
    UniqueDescriptors depthStencilDescriptor;

    tinygltf::TinyGLTF loader;

    CommandQueue copyQueue;
    ComPtr<ID3D12CommandAllocator> copyCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> copyCommandList;

    std::vector<Model> models;

    unsigned int frameIdx;

    DescriptorRef frameBufferRTVs[FrameBufferCount];
    DescriptorRef nonSRGBFrameBufferRTVs[FrameBufferCount];

    Camera camera;
    const UINT8* keyState;
    MouseState mouseState;
    ControllerState controllerState;
    SDL_GameController* controller;

    struct
    {
        DescriptorPool srvHeap;
        bool lightsOpen = true;
        bool meshesOpen = true;
        bool materialsOpen = false;
        bool geekOpen = true;
        bool demoOpen = false;
        bool showStats = true;

        UniqueDescriptors fontSRV;
        UniqueDescriptors debugSRV;
    } ImGui;


    ComPtr<D3D12MA::Allocator> mainAllocator;

    struct
    {
        std::array<ComPtr<ID3D12Resource>, GBuffer_Count - 1> renderTargets;
        UniqueDescriptors baseSrvReference;
        DescriptorRef rtvs[GBuffer_RTVCount];
    } GBuffer;

    struct
    {
        ManagedPSORef pointLightPSO;
        ManagedPSORef directionalLightPso;
        ManagedPSORef environentCubemapLightPso;
    } LightPass;

    struct
    {
        ComPtr<ID3D12Resource> constantBuffer;

        // These two are stored in the same constant buffer.
        // The lights are stored at offset (LightPassConstantData)
        LightPassConstantData* passData;
        LightConstantData* lightConstantData;

        UniqueDescriptors cbvHandle;

        UINT count;
    } LightBuffer;

    std::array<Light, MaxLightCount> lights;

    struct {
        ManagedPSORef toneMapPSO;
        float gamma = 2.2f;
        float exposure = 1.0f;
    } PostProcessPass;

    struct
    {
        ComPtr<D3D12MA::Allocation> cubemap;
        ComPtr<D3D12MA::Allocation> vertexBuffer;
        ComPtr<D3D12MA::Allocation> indexBuffer;
        ComPtr<D3D12MA::Allocation> perPrimitiveConstantBuffer;
        ComPtr<D3D12MA::Allocation> irradianceCubeMap;
        ComPtr<D3D12MA::Allocation> prefilterMap;
        UniqueDescriptors perPrimitiveCBV;
        UniqueDescriptors texcubeSRV;
        UniqueDescriptors irradianceCubeSRV;
        UniqueDescriptors prefilterMapSRV;
        PoolItem<Mesh> mesh;

        // LUT texture for environment BRDF split sum calculation.
        ComPtr<D3D12MA::Allocation> brdfLUT;
        UniqueDescriptors brdfLUTDescriptor;

        std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    } Skybox;

    struct
    {
        ComPtr<ID3D12RootSignature> rootSignature;
        ManagedPSORef PSO;
    } MipMapGenerator;

    ComPtr<IDXGraphicsAnalysis> graphicsAnalysis;

    struct
    {
        std::deque<std::string> gltfFilesToLoad;
        std::optional<SkyboxImagePaths> skyboxToLoad;

        std::vector<std::unique_ptr<AssetLoadProgress>> assetLoadInfo;

        std::mutex mutex;
        std::thread thread;
        std::condition_variable workEvent;
    } AssetThread;
};