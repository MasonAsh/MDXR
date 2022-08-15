#pragma once

#include "util.h"
#include "pso.h"
#include "pool.h"
#include "incrementalfence.h"
#include "commandqueue.h"
#include "descriptorpool.h"
#include "constantbufferstructures.h"

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
#include <variant>

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

enum MaterialType
{
    MaterialType_Unlit,
    MaterialType_PBR,
    MaterialType_AlphaBlendPBR,
};

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

struct AABB
{
    glm::vec3 min;
    glm::vec3 max;
};

struct Primitive
{
    // FIXME: Lot of duplicated data
    std::vector<D3D12_VERTEX_BUFFER_VIEW> vertexBufferViews;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    D3D12_PRIMITIVE_TOPOLOGY primitiveTopology;
    ManagedPSORef PSO;
    UINT indexCount;
    UINT materialIndex;
    DescriptorRef perPrimitiveDescriptor;
    PrimitiveInstanceConstantData* constantData;
    int instanceCount;

    // Optional custom descriptor for special primitive shaders. Can be anything.
    // For example, the skybox uses this for the texcube shader parameter.
    DescriptorRef miscDescriptorParameter;

    ManagedPSORef directionalShadowPSO;

    SharedPoolItem<Material> material = nullptr;

    AABB localBoundingBox;
    bool cull;
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

    std::vector<ComPtr<ID3D12Resource>> resources;
    std::vector<PoolItem<Mesh>> meshes;

    UniqueDescriptors primitiveDataDescriptors;
    UniqueDescriptors baseTextureDescriptor;
    DescriptorRef baseMaterialDescriptor;

    // All of the child mesh constant buffers stored in this constant buffer
    ComPtr<ID3D12Resource> perPrimitiveConstantBuffer;
    PrimitiveInstanceConstantData* perPrimitiveBufferPtr;
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

template<class T, size_t BlockSize>
class ConstantBufferVec
{
    static_assert((sizeof(T) % 256) == 0, "T must be 256-byte aligned");
public:
    ConstantBufferVec()
    {
        pool.SetBlockDataAllocator(
            std::bind(&ConstantBufferVec::CreateBlockData, this, std::placeholders::_1)
        );
    }

    void Initialize(D3D12MA::Allocator* allocator)
    {
        this->allocator = allocator;
    }

    PoolItem<T> AllocateUnique()
    {
        return pool.AllocateUnique();
    }

    SharedPoolItem<T> AllocateShared()
    {
        return pool.AllocateShared();
    }
private:
    struct CBVBlockData
    {
        ComPtr<D3D12MA::Allocation> allocation;
        void* mappedPtr;
    };

    void CreateBlockData(CBVBlockData* blockData)
    {
        blockData->allocation = CreateUploadBufferWithData(
            allocator,
            nullptr,
            0,
            sizeof(T) * BlockSize,
            &blockData->mappedPtr
        );
    }

    Pool<T, BlockSize, CBVBlockData> pool;
    D3D12MA::Allocator* allocator;
};

enum LightType
{
    LightType_Point,
    LightType_Directional,
};

struct App;

typedef std::function<void(App& app, Model& model)> ModelFinishCallback;

struct GLTFLoadEntry
{
    std::string assetPath;
    ModelFinishCallback finishCB;
};

struct AssetLoadContext
{
    std::string assetPath;
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

    PoolItem<PrimitiveInstanceConstantData> sphereInstanceData;

    float radianceThreshold = 0.001;

    // Point lights follow the inverse square law and don't have a radius.
    // But for optimization purposes we need to limit the range of the light.
    // So we compute the effective radius using radianceThreshold^
    //
    // float radiance = attenuation * colorIntensity
    // radiance = (1.0f / (distance * distance)) * colorIntensity
    // let effectiveRadius = distance
    // let radianceThreshold = radiance
    // radianceThreshold = (1.0f / (effectiveRadius * effectiveRadius))
    // effectiveRadius = sqrt(1.0f / (radianceThreshold * colorIntensity)) 
    float effectiveRadius;

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

        if (lightType == LightType_Point) {
            float CI = glm::length(color * intensity);
            effectiveRadius = sqrtf(1.0f / (radianceThreshold * CI));
        }

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
            constantData->MVP = directionalProjection * directionalView;
        }
    }
};

enum NodeType
{
    NodeType_Mesh,
    NodeType_Light,
};

struct Node
{
    NodeType nodeType;
    union
    {
        Mesh* mesh;
        Light* light;
    };
};

struct Scene
{
    std::vector<Node> nodes;
};

typedef Pool<Primitive, 100> PrimitivePool;
typedef Pool<Mesh, 32> MeshPool;

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

    PrimitivePool primitivePool;
    MeshPool meshPool;
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

    Scene scene;
    std::vector<Model> models;

    unsigned int frameIdx;

    FenceEvent previousFrameEvent;

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

        // Mesh* pointLightSphere;
        // ConstantBufferVec<PrimitiveInstanceConstantData, MaxLightCount> pointSphereConstantData;

        UINT count;
    } LightBuffer;

    std::array<Light, MaxLightCount> lights;

    struct
    {
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
        std::deque<GLTFLoadEntry> gltfLoadEntries;
        std::optional<SkyboxImagePaths> skyboxToLoad;

        std::vector<std::unique_ptr<AssetLoadContext>> assetLoadInfo;

        std::mutex mutex;
        std::thread thread;
        std::condition_variable workEvent;
    } AssetThread;
};