#pragma once

#include "util.h"
#include "pso.h"
#include "pool.h"
#include "incrementalfence.h"

#include <SDL.h>

#include <directx/d3dx12.h>
#include <D3D12MemAlloc.h>

#include <DXGItype.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <DXProgrammableCapture.h>

#include <tiny_gltf.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <span>
#include <string>
#include <deque>

const UINT FrameBufferCount = 2;
const UINT MaxLightCount = 64;
const UINT MaxMaterialCount = 2048;
const UINT MaxDescriptors = 4096;
const DXGI_FORMAT DepthFormat = DXGI_FORMAT_D32_FLOAT;

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
    float padding[32];
};
static_assert((sizeof(PerPrimitiveConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct alignas(16) GenerateMipsConstantData
{
    UINT srcMipLevel;
    UINT numMipLevels;
    UINT srcDimension;
    UINT isSRGB;
    glm::vec2 texelSize;
    float padding[58];
};
static_assert((sizeof(GenerateMipsConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct DescriptorRef
{
    ID3D12DescriptorHeap* heap;
    UINT incrementSize;
    UINT index;

    DescriptorRef()
        : heap(nullptr)
        , incrementSize(0)
        , index(-1)
    {
    }

    DescriptorRef(ID3D12DescriptorHeap* heap, UINT index, int incrementSize)
        : heap(heap)
        , index(index)
        , incrementSize(incrementSize)
    {
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE CPUHandle(int offset = 0) const {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(), index + offset, incrementSize);
    }

    CD3DX12_GPU_DESCRIPTOR_HANDLE GPUHandle() const {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), index, incrementSize);
    }

    DescriptorRef operator+(int offset) const
    {
        return DescriptorRef(heap, index + offset, incrementSize);
    }
};

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
    DescriptorRef cbvDescriptor;

    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    glm::vec4 metalRoughnessFactor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);

    MaterialType materialType;

    std::string name;

    void UpdateConstantData()
    {
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

    DescriptorRef primitiveDataDescriptors;
    DescriptorRef baseTextureDescriptor;
    DescriptorRef baseMaterialDescriptor;

    // All of the child mesh constant buffers stored in this constant buffer
    ComPtr<ID3D12Resource> perPrimitiveConstantBuffer;
    PerPrimitiveConstantData* perPrimitiveBufferPtr;
};

// Fixed capacity descriptor heap. Aborts when the size outgrows initial capacity.
//
// There are two types of descriptors that can be allocated with DescriptorArena:
//  * Static descriptors for the lifetime of the program with AllocateDesciptors
//  * Temporary descriptors pushed onto a stack with [Push|Pop]DescriptorStack
// 
// Static descriptors grow from the left side of the heap while the stack grows
// from the right.
struct DescriptorArena
{
    // Left side of descriptor heap is permanent descriptors
    // Right side is a stack for temporary allocations
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    UINT capacity;
    UINT size;
    UINT descriptorIncrementSize;
    std::string debugName;
    DescriptorRef stackPtr;
    std::mutex mutex;

    // Temporary descriptors can be allocated from the right side of the heap
    UINT stack;

    ID3D12DescriptorHeap* Heap()
    {
        return descriptorHeap.Get();
    }

    void Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_DESC heapDesc, const std::string& debugName)
    {
        this->debugName = debugName;
        descriptorIncrementSize = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        ASSERT_HRESULT(
            device->CreateDescriptorHeap(
                &heapDesc,
                IID_PPV_ARGS(&descriptorHeap)
            )
        );
        capacity = heapDesc.NumDescriptors;
        size = 0;
        stack = 0;

        stackPtr = DescriptorRef(descriptorHeap.Get(), capacity - 1, descriptorIncrementSize);
    }

    DescriptorRef AllocateDescriptors(UINT count, const char* debugName)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (debugName != nullptr) {
            DebugLog() << this->debugName << " allocation info: " <<
                "\n\tIndex: " << this->size <<
                "\n\tCount: " << count <<
                "\n\tReason: " << debugName << "\n";
        }
        DescriptorRef reference(descriptorHeap.Get(), size, descriptorIncrementSize);
        size += count;
        if (size + stack > capacity) {
            DebugLog() << "Error: descriptor heap is not large enough\n";
            abort();
        }
        return reference;
    }

    // Allocate some temporary descriptors.
    // These descriptors are allocated from the opposite side of the heap.
    DescriptorRef PushDescriptorStack(UINT count)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (size + (capacity - stackPtr.index) + count > capacity) {
            std::cerr << "Error: descriptor heap is not large enough\n";
            abort();
        }

        stackPtr.index -= count;

        return stackPtr;
    }

    // Pops temporary descriptors from stack
    void PopDescriptorStack(UINT count)
    {
        std::lock_guard<std::mutex> lock(mutex);

        stackPtr.index += count;
    }
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

    float range;

    UINT lightType;

    float pad[42];
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

    void UpdateConstantData(glm::mat4 viewMatrix)
    {
        constantData->position = glm::vec4(position, 1.0f);
        constantData->direction = glm::vec4(direction, 0.0f);
        constantData->positionViewSpace = viewMatrix * constantData->position;
        constantData->directionViewSpace = viewMatrix * glm::normalize(constantData->direction);
        constantData->colorIntensity = glm::vec4(color * intensity, 1.0f);
        constantData->range = range;
        constantData->lightType = (UINT)lightType;
    }
};

struct AssetLoadProgress {
    std::string assetName;
    std::string currentTask;
    float overallPercent;

    std::atomic<bool> isFinished{ false };
};

struct App
{
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
        long drawCalls = 0;
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

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    std::mutex commandQueueMutex;
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12Resource> renderTargets[FrameBufferCount];
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    CD3DX12_VIEWPORT viewport;
    CD3DX12_RECT scissorRect;

    DescriptorArena rtvDescriptorArena;

    ComPtr<ID3D12CommandQueue> computeQueue;
    ComPtr<ID3D12CommandAllocator> computeCommandAllocator;
    IncrementalFence computeFence;

    ComPtr<ID3D12Resource> depthStencilBuffer;
    ComPtr<ID3D12DescriptorHeap> dsHeap;

    IncrementalFence fence;

    tinygltf::TinyGLTF loader;

    ComPtr<ID3D12CommandQueue> copyCommandQueue;
    ComPtr<ID3D12CommandAllocator> copyCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> copyCommandList;
    IncrementalFence copyFence;

    std::vector<Model> models;

    unsigned int frameIdx;

    DescriptorRef frameBufferRTVs[FrameBufferCount];

    Camera camera;
    const UINT8* keyState;
    MouseState mouseState;

    struct
    {
        ComPtr<ID3D12DescriptorHeap> srvHeap;
        bool lightsOpen = true;
        bool meshesOpen = true;
        bool materialsOpen = false;
        bool geekOpen = true;
        bool demoOpen = false;
        bool showStats = true;
    } ImGui;

    DescriptorArena descriptorArena;

    ComPtr<D3D12MA::Allocator> mainAllocator;

    struct
    {
        std::array<ComPtr<ID3D12Resource>, GBuffer_Count - 1> renderTargets;
        DescriptorRef baseSrvReference;
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

        DescriptorRef cbvHandle;

        UINT count;
    } LightBuffer;

    std::array<Light, MaxLightCount> lights;

    // Per primitive constant buffer for cubemap rendering
    // Contains the view projection matrices for all CubeImage_*
    ComPtr<D3D12MA::Allocation> cubemapPerPrimitiveBuffer;
    DescriptorRef cubemapPerPrimitiveBufferDescriptor;

    struct
    {
        ComPtr<D3D12MA::Allocation> cubemap;
        ComPtr<D3D12MA::Allocation> vertexBuffer;
        ComPtr<D3D12MA::Allocation> indexBuffer;
        ComPtr<D3D12MA::Allocation> perPrimitiveConstantBuffer;
        ComPtr<D3D12MA::Allocation> irradianceCubeMap;
        DescriptorRef texcubeSRV;
        DescriptorRef irradianceCubeSRV;
        PoolItem<Mesh> mesh;

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