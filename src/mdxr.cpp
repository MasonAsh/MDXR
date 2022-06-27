#include "mdxr.h"
#include "util.h"
#include "pool.h"
#include "incrementalfence.h"
#include "uploadbatch.h"
#include <algorithm>
#include <iostream>
#include <assert.h>
#include <d3dcompiler.h>
#include <locale>
#include <codecvt>
#include <string>
#include <mutex>
#include "tiny_gltf.h"
#include "glm/gtc/matrix_transform.hpp"
#include "dxgidebug.h"
#include "stb_image.h"
#include <chrono>
#include <span>
#include <ranges>
#include <fstream>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_sdl.h"
#include "tinyfiledialogs.h"
#include "D3D12MemAlloc.h"
#include "GFSDK_Aftermath_GpuCrashDump.h"
#include <dxgitype.h>
#include <DXProgrammableCapture.h>

using namespace std::chrono;

const int FrameBufferCount = 2;
const int MaxLightCount = 64;
const int MaxMaterialCount = 2048;
const int MaxDescriptors = 4096;
const DXGI_FORMAT DepthFormat = DXGI_FORMAT_D32_FLOAT;

enum ConstantIndex {
    ConstantIndex_PrimitiveData,
    ConstantIndex_MaterialData,
    ConstantIndex_Light,
    ConstantIndex_LightPassData,
    ConstantIndex_MiscParameter,
    ConstantIndex_Count
};

struct PerPrimitiveConstantData {
    // MVP & MV are PerMesh, but most meshes only have one primitive.
    glm::mat4 MVP;
    glm::mat4 MV;
    float padding[32];
};
static_assert((sizeof(PerPrimitiveConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct alignas(16) GenerateMipsConstantData {
    UINT srcMipLevel;
    UINT numMipLevels;
    UINT srcDimension;
    UINT isSRGB;
    glm::vec2 texelSize;
    float padding[58];
};
static_assert((sizeof(GenerateMipsConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct PSOGraphicsShaderPaths {
    std::wstring vertex;
    std::wstring pixel;
};

class IManagedPSO {
public:
    virtual ID3D12PipelineState* Get() = 0;
    virtual void reload(ID3D12Device* device) = 0;
};

struct ManagedGraphicsPSO : public IManagedPSO {
    ComPtr<ID3D12PipelineState> PSO;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    PSOGraphicsShaderPaths shaderPaths;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;

    ID3D12PipelineState* Get() override
    {
        return PSO.Get();
    }

    // FIXME: This leaks. Shouldn't be a big deal since this is just for debugging, but something to keep in mind.
    void reload(ID3D12Device* device) override
    {
        ComPtr<ID3DBlob> vertexShader = nullptr;
        ComPtr<ID3DBlob> pixelShader = nullptr;

        if (!shaderPaths.vertex.empty()) {
            if (!SUCCEEDED(D3DReadFileToBlob(shaderPaths.vertex.c_str(), &vertexShader))) {
                std::wcout << "Failed to read vertex shader " << shaderPaths.vertex << "\n";
                return;
            }
            desc.VS = { reinterpret_cast<UINT8*>(vertexShader->GetBufferPointer()), vertexShader->GetBufferSize() };
        }
        if (!shaderPaths.pixel.empty()) {
            if (!SUCCEEDED(D3DReadFileToBlob(shaderPaths.pixel.c_str(), &pixelShader))) {
                std::wcout << "Failed to read pixel shader " << shaderPaths.pixel << "\n";
                return;
            }
            desc.PS = { reinterpret_cast<UINT8*>(pixelShader->GetBufferPointer()), pixelShader->GetBufferSize() };
        }

        ComPtr<ID3D12PipelineState> NewPSO;
        if (!SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&NewPSO)))) {
            std::wcout << L"Error: PSO reload failed for PSO with\n"
                << L"Vertex shader: " << shaderPaths.vertex << L"\n"
                << L"Pixel shader: " << shaderPaths.pixel << L"\n";
            return;
        }
        PSO = NewPSO;
    }
};

struct ManagedComputePSO : public IManagedPSO {
    ComPtr<ID3D12PipelineState> PSO;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
    std::wstring computeShaderPath;

    ID3D12PipelineState* Get() override
    {
        return PSO.Get();
    }

    void reload(ID3D12Device* device) override
    {
        ComPtr<ID3DBlob> computeShader = nullptr;

        if (!SUCCEEDED(D3DReadFileToBlob(computeShaderPath.c_str(), &computeShader))) {
            std::wcout << "Failed to read compute shader " << computeShaderPath << "\n";
            return;
        }
        desc.CS = { reinterpret_cast<UINT8*>(computeShader->GetBufferPointer()), computeShader->GetBufferSize() };

        ComPtr<ID3D12PipelineState> NewPSO;
        if (!SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&NewPSO)))) {
            std::wcout << L"Error: PSO reload failed for PSO with\n"
                << L"Compute shader: " << computeShaderPath << L"\n";
            return;
        }
        PSO = NewPSO;
    }
};

typedef std::shared_ptr<IManagedPSO> ManagedPSORef;

struct PSOManager {
    std::vector<std::weak_ptr<IManagedPSO>> PSOs;

    void reload(ID3D12Device* device)
    {
        for (auto& PSO : PSOs) {
            if (std::shared_ptr<IManagedPSO> pso = PSO.lock()) {
                pso->reload(device);
            }
        }
    }
};

struct DescriptorRef {
    ID3D12DescriptorHeap* heap;
    int incrementSize;
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

    CD3DX12_CPU_DESCRIPTOR_HANDLE CPUHandle(size_t offset = 0) const {
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


enum MaterialType {
    MaterialType_Unlit,
    MaterialType_PBR,
    MaterialType_AlphaBlendPBR,
};

struct MaterialConstantData {
    glm::vec4 baseColorFactor;
    glm::vec4 metalRoughnessFactor;

    UINT baseColorTextureIdx;
    UINT normalTextureIdx;
    UINT metalRoughnessTextureIdx;

    UINT materialType;

    float padding[52];
};
static_assert((sizeof(MaterialConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct MaterialTextureDescriptors {
    DescriptorRef baseColor;
    DescriptorRef normal;
    DescriptorRef metalRoughness;
};

struct Material {
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

struct Primitive {
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

struct Mesh {
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
};

struct Model {
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

enum SkyboxImageIndex {
    SkyboxImage_Right,
    SkyboxImage_Left,
    SkyboxImage_Top,
    SkyboxImage_Bottom,
    SkyboxImage_Front,
    SkyboxImage_Back,
    SkyboxImage_Count, // The amount of faces in a cube may fluctuate in the future
};

struct SkyboxAssets {
    std::array<tinygltf::Image, SkyboxImage_Count> images;
};

struct AssetBundle {
    std::vector<tinygltf::Model> models;
    std::optional<SkyboxAssets> skybox;
};

// Fixed capacity descriptor heap. Aborts when the size outgrows initial capacity.
//
// There are two types of descriptors that can be allocated with DescriptorArena:
//  * Static descriptors for the lifetime of the program with AllocateDesciptors
//  * Temporary descriptors pushed onto a stack with [Push|Pop]DescriptorStack
// 
// Static descriptors grow from the left side of the heap while the stack grows
// from the right.
struct DescriptorArena {
    // Left side of descriptor heap is permanent descriptors
    // Right side is a stack for temporary allocations
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    size_t capacity;
    size_t size;
    UINT descriptorIncrementSize;
    std::string debugName;
    DescriptorRef stackPtr;

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
        if (debugName != nullptr) {
            std::cout << this->debugName << " allocation info: " <<
                "\n\tIndex: " << this->size <<
                "\n\tCount: " << count <<
                "\n\tReason: " << debugName << "\n";
        }
        DescriptorRef reference(descriptorHeap.Get(), size, descriptorIncrementSize);
        size += count;
        if (size + stack > capacity) {
            std::cerr << "Error: descriptor heap is not large enough\n";
            abort();
        }
        return reference;
    }

    // Allocate some temporary descriptors.
    // These descriptors are allocated from the opposite side of the heap.
    DescriptorRef PushDescriptorStack(UINT count)
    {
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
        stackPtr.index += count;
    }
};

struct Camera {
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

struct MouseState {
    int xrel, yrel;
    float scrollDelta;
};


enum GBufferTarget {
    GBuffer_BaseColor,
    GBuffer_Normal,
    GBuffer_MetalRoughness,
    GBuffer_Depth,
    GBuffer_Count,
};

const int GBuffer_RTVCount = GBuffer_Depth;

struct LightPassConstantData {
    glm::mat4 inverseProjectionMatrix;
    glm::mat4 inverseViewMatrix;
    glm::vec4 environmentIntensity;
    UINT baseGBufferIndex;
    UINT debug;
    float pad[26];
};
static_assert((sizeof(LightPassConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct LightConstantData {
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
struct {
    int CbvSrvUav;
    int Rtv;
} G_IncrementSizes;

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
                D3D12_RESOURCE_STATE_COMMON,
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
        this->capacity = resource->GetDesc().Width / sizeof(T);
        this->offset = offsetInBuffer;
        this->size = 0;
        resource->Map(0, nullptr, reinterpret_cast<void**>(&mappedPtr));
    }

    ConstantBufferSlice<T> Allocate(int count)
    {
        int index = size;
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
    int capacity = 0;
    int size = 0;
    UINT64 offset;
};

enum LightType {
    LightType_Point,
    LightType_Directional,
};

struct Light {
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

struct App {
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

    struct {
        ComPtr<ID3D12DescriptorHeap> srvHeap;
        bool lightsOpen = true;
        bool meshesOpen = true;
        bool materialsOpen = false;
        bool geekOpen = true;
        bool showStats = true;
    } ImGui;

    DescriptorArena descriptorArena;

    ComPtr<D3D12MA::Allocator> mainAllocator;

    struct {
        std::array<ComPtr<ID3D12Resource>, GBuffer_Count - 1> renderTargets;
        DescriptorRef baseSrvReference;
        DescriptorRef rtvs[GBuffer_RTVCount];
    } GBuffer;

    struct {
        ManagedPSORef pointLightPSO;
        ManagedPSORef directionalLightPso;
        ManagedPSORef environentCubemapLightPso;
    } LightPass;

    struct {
        ComPtr<ID3D12Resource> constantBuffer;

        // These two are stored in the same constant buffer.
        // The lights are stored at offset (LightPassConstantData)
        LightPassConstantData* passData;
        LightConstantData* lightConstantData;

        DescriptorRef cbvHandle;

        int count;
    } LightBuffer;

    std::array<Light, MaxLightCount> lights;

    struct {
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

    struct {
        ComPtr<ID3D12RootSignature> rootSignature;
        ManagedPSORef PSO;
    } MipMapGenerator;

    ComPtr<IDXGraphicsAnalysis> graphicsAnalysis;
};

void SetupDepthStencil(App& app, bool isResize)
{
    if (!isResize) {
        D3D12_DESCRIPTOR_HEAP_DESC dsHeapDesc = {};
        dsHeapDesc.NumDescriptors = 1;
        dsHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ASSERT_HRESULT(app.device->CreateDescriptorHeap(&dsHeapDesc, IID_PPV_ARGS(&app.dsHeap)));
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsDesc = {};
    dsDesc.Format = DepthFormat;
    dsDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsDesc.Flags = D3D12_DSV_FLAG_NONE;

    D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
    depthOptimizedClearValue.Format = DepthFormat;
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;

    ComPtr<ID3D12Resource> depthStencilBuffer;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, app.windowWidth, app.windowHeight, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ASSERT_HRESULT(app.device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthOptimizedClearValue,
        IID_PPV_ARGS(&depthStencilBuffer)
    ));
    app.depthStencilBuffer = depthStencilBuffer;

    app.device->CreateDepthStencilView(
        app.depthStencilBuffer.Get(),
        &dsDesc,
        app.dsHeap->GetCPUDescriptorHandleForHeapStart()
    );
}

void SetupRenderTargets(App& app)
{
    auto frameBufferDescriptor = app.rtvDescriptorArena.AllocateDescriptors(FrameBufferCount, "FrameBuffer RTVs");
    for (int i = 0; i < FrameBufferCount; i++) {
        ASSERT_HRESULT(
            app.swapChain->GetBuffer(i, IID_PPV_ARGS(&app.renderTargets[i]))
        );

        app.frameBufferRTVs[i] = (frameBufferDescriptor + i);
        app.device->CreateRenderTargetView(app.renderTargets[i].Get(), nullptr, app.frameBufferRTVs[i].CPUHandle());
    }

}

D3D12_GRAPHICS_PIPELINE_STATE_DESC DefaultGraphicsPSODesc()
{
    // GLTF expects CCW winding order
    CD3DX12_RASTERIZER_DESC rasterizerState(D3D12_DEFAULT);
    rasterizerState.FrontCounterClockwise = TRUE;

    // Describe and create the graphics pipeline state object (PSO).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.RasterizerState = rasterizerState;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    return psoDesc;
}

ManagedPSORef CreateGraphicsPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const PSOGraphicsShaderPaths& paths,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc
)
{
    auto mPso = std::make_shared<ManagedGraphicsPSO>();
    mPso->desc = psoDesc;
    mPso->inputLayout = inputLayout;
    mPso->desc.pRootSignature = rootSignature;
    mPso->desc.InputLayout = { mPso->inputLayout.data(), (UINT)mPso->inputLayout.size() };
    mPso->shaderPaths = paths;
    mPso->reload(device);

    manager.PSOs.emplace_back(mPso);

    return mPso;
}

ManagedPSORef SimpleCreateGraphicsPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& baseShaderPath,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc
)
{
    std::wstring wBaseShaderPath = convert_to_wstring(baseShaderPath);

    const std::wstring vertexShaderPath = (wBaseShaderPath + L".cvert");
    const std::wstring pixelShaderPath = (wBaseShaderPath + L".cpixel");

    PSOGraphicsShaderPaths paths;
    paths.vertex = vertexShaderPath;
    paths.pixel = pixelShaderPath;

    return CreateGraphicsPSO(
        manager,
        device,
        paths,
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateComputePSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& baseShaderPath,
    ID3D12RootSignature* rootSignature,
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc
)
{
    auto mPso = std::make_shared<ManagedComputePSO>();
    mPso->desc = psoDesc;
    mPso->desc.pRootSignature = rootSignature;
    mPso->computeShaderPath = convert_to_wstring(baseShaderPath + ".ccomp");
    mPso->reload(device);

    manager.PSOs.emplace_back(mPso);

    return mPso;
}

ManagedPSORef CreateMipMapGeneratorPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature
)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    return CreateComputePSO(
        manager,
        device,
        dataDir + "generatemipmaps",
        rootSignature,
        desc
    );
}

D3D12_RESOURCE_DESC GBufferResourceDesc(GBufferTarget target, int windowWidth, int windowHeight);

ManagedPSORef CreateMeshPBRPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.NumRenderTargets = GBuffer_RTVCount + 1;
    for (int i = 1; i < psoDesc.NumRenderTargets; i++) {
        psoDesc.RTVFormats[i] = GBufferResourceDesc((GBufferTarget)(i - 1), 0, 0).Format;
    }
    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "mesh_gbuffer_pbr",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateMeshAlphaBlendedPBRPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.NumRenderTargets = GBuffer_RTVCount + 1;
    for (int i = 1; i < psoDesc.NumRenderTargets; i++) {
        psoDesc.RTVFormats[i] = GBufferResourceDesc((GBufferTarget)(i - 1), 0, 0).Format;
    }

    D3D12_RENDER_TARGET_BLEND_DESC blendDesc;
    blendDesc.BlendEnable = TRUE;
    blendDesc.LogicOpEnable = FALSE;
    blendDesc.SrcBlend = D3D12_BLEND_ONE;
    blendDesc.DestBlend = D3D12_BLEND_ONE;
    blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState.RenderTarget[0] = blendDesc;

    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "mesh_alpha_blended_pbr",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateMeshUnlitPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.NumRenderTargets = GBuffer_RTVCount + 1;
    for (int i = 1; i < psoDesc.NumRenderTargets; i++) {
        psoDesc.RTVFormats[i] = GBufferResourceDesc((GBufferTarget)(i - 1), 0, 0).Format;
    }
    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "mesh_gbuffer_unlit",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateDirectionalLightPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    // Unlike other PSOs, we go clockwise here.
    CD3DX12_RASTERIZER_DESC rasterizerState(D3D12_DEFAULT);
    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.RasterizerState = rasterizerState;

    // Blending enabled for the accumulation (back) buffer
    D3D12_RENDER_TARGET_BLEND_DESC blendDesc;
    blendDesc.BlendEnable = TRUE;
    blendDesc.LogicOpEnable = FALSE;
    blendDesc.SrcBlend = D3D12_BLEND_ONE;
    blendDesc.DestBlend = D3D12_BLEND_ONE;
    blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState.RenderTarget[0] = blendDesc;

    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "lighting_directional",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateEnvironmentCubemapLightPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    // Unlike other PSOs, we go clockwise here.
    CD3DX12_RASTERIZER_DESC rasterizerState(D3D12_DEFAULT);
    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.RasterizerState = rasterizerState;

    // Blending enabled for the accumulation (back) buffer
    D3D12_RENDER_TARGET_BLEND_DESC blendDesc;
    blendDesc.BlendEnable = TRUE;
    blendDesc.LogicOpEnable = FALSE;
    blendDesc.SrcBlend = D3D12_BLEND_ONE;
    blendDesc.DestBlend = D3D12_BLEND_ONE;
    blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState.RenderTarget[0] = blendDesc;

    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "lighting_environment_cubemap",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreatePointLightPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    // Unlike other PSOs, we go clockwise here.
    CD3DX12_RASTERIZER_DESC rasterizerState(D3D12_DEFAULT);
    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.RasterizerState = rasterizerState;

    // Blend settings for accumulation buffer
    D3D12_RENDER_TARGET_BLEND_DESC blendDesc;
    blendDesc.BlendEnable = TRUE;
    blendDesc.LogicOpEnable = FALSE;
    blendDesc.SrcBlend = D3D12_BLEND_ONE;
    blendDesc.DestBlend = D3D12_BLEND_ONE;
    blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState.RenderTarget[0] = blendDesc;

    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "lighting_point",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateSkyboxPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();
    CD3DX12_RASTERIZER_DESC rasterizerState(D3D12_DEFAULT);
    rasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState = rasterizerState;

    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "skybox",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateSkyboxDiffuseIrradiancePSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();
    CD3DX12_RASTERIZER_DESC rasterizerState(D3D12_DEFAULT);
    rasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState = rasterizerState;
    psoDesc.DepthStencilState.DepthEnable = false;
    psoDesc.DepthStencilState.StencilEnable = false;

    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "skybox_diffuse_irradiance",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

D3D12_RESOURCE_DESC GBufferResourceDesc(GBufferTarget target, int windowWidth, int windowHeight)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.MipLevels = 1;
    desc.Width = windowWidth;
    desc.Height = windowHeight;
    desc.DepthOrArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // https://www.3dgep.com/forward-plus/#:~:text=G%2Dbuffer%20pass.-,Layout%20Summary,G%2Dbuffer%20layout%20looks%20similar%20to%20the%20table%20shown%20below.,-R
    switch (target)
    {
    case GBuffer_BaseColor:
    case GBuffer_MetalRoughness:
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case GBuffer_Normal:
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        break;
    case GBuffer_Depth:
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        break;
    };

    return desc;
}

void SetupGBuffer(App& app, bool isResize)
{
    if (!isResize) {
        // Create SRV heap for the render targets
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = (UINT)GBuffer_Count + MaxLightCount + 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        //app.lightingPassDescriptorArena.Initialize(app.device.Get(), heapDesc, "LightPassArena");
        app.GBuffer.baseSrvReference = app.descriptorArena.AllocateDescriptors(GBuffer_Count, "GBuffer SRVs");
    } else {
        // If we're resizing then we need to release existing gbuffer
        for (auto& renderTarget : app.GBuffer.renderTargets) {
            renderTarget = nullptr;
        }
    }

    DescriptorRef rtvs = app.rtvDescriptorArena.AllocateDescriptors(GBuffer_RTVCount, "GBuffer RTVs");
    auto rtvHandle = rtvs.CPUHandle();
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle = app.GBuffer.baseSrvReference.CPUHandle();
    for (int i = 0; i < GBuffer_Depth; i++) {
        app.GBuffer.rtvs[i] = rtvs + i;

        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = GBufferResourceDesc(static_cast<GBufferTarget>(i), app.windowWidth, app.windowHeight);
        auto resourceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = resourceDesc.Format;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;
        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        memcpy(clearValue.Color, clearColor, sizeof(clearValue.Color));

        ASSERT_HRESULT(
            app.device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                resourceState,
                &clearValue,
                IID_PPV_ARGS(&app.GBuffer.renderTargets[i])
            )
        );

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = resourceDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        app.device->CreateShaderResourceView(app.GBuffer.renderTargets[i].Get(), &srvDesc, srvHandle);
        app.device->CreateRenderTargetView(app.GBuffer.renderTargets[i].Get(), nullptr, rtvHandle);

        rtvHandle.Offset(1, G_IncrementSizes.Rtv);
        srvHandle.Offset(1, G_IncrementSizes.CbvSrvUav);
    }

    // Depth buffer is special and does not get a render target.
    // We still need an SRV to sample in the deferred shader.
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT; // Can't use D32_FLOAT with SRVs...
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MipLevels = 1;
    app.device->CreateShaderResourceView(app.depthStencilBuffer.Get(), &depthSrvDesc, srvHandle);
}

void CreateConstantBufferAndViews(
    App& app,
    ComPtr<ID3D12Resource>& buffer,
    size_t elementSize,
    UINT count,
    D3D12_CPU_DESCRIPTOR_HANDLE baseDescriptorHandle
)
{
    const UINT constantBufferSize = (UINT)elementSize * count;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
    ASSERT_HRESULT(
        app.device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&buffer)
        )
    );

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(baseDescriptorHandle);
    for (int i = 0; i < count; i++) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = buffer->GetGPUVirtualAddress() + (i * elementSize);
        cbvDesc.SizeInBytes = elementSize;
        app.device->CreateConstantBufferView(
            &cbvDesc,
            cpuHandle
        );
        cpuHandle.Offset(1, G_IncrementSizes.CbvSrvUav);
    }
}

void CreateMainRootSignature(App& app)
{
    ID3D12Device* device = app.device.Get();
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
    {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    CD3DX12_ROOT_PARAMETER1 rootParameters[1];

    rootParameters->InitAsConstants(ConstantIndex_Count, 0);

    D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    // FIXME: This is going to have to be dynamic...
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 4;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = -D3D12_FLOAT32_MAX;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(
        _countof(rootParameters),
        rootParameters,
        1,
        &sampler,
        rootSignatureFlags
    );

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (!SUCCEEDED(
        D3DX12SerializeVersionedRootSignature(
            &rootSignatureDesc,
            featureData.HighestVersion,
            &signature,
            &error
        )
    )) {
        std::cout << "Error: root signature compilation failed\n";
        std::cout << (char*)error->GetBufferPointer();
        abort();
    }

    ASSERT_HRESULT(
        device->CreateRootSignature(
            0,
            signature->GetBufferPointer(),
            signature->GetBufferSize(),
            IID_PPV_ARGS(&app.rootSignature)
        )
    );
}

void SetupMipMapGenerator(App& app)
{
    ID3D12Device* device = app.device.Get();
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
    {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    CD3DX12_ROOT_PARAMETER1 rootParameters[1];

    rootParameters->InitAsConstants(6, 0);

    D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    CD3DX12_STATIC_SAMPLER_DESC sampler(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP
    );

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(
        _countof(rootParameters),
        rootParameters,
        1,
        &sampler,
        rootSignatureFlags
    );

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (!SUCCEEDED(
        D3DX12SerializeVersionedRootSignature(
            &rootSignatureDesc,
            featureData.HighestVersion,
            &signature,
            &error
        )
    )) {
        std::cout << "Error: root signature compilation failed\n";
        std::cout << (char*)error->GetBufferPointer();
        abort();
    }

    ASSERT_HRESULT(
        device->CreateRootSignature(
            0,
            signature->GetBufferPointer(),
            signature->GetBufferSize(),
            IID_PPV_ARGS(&app.MipMapGenerator.rootSignature)
        )
    );

    app.MipMapGenerator.PSO = CreateMipMapGeneratorPSO(
        app.psoManager,
        device,
        app.dataDir,
        app.MipMapGenerator.rootSignature.Get()
    );
}

void SetupLightBuffer(App& app)
{
    auto descriptorHandle = app.descriptorArena.AllocateDescriptors(MaxLightCount + 1, "light pass and light buffer");

    CreateConstantBufferAndViews(
        app,
        app.LightBuffer.constantBuffer,
        sizeof(LightConstantData),
        MaxLightCount + 1,
        descriptorHandle.CPUHandle()
    );

    app.LightBuffer.constantBuffer->Map(0, nullptr, (void**)&app.LightBuffer.passData);
    // Lights are stored immediately after the pass data
    app.LightBuffer.lightConstantData = reinterpret_cast<LightConstantData*>(app.LightBuffer.passData + 1);
    app.LightBuffer.cbvHandle = descriptorHandle;

    app.LightBuffer.passData->baseGBufferIndex = app.GBuffer.baseSrvReference.index;
    app.LightBuffer.passData->environmentIntensity = glm::vec4(5.0f);

    for (int i = 0; i < MaxLightCount; i++) {
        // Link convenient light structures back to the constant buffer
        app.lights[i].constantData = &app.LightBuffer.lightConstantData[i];
    }
}

void SetupMaterialBuffer(App& app)
{
    ComPtr<ID3D12Resource> resource;
    const UINT constantBufferSize = (UINT)sizeof(MaterialConstantData) * MaxMaterialCount;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
    ASSERT_HRESULT(
        app.device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&resource)
        )
    );

    app.materialConstantBuffer.InitializeWithBuffer(resource, 0);
}

void SetupGBufferPass(App& app)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = MaxDescriptors;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    app.descriptorArena.Initialize(app.device.Get(), heapDesc, "GBufferPassArena");

    SetupMaterialBuffer(app);
}

void SetupLightingPass(App& app)
{
    SetupLightBuffer(app);

    // GBuffer lighting does not need an input layout, as the vertices are created
    // entirely in the vertex buffer without any input vertices.
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;

    app.LightPass.pointLightPSO = CreatePointLightPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );

    app.LightPass.directionalLightPso = CreateDirectionalLightPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );

    app.LightPass.environentCubemapLightPso = CreateEnvironmentCubemapLightPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );
}

void PrintCapabilities(ID3D12Device* device, IDXGIAdapter1* adapter)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS featureSupport;
    ASSERT_HRESULT(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureSupport, sizeof(featureSupport)));
    switch (featureSupport.ResourceBindingTier)
    {
    case D3D12_RESOURCE_BINDING_TIER_1:
        std::cout << "Hardware is tier 1\n";
        break;

    case D3D12_RESOURCE_BINDING_TIER_2:
        // Tiers 1 and 2 are supported.
        std::cout << "Hardware is tier 2\n";
        break;

    case D3D12_RESOURCE_BINDING_TIER_3:
        // Tiers 1, 2, and 3 are supported.
        std::cout << "Hardware is tier 3\n";
        break;
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{};
    shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_7;
    ASSERT_HRESULT(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)));

    switch (shaderModel.HighestShaderModel) {
    case D3D_SHADER_MODEL_5_1:
        std::cout << "Shader model 5_1 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_0:
        std::cout << "Shader model 6_0 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_1:
        std::cout << "Shader model 6_1 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_2:
        std::cout << "Shader model 6_2 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_3:
        std::cout << "Shader model 6_3 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_4:
        std::cout << "Shader model 6_4 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_5:
        std::cout << "Shader model 6_5 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_6:
        std::cout << "Shader model 6_6 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_7:
        std::cout << "Shader model 6_7 is supported\n";
        break;
    }

    {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info;
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &info
            ))) {
                std::cout << "\nVideo memory information:\n";
                std::cout << "\tBudget: " << info.Budget << " bytes\n";
                std::cout << "\tAvailable for reservation: " << info.AvailableForReservation << " bytes\n";
                std::cout << "\tCurrent usage: " << info.CurrentUsage << " bytes\n";
                std::cout << "\tCurrent reservation: " << info.CurrentReservation << " bytes\n\n";
            }
        }
    }
}

void Aftermath_GpuCrashDumpCb(const void* pGpuCrashDump, const uint32_t gpuCrashDumpSize, void* pUserData)
{
    App* app = reinterpret_cast<App*>(pUserData);
}

void Aftermath_ShaderDebugInfoCb(const void* pShaderDebugInfo, const uint32_t shaderDebugInfoSize, void* pUserData)
{
    App* app = reinterpret_cast<App*>(pUserData);
}

void Aftermath_GpuCrashDumpDescriptionCb(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addValue, void* pUserData)
{

}

void Aftermath_ResolveMarkerCb(const void* pMarker, void* pUserData, void** resolvedMarkerData, uint32_t* markerSize)
{

}

void InitD3D(App& app)
{
    if (app.gpuDebug) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
        } else {
            std::cout << "Failed to enable D3D12 debug layer\n";
        }

        ComPtr<ID3D12Debug1> debugController1;
        ASSERT_HRESULT(debugController->QueryInterface(IID_PPV_ARGS(&debugController1)));
        debugController1->SetEnableGPUBasedValidation(true);

        // GFSDK_Aftermath_EnableGpuCrashDumps(
        //     GFSDK_Aftermath_Version_API,
        //     GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_DX,
        //     GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
        //     Aftermath_GpuCrashDumpCb,
        //     Aftermath_ShaderDebugInfoCb,
        //     Aftermath_GpuCrashDumpDescriptionCb,
        //     Aftermath_ResolveMarkerCb,
        //     reinterpret_cast<void*>(&app)
        // );

        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> pDredSettings;
        ASSERT_HRESULT(D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings)));
        pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);

        DXGIGetDebugInterface1(0, IID_PPV_ARGS(&app.graphicsAnalysis));
    }

    ComPtr<IDXGIFactory4> dxgiFactory;
    ASSERT_HRESULT(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&dxgiFactory)));

    ComPtr<IDXGIAdapter1> adapter;
    ASSERT_HRESULT(
        dxgiFactory->EnumAdapters1(0, &adapter)
    );

    ASSERT_HRESULT(
        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&app.device))
    );

    ID3D12Device* device = app.device.Get();

    if (app.gpuDebug) {
        // Break on debug layer errors or corruption
        ComPtr<ID3D12InfoQueue> infoQueue;
        app.device->QueryInterface(IID_PPV_ARGS(&infoQueue));
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
    }

    D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
    allocatorDesc.pDevice = device;
    allocatorDesc.pAdapter = adapter.Get();
    ASSERT_HRESULT(
        D3D12MA::CreateAllocator(&allocatorDesc, &app.mainAllocator)
    );

    PrintCapabilities(device, adapter.Get());

    // Graphics command queue
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        ASSERT_HRESULT(
            device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&app.commandQueue))
        );
    }

    // Copy command queue
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;

        ASSERT_HRESULT(
            device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&app.copyCommandQueue))
        );
    }

    // Compute command queue
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;

        ASSERT_HRESULT(
            device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&app.computeQueue))
        );
    }

    {
        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChainDesc.BufferCount = FrameBufferCount;
        swapChainDesc.BufferDesc.Width = app.windowWidth;
        swapChainDesc.BufferDesc.Height = app.windowHeight;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.OutputWindow = app.hwnd;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        ComPtr<IDXGISwapChain> swapChain;
        ASSERT_HRESULT(
            dxgiFactory->CreateSwapChain(
                app.commandQueue.Get(),
                &swapChainDesc,
                &swapChain
            )
        );
        ASSERT_HRESULT(swapChain.As(&app.swapChain));
    }

    ASSERT_HRESULT(
        dxgiFactory->MakeWindowAssociation(app.hwnd, DXGI_MWA_NO_ALT_ENTER)
    );

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();

    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameBufferCount + GBuffer_RTVCount + 16;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        app.rtvDescriptorArena.Initialize(app.device.Get(), rtvHeapDesc, "RTV Heap Arena");
    }

    G_IncrementSizes.CbvSrvUav = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    G_IncrementSizes.Rtv = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    SetupRenderTargets(app);
    SetupDepthStencil(app, false);

    SetupGBufferPass(app);
    SetupGBuffer(app, false);

    CreateMainRootSignature(app);

    SetupMipMapGenerator(app);

    SetupLightingPass(app);

    ASSERT_HRESULT(
        device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COPY,
            IID_PPV_ARGS(&app.copyCommandAllocator)
        )
    );

    ASSERT_HRESULT(
        device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            IID_PPV_ARGS(&app.computeCommandAllocator)
        )
    );

    ASSERT_HRESULT(
        device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&app.commandAllocator)
        )
    );

    // Create command lists
    {
        ASSERT_HRESULT(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                app.commandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&app.commandList)
            )
        );

        ASSERT_HRESULT(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_COPY,
                app.copyCommandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&app.copyCommandList)
            )
        );

        ASSERT_HRESULT(app.copyCommandList->Close());
    }

    {
        app.fence.Initialize(app.device.Get());
        app.copyFence.Initialize(app.device.Get());
    }
}

void InitWindow(App& app)
{
    Uint32 windowFlags = SDL_WINDOW_RESIZABLE;
    if (app.borderlessFullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    SDL_Window* window = SDL_CreateWindow("MDXR",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        app.windowWidth, app.windowHeight,
        windowFlags
    );

    assert(window);

    app.window = window;

    SDL_SysWMinfo wmInfo = {};
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
        std::cout << "Failed to fetch window info from SDL\n";
        std::cout << "SDL_GetError(): " << SDL_GetError() << std::endl;
        abort();
    }
    app.hwnd = wmInfo.info.win.window;

    app.keyState = SDL_GetKeyboardState(nullptr);
}

void CreateDataDirWatchHandle(App& app)
{
    app.shaderWatchHandle = FindFirstChangeNotificationW(app.wDataDir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    assert(app.shaderWatchHandle != INVALID_HANDLE_VALUE);
}

void InitApp(App& app, int argc, char** argv)
{
    app.viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(app.windowWidth), static_cast<float>(app.windowHeight));
    app.scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(app.windowWidth), static_cast<LONG>(app.windowHeight));

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--datadir")) {
            if (i + 1 < argc) {
                app.dataDir = argv[i + 1];
            }
        } else if (!strcmp(argv[i], "--borderless")) {
            app.borderlessFullscreen = true;
        } else if (!strcmp(argv[i], "--gpudebug")) {
            app.gpuDebug = true;
        }
    }

    if (app.dataDir.empty()) {
        std::cout << "Error: no data directory specified\n";
        std::cout << "The program will now exit" << std::endl;
        abort();
    }

    app.wDataDir = convert_to_wstring(app.dataDir);
    CreateDataDirWatchHandle(app);
}


void WaitForPreviousFrame(App& app)
{
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity. More advanced samples 
    // illustrate how to use fences for efficient resource usage.

    app.fence.SignalAndWait(app.commandQueue.Get());

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();
}

void LoadModelAsset(const App& app, AssetBundle& assets, tinygltf::TinyGLTF& loader, const std::string& filePath)
{
    tinygltf::Model model;
    assets.models.push_back(model);
    std::string err;
    std::string warn;
    CHECK(
        loader.LoadASCIIFromFile(
            &assets.models.back(),
            &err,
            &warn,
            filePath
        )
    );
}

std::vector<unsigned char> LoadBinaryFile(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);

    file.unsetf(std::ios::skipws);

    std::streampos fileSize;
    file.seekg(0, std::ios::end);
    fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<unsigned char> data;
    data.reserve(fileSize);

    data.insert(data.begin(), std::istream_iterator<unsigned char>(file), std::istream_iterator<unsigned char>());

    return data;
}

tinygltf::Image LoadImageFile(const std::string& imagePath)
{
    tinygltf::Image image;

    auto fileData = LoadBinaryFile(imagePath);
    unsigned char* imageData = stbi_load_from_memory(fileData.data(), fileData.size(), &image.width, &image.height, nullptr, STBI_rgb_alpha);
    if (!imageData) {
        std::cout << "Failed to load " << imagePath << ": " << stbi_failure_reason() << "\n";
        assert(false);
    }
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.component = STBI_rgb_alpha;

    // Copy image data over
    image.image.assign(imageData, imageData + (image.width * image.height * STBI_rgb_alpha));

    stbi_image_free(imageData);

    return image;
}

AssetBundle LoadAssets(App& app)
{
    AssetBundle assets;

    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    LoadModelAsset(app, assets, loader, app.dataDir + "/floor/floor.gltf");
    LoadModelAsset(app, assets, loader, app.dataDir + "/metallicsphere.gltf");

    SkyboxAssets skybox;
    skybox.images[SkyboxImage_Front] = LoadImageFile(app.dataDir + "/DebugSky/pz.png");
    skybox.images[SkyboxImage_Back] = LoadImageFile(app.dataDir + "/DebugSky/pz.png");
    skybox.images[SkyboxImage_Left] = LoadImageFile(app.dataDir + "/DebugSky/px.png");
    skybox.images[SkyboxImage_Right] = LoadImageFile(app.dataDir + "/DebugSky/px.png");
    skybox.images[SkyboxImage_Top] = LoadImageFile(app.dataDir + "/DebugSky/py.png");
    skybox.images[SkyboxImage_Bottom] = LoadImageFile(app.dataDir + "/DebugSky/py.png");
    // skybox.images[SkyboxImage_Front] = LoadImageFile(app.dataDir + "/ColorfulStudio/pz.png");
    // skybox.images[SkyboxImage_Back] = LoadImageFile(app.dataDir + "/ColorfulStudio/nz.png");
    // skybox.images[SkyboxImage_Left] = LoadImageFile(app.dataDir + "/ColorfulStudio/nx.png");
    // skybox.images[SkyboxImage_Right] = LoadImageFile(app.dataDir + "/ColorfulStudio/px.png");
    // skybox.images[SkyboxImage_Top] = LoadImageFile(app.dataDir + "/ColorfulStudio/py.png");
    // skybox.images[SkyboxImage_Bottom] = LoadImageFile(app.dataDir + "/ColorfulStudio/ny.png");
    // skybox.images[SkyboxImage_Front] = LoadImageFile(app.dataDir + "/skybox/front.png");
    // skybox.images[SkyboxImage_Back] = LoadImageFile(app.dataDir + "/skybox/back.png");
    // skybox.images[SkyboxImage_Left] = LoadImageFile(app.dataDir + "/skybox/left.png");
    // skybox.images[SkyboxImage_Right] = LoadImageFile(app.dataDir + "/skybox/right.png");
    // skybox.images[SkyboxImage_Top] = LoadImageFile(app.dataDir + "/skybox/top.png");
    // skybox.images[SkyboxImage_Bottom] = LoadImageFile(app.dataDir + "/skybox/bottom.png");
    assets.skybox = skybox;

    return assets;
}

void CreateModelDescriptors(
    App& app,
    const tinygltf::Model& inputModel,
    Model& outputModel,
    const std::span<ComPtr<ID3D12Resource>> textureResources
)
{
    // Allocate 1 descriptor for the constant buffer and the rest for the textures.
    size_t numConstantBuffers = 0;
    for (const auto& mesh : inputModel.meshes) {
        numConstantBuffers += mesh.primitives.size();
    }
    size_t numDescriptors = numConstantBuffers + inputModel.textures.size();

    UINT incrementSize = G_IncrementSizes.CbvSrvUav;

    // Create per-primitive constant buffer
    ComPtr<ID3D12Resource> perPrimitiveConstantBuffer;
    {
        auto descriptorRef = app.descriptorArena.AllocateDescriptors(numConstantBuffers, "PerPrimitiveConstantBuffer");
        auto cpuHandle = descriptorRef.CPUHandle();
        CreateConstantBufferAndViews(
            app,
            perPrimitiveConstantBuffer,
            sizeof(PerPrimitiveConstantData),
            numConstantBuffers,
            cpuHandle
        );
        outputModel.primitiveDataDescriptors = descriptorRef;
    }

    // Create SRVs
    {
        auto descriptorRef = app.descriptorArena.AllocateDescriptors(textureResources.size(), "MeshTextures");
        auto cpuHandle = descriptorRef.CPUHandle();
        //CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), (int)inputModel.meshes.size(), incrementSize);
        for (const auto& textureResource : textureResources) {
            auto textureDesc = textureResource->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = textureDesc.Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = textureDesc.MipLevels;
            app.device->CreateShaderResourceView(textureResource.Get(), &srvDesc, cpuHandle);
            cpuHandle.Offset(1, incrementSize);
        }
        outputModel.baseTextureDescriptor = descriptorRef;
    }

    outputModel.perPrimitiveConstantBuffer = perPrimitiveConstantBuffer;
    CD3DX12_RANGE readRange(0, 0);
    ASSERT_HRESULT(perPrimitiveConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&outputModel.perPrimitiveBufferPtr)));
}

void CreateModelMaterials(
    App& app,
    const tinygltf::Model& inputModel,
    Model& outputModel,
    std::vector<SharedPoolItem<Material>>& modelMaterials
)
{
    // Allocate material constant buffers and create views
    int materialCount = inputModel.materials.size();
    auto descriptorReference = app.descriptorArena.AllocateDescriptors(materialCount, "model materials");
    auto constantBufferSlice = app.materialConstantBuffer.Allocate(materialCount);
    app.materialConstantBuffer.CreateViews(app.device.Get(), constantBufferSlice, descriptorReference.CPUHandle());
    outputModel.baseMaterialDescriptor = descriptorReference;

    auto baseTextureDescriptor = outputModel.baseTextureDescriptor;

    //startingMaterialIdx = app.materials.size();

    for (int i = 0; i < materialCount; i++) {
        auto& inputMaterial = inputModel.materials[i];

        DescriptorRef baseColorTextureDescriptor;
        DescriptorRef normalTextureDescriptor;
        DescriptorRef metalRoughnessTextureDescriptor;

        if (inputMaterial.pbrMetallicRoughness.baseColorTexture.index != -1) {
            auto texture = inputModel.textures[inputMaterial.pbrMetallicRoughness.baseColorTexture.index];
            auto imageIdx = texture.source;
            baseColorTextureDescriptor = baseTextureDescriptor + imageIdx;
        }
        if (inputMaterial.normalTexture.index != -1) {
            auto texture = inputModel.textures[inputMaterial.normalTexture.index];
            auto imageIdx = texture.source;
            normalTextureDescriptor = baseTextureDescriptor + imageIdx;
        }
        if (inputMaterial.pbrMetallicRoughness.metallicRoughnessTexture.index != -1) {
            auto texture = inputModel.textures[inputMaterial.pbrMetallicRoughness.metallicRoughnessTexture.index];
            auto imageIdx = texture.source;
            metalRoughnessTextureDescriptor = baseTextureDescriptor + imageIdx;
        }

        MaterialType materialType = MaterialType_PBR;
        if (inputMaterial.extensions.contains("KHR_materials_unlit")) {
            materialType = MaterialType_Unlit;
        } else if (inputMaterial.alphaMode.size() > 0 && inputMaterial.alphaMode != "OPAQUE") {
            if (inputMaterial.alphaMode == "BLEND") {
                materialType = MaterialType_AlphaBlendPBR;
            } else {
                std::cout << "GLTF material " << inputMaterial.name << " has unsupported alpha mode and will be treated as opaque\n";
                materialType = MaterialType_PBR;
            }
        }

        auto material = app.materials.AllocateShared();
        // Material material;
        material->constantData = &constantBufferSlice.data[i];
        material->materialType = materialType;
        material->textureDescriptors.baseColor = baseColorTextureDescriptor;
        material->textureDescriptors.normal = normalTextureDescriptor;
        material->textureDescriptors.metalRoughness = metalRoughnessTextureDescriptor;
        material->baseColorFactor.r = inputMaterial.pbrMetallicRoughness.baseColorFactor[0];
        material->baseColorFactor.g = inputMaterial.pbrMetallicRoughness.baseColorFactor[1];
        material->baseColorFactor.b = inputMaterial.pbrMetallicRoughness.baseColorFactor[2];
        material->baseColorFactor.a = inputMaterial.pbrMetallicRoughness.baseColorFactor[3];
        material->metalRoughnessFactor.g = inputMaterial.pbrMetallicRoughness.roughnessFactor;
        material->metalRoughnessFactor.b = inputMaterial.pbrMetallicRoughness.metallicFactor;
        material->cbvDescriptor = descriptorReference + i;
        material->name = inputMaterial.name;
        material->UpdateConstantData();

        modelMaterials.push_back(SharedPoolItem<Material>(material));
    }
}

D3D12_RESOURCE_DESC GetImageResourceDesc(const tinygltf::Image& image)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_FLAG_NONE;
    desc.DepthOrArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    // Generate as many mips as we can
    UINT highestDimension = std::max((UINT)desc.Width, desc.Height);
    desc.MipLevels = (UINT16)std::floor(std::log2(highestDimension)) + 1;

    CHECK(image.component == STBI_rgb_alpha);

    switch (image.pixel_type) {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        break;
    default:
        abort();
    };

    return desc;
}

void DeviceRemovedHandler(ID3D12Device* device)
{
    ComPtr<ID3D12DeviceRemovedExtendedData> pDred;
    ASSERT_HRESULT(device->QueryInterface(IID_PPV_ARGS(&pDred)));

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT DredAutoBreadcrumbsOutput;
    D3D12_DRED_PAGE_FAULT_OUTPUT DredPageFaultOutput;
    ASSERT_HRESULT(pDred->GetPageFaultAllocationOutput(&DredPageFaultOutput));
    ASSERT_HRESULT(pDred->GetAutoBreadcrumbsOutput(&DredAutoBreadcrumbsOutput));

    std::cout << "PAGE FAULT INFORMATION:\n"
        << "\tVirtualAddress: " << DredPageFaultOutput.PageFaultVA << "\n";

    std::cout << "DRED Breadcrumbs:\n";
    auto breadcrumb = DredAutoBreadcrumbsOutput.pHeadAutoBreadcrumbNode;
    while (breadcrumb) {
        std::cout << "\tCommandList: " << breadcrumb->pCommandListDebugNameA << "\n";
        std::cout << "\tBreadcrumbCount: " << breadcrumb->BreadcrumbCount << "\n";
        breadcrumb = breadcrumb->pNext;
    }
}

// Generates mip maps for a range of textures. The `textures` must have their
// MipLevels already set. Textures must also be UAV compatible.
void GenerateMipMaps(App& app, const std::span<ComPtr<ID3D12Resource>>& textures, const std::vector<bool>& textureIsSRGB)
{
    if (textures.size() == 0) {
        return;
    }

    ScopedPerformanceTracker perf("GenerateMipMaps", PerformancePrecision::Milliseconds);

    UINT descriptorCount = 0;

    // Create SRVs for the base mip
    auto baseTextureDescriptor = app.descriptorArena.PushDescriptorStack(textures.size());
    descriptorCount += textures.size();
    for (int i = 0; i < textures.size(); i++) {
        auto textureDesc = textures[i]->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = textureDesc.MipLevels;
        app.device->CreateShaderResourceView(textures[i].Get(), &srvDesc, (baseTextureDescriptor + i).CPUHandle());
    }

    ComPtr<D3D12MA::Allocation> constantBuffer;

    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(GenerateMipsConstantData));
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    ASSERT_HRESULT(
        app.mainAllocator->CreateResource(
            &allocDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            &constantBuffer,
            IID_NULL, nullptr
        )
    );

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ASSERT_HRESULT(
        app.device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            app.computeCommandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&commandList)
        )
    );

    commandList->SetPipelineState(app.MipMapGenerator.PSO->Get());
    commandList->SetComputeRootSignature(app.MipMapGenerator.rootSignature.Get());

    ID3D12DescriptorHeap* ppHeaps[] = { app.descriptorArena.Heap() };
    commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    size_t cbvCount = 0;
    // Dry run to find the total number of constant buffers to allocate
    for (size_t textureIdx = 0; textureIdx < textures.size(); textureIdx++) {
        auto resourceDesc = textures[textureIdx]->GetDesc();
        for (UINT srcMip = 0; srcMip < resourceDesc.MipLevels - 1; ) {
            UINT64 srcWidth = resourceDesc.Width >> srcMip;
            UINT64 srcHeight = resourceDesc.Height >> srcMip;
            UINT64 dstWidth = (UINT)(srcWidth >> 1);
            UINT64 dstHeight = srcHeight >> 1;

            DWORD mipCount;
            _BitScanForward(&mipCount, (dstWidth == 1 ? dstHeight : dstWidth) | (dstHeight == 1 ? dstWidth : dstHeight));

            mipCount = std::min<DWORD>(4, mipCount + 1);
            mipCount = (srcMip + mipCount) >= resourceDesc.MipLevels ? resourceDesc.MipLevels - srcMip - 1 : mipCount;

            srcMip += mipCount;

            cbvCount++;
        }
    }

    ConstantBufferArena<GenerateMipsConstantData> constantBufferArena;
    constantBufferArena.InitializeWithCapacity(app.mainAllocator.Get(), cbvCount);
    auto constantBuffers = constantBufferArena.Allocate(cbvCount);

    auto cbvs = app.descriptorArena.PushDescriptorStack(cbvCount);
    descriptorCount += cbvCount;

    constantBufferArena.CreateViews(app.device.Get(), constantBuffers, cbvs.CPUHandle());

    UINT totalUAVs = 0;
    UINT cbvIndex = 0;

    for (size_t textureIdx = 0; textureIdx < textures.size(); textureIdx++) {
        auto resourceDesc = textures[textureIdx]->GetDesc();
        std::vector<CD3DX12_RESOURCE_BARRIER> resourceBarriers;
        for (UINT srcMip = 0; srcMip < resourceDesc.MipLevels - 1; ) {
            UINT64 srcWidth = resourceDesc.Width >> srcMip;
            UINT64 srcHeight = resourceDesc.Height >> srcMip;
            UINT64 dstWidth = (UINT)(srcWidth >> 1);
            UINT64 dstHeight = srcHeight >> 1;

            DWORD mipCount;
            _BitScanForward(&mipCount, (dstWidth == 1 ? dstHeight : dstWidth) | (dstHeight == 1 ? dstWidth : dstHeight));

            mipCount = std::min<DWORD>(4, mipCount + 1);
            mipCount = (srcMip + mipCount) >= resourceDesc.MipLevels ? resourceDesc.MipLevels - srcMip - 1 : mipCount;

            dstWidth = std::max<DWORD>(1, dstWidth);
            dstHeight = std::max<DWORD>(1, dstHeight);

            auto uavs = app.descriptorArena.PushDescriptorStack(mipCount);
            descriptorCount += mipCount;
            for (UINT mip = 0; mip < mipCount; mip++) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                uavDesc.Format = resourceDesc.Format;
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = srcMip + mip + 1;

                app.device->CreateUnorderedAccessView(textures[textureIdx].Get(), nullptr, &uavDesc, (uavs + mip).CPUHandle());
            }

            constantBuffers.data[cbvIndex].srcMipLevel = srcMip;
            constantBuffers.data[cbvIndex].srcDimension = (srcHeight & 1) << 1 | (srcWidth & 1);
            constantBuffers.data[cbvIndex].isSRGB = 0 /*textureIsSRGB[textureIdx]*/; // SRGB does not seem to work right
            constantBuffers.data[cbvIndex].numMipLevels = mipCount;
            constantBuffers.data[cbvIndex].texelSize.x = 1.0f / (float)dstWidth;
            constantBuffers.data[cbvIndex].texelSize.y = 1.0f / (float)dstHeight;

            UINT constantValues[6] = {
                uavs.index,
                uavs.index + 1,
                uavs.index + 2,
                uavs.index + 3,
                cbvs.index + cbvIndex,
                (baseTextureDescriptor + textureIdx).index
            };

            commandList->SetComputeRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
            commandList->Dispatch(std::ceil((float)dstWidth / 8.0f), std::ceil((float)dstHeight / 8.0f), 1);

            CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(textures[textureIdx].Get());
            commandList->ResourceBarrier(1, &barrier);

            cbvIndex++;
            srcMip += mipCount;
        }
    }

    commandList->Close();

    ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
    app.computeQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
    app.fence.SignalAndWait(app.computeQueue.Get());

    app.descriptorArena.PopDescriptorStack(descriptorCount);
}

void LoadModelTextures(
    App& app,
    Model& outputModel,
    tinygltf::Model& inputModel,
    std::vector<CD3DX12_RESOURCE_BARRIER>& resourceBarriers,
    const std::vector<bool>& textureIsSRGB
)
{
    // We generate mips on unordered access view textures, but these textures
    // are slow for rendering, so we have to copy them to normal textures
    // aftwards.
    std::vector<ComPtr<ID3D12Resource>> stagingTexturesForMipMaps;
    stagingTexturesForMipMaps.reserve(inputModel.images.size());

    UploadBatch uploadBatch;
    uploadBatch.Begin(app.mainAllocator.Get(), app.copyCommandQueue.Get(), &app.copyFence);

    // Upload images to buffers
    for (int i = 0; i < inputModel.images.size(); i++) {
        const auto& gltfImage = inputModel.images[i];
        ComPtr<ID3D12Resource> buffer;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = GetImageResourceDesc(gltfImage);
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto resourceState = D3D12_RESOURCE_STATE_COMMON;
        ASSERT_HRESULT(
            app.device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                resourceState,
                nullptr,
                IID_PPV_ARGS(&buffer)
            )
        );

        D3D12_SUBRESOURCE_DATA subresourceData;
        subresourceData.pData = gltfImage.image.data();
        subresourceData.RowPitch = gltfImage.width * gltfImage.component;
        subresourceData.SlicePitch = gltfImage.height * subresourceData.RowPitch;
        uploadBatch.AddTexture(buffer.Get(), &subresourceData, 0, 1);

        stagingTexturesForMipMaps.push_back(buffer);
    }

    uploadBatch.Finish();

    // Now that the images are uploaded these can be free'd
    for (auto& image : inputModel.images) {
        image.image.clear();
        image.image.shrink_to_fit();
    }

    GenerateMipMaps(app, stagingTexturesForMipMaps, textureIsSRGB);

    D3D12MA::Budget localBudget;
    app.mainAllocator->GetBudget(&localBudget, nullptr);

    // Only use up to 75% remaining memory on these uploads
    size_t maxUploadBytes = localBudget.BudgetBytes / 2;
    size_t pendingUploadBytes = 0;

    ASSERT_HRESULT(
        app.copyCommandList->Reset(
            app.copyCommandAllocator.Get(),
            nullptr
        )
    );

    // Copy mip map textures to non-UAV textures for the model.
    for (int textureIdx = 0; textureIdx < inputModel.images.size(); textureIdx++) {
        // FIXME: This would be much better with aliased resources, but that will 
        // require switching the model to use the D3D12MA system for placed resources.
        ComPtr<ID3D12Resource> destResource;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = GetImageResourceDesc(inputModel.images[textureIdx]);

        auto allocInfo = app.device->GetResourceAllocationInfo(0, 1, &resourceDesc);
        if (pendingUploadBytes > 0 && allocInfo.SizeInBytes + pendingUploadBytes > maxUploadBytes) {
            // Flush the upload
            ASSERT_HRESULT(app.copyCommandList->Close());
            ID3D12CommandList* ppCommandLists[] = { app.copyCommandList.Get() };
            app.copyCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
            app.copyFence.SignalAndWait(app.copyCommandQueue.Get());

            // Clear the previous staging textures to free memory
            for (int stageIdx = 0; stageIdx < textureIdx; stageIdx++) {
                stagingTexturesForMipMaps[stageIdx] = nullptr;
            }

            ASSERT_HRESULT(
                app.copyCommandList->Reset(
                    app.copyCommandAllocator.Get(),
                    nullptr
                )
            );

            pendingUploadBytes = 0;
        }

        ASSERT_HRESULT(
            app.device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&destResource)
            )
        );

        pendingUploadBytes += allocInfo.SizeInBytes;

        app.copyCommandList->CopyResource(destResource.Get(), stagingTexturesForMipMaps[textureIdx].Get());
        outputModel.buffers.push_back(destResource);

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            outputModel.buffers.back().Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        ));
    }

    ASSERT_HRESULT(app.copyCommandList->Close());
    if (pendingUploadBytes > 0) {
        ID3D12CommandList* ppCommandLists[] = { app.copyCommandList.Get() };
        app.copyCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
        app.copyFence.SignalAndWait(app.copyCommandQueue.Get());
    }
}

// Gets a vector of booleans parallel to inputModel.images to determine which
// images are SRGB. This information is needed to generate the mipmaps properly.
std::vector<bool> DetermineSRGBTextures(const tinygltf::Model& inputModel)
{
    std::vector<bool> textureIsSRGB(inputModel.images.size(), false);

    // The only textures in a GLTF model that are SRGB are the base color textures.
    for (const auto& material : inputModel.materials) {
        auto textureIndex = material.pbrMetallicRoughness.baseColorTexture.index;
        if (textureIndex != -1) {
            auto imageIndex = inputModel.textures[textureIndex].source;
            textureIsSRGB[imageIndex] = true;
        }
    }

    return textureIsSRGB;
}

std::vector<ComPtr<ID3D12Resource>> UploadModelBuffers(
    Model& outputModel,
    App& app,
    tinygltf::Model& inputModel,
    const std::vector<UINT64>& uploadOffsets,
    std::span<ComPtr<ID3D12Resource>>& outGeometryResources,
    std::span<ComPtr<ID3D12Resource>>& outTextureResources
)
{
    std::vector<bool> textureIsSRGB = DetermineSRGBTextures(inputModel);

    std::vector<ComPtr<ID3D12Resource>>& resourceBuffers = outputModel.buffers;
    resourceBuffers.reserve(inputModel.buffers.size() + inputModel.images.size());

    UploadBatch uploadBatch;
    uploadBatch.Begin(app.mainAllocator.Get(), app.copyCommandQueue.Get(), &app.copyFence);

    std::vector<CD3DX12_RESOURCE_BARRIER> resourceBarriers;

    // Copy all the gltf buffer data to a dedicated geometry buffer
    for (size_t bufferIdx = 0; bufferIdx < inputModel.buffers.size(); bufferIdx++) {
        const auto& gltfBuffer = inputModel.buffers[bufferIdx];
        ComPtr<ID3D12Resource> geometryBuffer;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(gltfBuffer.data.size());
        auto resourceState = D3D12_RESOURCE_STATE_COMMON;
        ASSERT_HRESULT(
            app.device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                resourceState,
                nullptr,
                IID_PPV_ARGS(&geometryBuffer)
            )
        );

        uploadBatch.AddBuffer(geometryBuffer.Get(), 0, (void*)gltfBuffer.data.data(), gltfBuffer.data.size());

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            geometryBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER
        ));

        resourceBuffers.push_back(geometryBuffer);
    }

    uploadBatch.Finish();

    LoadModelTextures(
        app,
        outputModel,
        inputModel,
        resourceBarriers,
        textureIsSRGB
    );

    auto endGeometryBuffer = resourceBuffers.begin() + inputModel.buffers.size();
    outGeometryResources = std::span(resourceBuffers.begin(), endGeometryBuffer);
    outTextureResources = std::span(endGeometryBuffer, resourceBuffers.end());

    app.commandList->ResourceBarrier((UINT)resourceBarriers.size(), resourceBarriers.data());

    return resourceBuffers;
}

glm::mat4 GetNodeTransfomMatrix(const tinygltf::Node& node) {
    if (node.matrix.size() > 0) {
        CHECK(node.matrix.size() == 16);
        return glm::make_mat4(node.matrix.data());;
    } else {
        glm::vec3 translate(0.0f);
        glm::quat rotation = glm::quat_identity<float, glm::defaultp>();
        glm::vec3 scale(1.0f);
        if (node.translation.size() > 0) {
            auto translationData = node.translation.data();
            translate = glm::make_vec3(translationData);
        }
        if (node.rotation.size() > 0) {
            auto rotationData = node.rotation.data();
            rotation = glm::make_quat(rotationData);
        }
        if (node.scale.size() > 0) {
            auto scaleData = node.scale.data();
            scale = glm::make_vec3(scaleData);
        }

        glm::mat4 modelMatrix(1.0f);
        glm::mat4 T = glm::translate(glm::mat4(1.0f), translate);
        glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
        glm::mat4 R = glm::toMat4(rotation);
        return S * R * T;
    }
}

void TraverseNode(const tinygltf::Model& model, const tinygltf::Node& node, std::vector<PoolItem<Mesh>>& meshes, const glm::mat4& accumulator) {
    glm::mat4 transform = accumulator * GetNodeTransfomMatrix(node);
    if (node.mesh != -1) {
        meshes[node.mesh]->baseModelTransform = transform;
    }
    for (const auto& child : node.children) {
        TraverseNode(model, model.nodes[child], meshes, transform);
    }
}

// Traverse the GLTF scene to get the correct model matrix for each mesh.
void ResolveMeshTransforms(
    const tinygltf::Model& model,
    std::vector<PoolItem<Mesh>>& meshes
)
{
    if (model.scenes.size() == 0) {
        return;
    }

    int scene = model.defaultScene != 0 ? model.defaultScene : 0;
    for (const auto& node : model.scenes[scene].nodes) {
        TraverseNode(model, model.nodes[node], meshes, glm::mat4(1.0f));
    }
}

PoolItem<Primitive> CreateModelPrimitive(
    App& app,
    Model& outputModel,
    const tinygltf::Model& inputModel,
    const tinygltf::Mesh& inputMesh,
    const tinygltf::Primitive& inputPrimitive,
    const std::vector<SharedPoolItem<Material>>& modelMaterials,
    int perPrimitiveDescriptorIdx
)
{
    // Just storing these strings so that we don't have to keep the Model object around.
    static std::array SEMANTIC_NAMES{
        std::string("POSITION"),
        std::string("NORMAL"),
        std::string("TEXCOORD"),
        std::string("TANGENT"),
    };

    // GLTF stores attribute names like "TEXCOORD_0", "TEXCOORD_1", etc.
    // But DirectX expects SemanticName "TEXCOORD" and SemanticIndex 0
    // So parse it out here
    auto ParseAttribToSemantic = [](const std::string& attribName) -> std::pair<std::string, int>
    {
        auto underscorePos = attribName.find('_');
        if (underscorePos == std::string::npos) {
            return std::pair(attribName, 0);
        } else {
            std::string semantic = attribName.substr(0, underscorePos);
            int index = std::stoi(attribName.substr(underscorePos + 1));
            return std::pair(semantic, index);
        }
    };

    const std::vector<ComPtr<ID3D12Resource>>& resourceBuffers = outputModel.buffers;

    auto primitive = app.primitivePool.AllocateUnique();

    primitive->perPrimitiveDescriptor = outputModel.primitiveDataDescriptors + perPrimitiveDescriptorIdx;
    primitive->constantData = &outputModel.perPrimitiveBufferPtr[perPrimitiveDescriptorIdx];
    perPrimitiveDescriptorIdx++;

    std::vector<D3D12_VERTEX_BUFFER_VIEW>& vertexBufferViews = primitive->vertexBufferViews;

    // Track what addresses are mapped to a vertex buffer view.
    // 
    // The key is the address in the buffer of the first vertex,
    // the value is an index into the vertexBufferViews array.
    // 
    // This needs to be done because certain GLTF models are designed in a way that 
    // doesn't allow us to have a one-to-one relationship between gltf buffer views 
    // and d3d buffer views.
    std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT> vertexStartOffsetToBufferView;

    // Build per drawcall data 
    // input layout and vertex buffer views
    std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout = primitive->inputLayout;
    inputLayout.reserve(inputPrimitive.attributes.size());

    for (const auto& attrib : inputPrimitive.attributes) {
        auto [targetSemantic, semanticIndex] = ParseAttribToSemantic(attrib.first);
        auto semanticName = std::find(SEMANTIC_NAMES.begin(), SEMANTIC_NAMES.end(), targetSemantic);

        if (semanticName == SEMANTIC_NAMES.end()) {
            std::cout << "Unsupported semantic in " << inputMesh.name << " " << targetSemantic << "\n";
            continue;
        }

        D3D12_INPUT_ELEMENT_DESC desc;
        int accessorIdx = attrib.second;
        auto& accessor = inputModel.accessors[accessorIdx];
        desc.SemanticName = semanticName->c_str();
        desc.SemanticIndex = semanticIndex;
        desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        desc.InstanceDataStepRate = 0;
        UINT64 byteStride;

        switch (accessor.type) {
        case TINYGLTF_TYPE_VEC2:
            desc.Format = DXGI_FORMAT_R32G32_FLOAT;
            byteStride = 4 * 2;
            break;
        case TINYGLTF_TYPE_VEC3:
            desc.Format = DXGI_FORMAT_R32G32B32_FLOAT;
            byteStride = 4 * 3;
            break;
        case TINYGLTF_TYPE_VEC4:
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            byteStride = 4 * 4;
            break;
        };

        // Accessors can be linked to the same bufferview, so here we keep
        // track of what input slot is linked to a bufferview.
        int bufferViewIdx = accessor.bufferView;
        auto bufferView = inputModel.bufferViews[bufferViewIdx];

        byteStride = bufferView.byteStride > 0 ? (UINT)bufferView.byteStride : byteStride;

        auto buffer = resourceBuffers[bufferView.buffer];
        UINT64 vertexStartOffset = bufferView.byteOffset + accessor.byteOffset - (accessor.byteOffset % byteStride);
        D3D12_GPU_VIRTUAL_ADDRESS vertexStartAddress = buffer->GetGPUVirtualAddress() + vertexStartOffset;

        desc.AlignedByteOffset = (UINT)accessor.byteOffset - vertexStartOffset + (UINT)bufferView.byteOffset;
        // No d3d buffer view attached to this range of vertices yet, add one
        if (!vertexStartOffsetToBufferView.contains(vertexStartAddress)) {
            D3D12_VERTEX_BUFFER_VIEW view;
            view.BufferLocation = vertexStartAddress;
            view.SizeInBytes = accessor.count * byteStride;
            view.StrideInBytes = (UINT)byteStride;

            if (view.BufferLocation + view.SizeInBytes > buffer->GetGPUVirtualAddress() + buffer->GetDesc().Width) {
                std::cout << "NO!!\n";
                std::cout << "Mesh " << inputMesh.name << "\n";
                std::cout << "Input element desc.AlignedByteOffset: " << desc.AlignedByteOffset << "\n";
                std::cout << "START ADDRESS: " << buffer->GetGPUVirtualAddress() << "\n";
                std::cout << "END ADDRESS: " << buffer->GetGPUVirtualAddress() + buffer->GetDesc().Width << "\n";
                DEBUG_VAR(byteStride);
                DEBUG_VAR(desc.AlignedByteOffset);
                DEBUG_VAR(accessor.byteOffset);
                DEBUG_VAR(accessor.count);
                DEBUG_VAR(view.BufferLocation);
                DEBUG_VAR(buffer->GetDesc().Width);
                DEBUG_VAR(vertexStartOffset);
                DEBUG_VAR(*semanticName);
                primitive = nullptr;
                return nullptr;
            }

            vertexBufferViews.push_back(view);
            vertexStartOffsetToBufferView[vertexStartAddress] = (UINT)vertexBufferViews.size() - 1;
        }
        desc.InputSlot = vertexStartOffsetToBufferView[vertexStartAddress];

        inputLayout.push_back(desc);

        D3D12_PRIMITIVE_TOPOLOGY& topology = primitive->primitiveTopology;
        switch (inputPrimitive.mode)
        {
        case TINYGLTF_MODE_POINTS:
            topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            break;
        case TINYGLTF_MODE_LINE:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case TINYGLTF_MODE_LINE_LOOP:
            std::cout << "Error: line loops are not supported";
            return nullptr;
        case TINYGLTF_MODE_LINE_STRIP:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case TINYGLTF_MODE_TRIANGLES:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        case TINYGLTF_MODE_TRIANGLE_STRIP:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case TINYGLTF_MODE_TRIANGLE_FAN:
            std::cout << "Error: triangle fans are not supported";
            return nullptr;
        };

    }

    if (inputPrimitive.material != -1) {
        auto& material = modelMaterials[inputPrimitive.material];
        primitive->material = material;
        if (material->materialType == MaterialType_PBR) {
            primitive->PSO = CreateMeshPBRPSO(
                app.psoManager,
                app.device.Get(),
                app.dataDir,
                app.rootSignature.Get(),
                primitive->inputLayout
            );
        } else if (material->materialType == MaterialType_AlphaBlendPBR) {
            primitive->PSO = CreateMeshAlphaBlendedPBRPSO(
                app.psoManager,
                app.device.Get(),
                app.dataDir,
                app.rootSignature.Get(),
                primitive->inputLayout
            );
        } else if (material->materialType == MaterialType_Unlit) {
            primitive->PSO = CreateMeshUnlitPSO(
                app.psoManager,
                app.device.Get(),
                app.dataDir,
                app.rootSignature.Get(),
                primitive->inputLayout
            );
        } else {
            // Unimplemented MaterialType
            abort();
        }
    } else {
        // Just pray this will work
        primitive->materialIndex = -1;
        primitive->PSO = CreateMeshUnlitPSO(
            app.psoManager,
            app.device.Get(),
            app.dataDir,
            app.rootSignature.Get(),
            primitive->inputLayout
        );
    }

    D3D12_INDEX_BUFFER_VIEW& ibv = primitive->indexBufferView;
    int accessorIdx = inputPrimitive.indices;
    auto& accessor = inputModel.accessors[accessorIdx];
    int indexBufferViewIdx = accessor.bufferView;
    auto bufferView = inputModel.bufferViews[indexBufferViewIdx];
    ibv.BufferLocation = resourceBuffers[bufferView.buffer]->GetGPUVirtualAddress() + bufferView.byteOffset + accessor.byteOffset;
    ibv.SizeInBytes = (UINT)(bufferView.byteLength - accessor.byteOffset);

    switch (accessor.componentType) {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        ibv.Format = DXGI_FORMAT_R8_UINT;
        std::cout << "GLTF mesh uses byte indices which aren't supported " << inputMesh.name;
        abort();
        break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        ibv.Format = DXGI_FORMAT_R16_UINT;
        break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        ibv.Format = DXGI_FORMAT_R32_UINT;
        break;
    };

    primitive->indexCount = (UINT)accessor.count;
    app.Stats.triangleCount += primitive->indexCount;

    return primitive;
}

void FinalizeModel(
    Model& outputModel,
    App& app,
    const tinygltf::Model& inputModel,
    const std::vector<SharedPoolItem<Material>>& modelMaterials
)
{
    const std::vector<ComPtr<ID3D12Resource>>& resourceBuffers = outputModel.buffers;

    int perPrimitiveDescriptorIdx = 0;

    for (const auto& inputMesh : inputModel.meshes) {
        outputModel.meshes.emplace_back(std::move(app.meshPool.AllocateUnique()));

        PoolItem<Mesh>& mesh = outputModel.meshes.back();
        mesh->name = inputMesh.name;
        std::vector<PoolItem<Primitive>>& primitives = mesh->primitives;

        for (int primitiveIdx = 0; primitiveIdx < inputMesh.primitives.size(); primitiveIdx++) {
            const auto& inputPrimitive = inputMesh.primitives[primitiveIdx];

            auto primitive = CreateModelPrimitive(
                app,
                outputModel,
                inputModel,
                inputMesh,
                inputPrimitive,
                modelMaterials,
                perPrimitiveDescriptorIdx
            );

            if (primitive != nullptr) {
                mesh->primitives.emplace_back(std::move(primitive));
                perPrimitiveDescriptorIdx++;
            }
        }
    }

    ResolveMeshTransforms(inputModel, outputModel.meshes);
}

// For the time being we cannot handle GLTF meshes without normals, tangents and UVs.
bool ValidateGLTFModel(const tinygltf::Model& model)
{
    for (const auto& mesh : model.meshes)
    {
        for (const auto& primitive : mesh.primitives)
        {
            const auto& attributes = primitive.attributes;
            bool hasNormals = primitive.attributes.contains("NORMAL");
            bool hasTangents = primitive.attributes.contains("TANGENT");
            bool hasTexcoords = primitive.attributes.contains("TEXCOORD") || primitive.attributes.contains("TEXCOORD_0");
            if (!hasNormals || !hasTangents || !hasTexcoords) {
                std::cout << "Model with mesh " << mesh.name << " is missing required vertex attributes and will be skipped\n";
                return false;
            }
        }
    }

    return true;
}

bool ValidateSkyboxAssets(const SkyboxAssets& assets)
{
    auto resourceDesc = GetImageResourceDesc(assets.images[0]);
    for (int i = 1; i < assets.images.size(); i++) {
        if (GetImageResourceDesc(assets.images[i]) != resourceDesc) {
            std::cout << "Error: all skybox images must have the same image format and dimensions\n";
            return false;
        }
    }

    return true;
}

template<class T>
Primitive CreatePrimitiveFromGeometryData(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    ComPtr<ID3D12Resource>& outGeometryBuffer,
    ComPtr<ID3D12Resource>& uploadHeap,
    const std::vector<T>& vertexData,
    const std::vector<unsigned int> indices,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout,
    ManagedPSORef PSO
)
{
    UINT64 requiredBufferSize = sizeof(T) * vertexData.size() + sizeof(unsigned int) * indices.size();
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredBufferSize);
    auto resourceState = D3D12_RESOURCE_STATE_COMMON;
    ASSERT_HRESULT(
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            resourceState,
            nullptr,
            IID_PPV_ARGS(&outGeometryBuffer)
        )
    );

    void* uploadHeapPtr;
    uploadHeap->Map(0, nullptr, uploadHeapPtr);
    auto vertexBlockSize = vertexData.size() * sizeof(T);
    auto indexBlockSize = sizeof(unsigned int) * indices.size();
    memcpy(uploadHeapPtr, (void*)vertexData.data(), vertexBlockSize);
    memcpy(uploadHeapPtr + vertexBlockSize, (void*)indices.data(), indexBlockSize);
    uploadHeap->Unmap(0, nullptr);

    commandList->CopyBufferRegion(outGeometryBuffer, 0, uploadHeap, 0, requiredBufferSize);

    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    vertexView.BufferLocation = outGeometryBuffer->GetGPUVirtualAddress();
    vertexView.SizeInBytes = (UINT)vertexBlockSize;
    vertexView.StrideInBytes = sizeof(T);

    D3D12_INDEX_BUFFER_VIEW indexView{};
    indexView.BufferLocation = outGeometryBuffer->GetGPUVirtualAddress() + vertexBlockSize;
    indexView.Format = DXGI_FORMAT_R32_UINT;
    indexView.SizeInBytes = indexBlockSize;

    Primitive primitive;
}

void RenderSkyboxDiffuseIrradianceMap(App& app, const SkyboxAssets& assets)
{
    ScopedPerformanceTracker perf(__func__, PerformancePrecision::Milliseconds);

    if (app.graphicsAnalysis) {
        app.graphicsAnalysis->BeginCapture();
    }

    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ASSERT_HRESULT(
        app.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&commandAllocator)
        )
    );

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ASSERT_HRESULT(
        app.device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            commandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&commandList)
        )
    );
    ASSERT_HRESULT(commandList->Close());

    // Create a new cubemap matching the skybox's cubemap resource.
    auto cubemapDesc = app.Skybox.cubemap->GetResource()->GetDesc();
    cubemapDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    ASSERT_HRESULT(
        app.mainAllocator->CreateResource(
            &allocDesc,
            &cubemapDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr,
            &app.Skybox.irradianceCubeMap,
            IID_NULL, nullptr
        )
    );

    auto rtv = app.rtvDescriptorArena.PushDescriptorStack(SkyboxImage_Count);

    for (int i = 0; i < SkyboxImage_Count; i++)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.FirstArraySlice = i;
        rtvDesc.Texture2DArray.ArraySize = 1;
        app.device->CreateRenderTargetView(app.Skybox.irradianceCubeMap->GetResource(), &rtvDesc, rtv.CPUHandle(i));
    }

    ManagedPSORef PSO = CreateSkyboxDiffuseIrradiancePSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        app.Skybox.inputLayout
    );

    // glm::mat4 projection = glm::ortho(0.0f, (float)cubemapDesc.Width, 0.0f, (float)cubemapDesc.Height);
    glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1.0f);
    glm::mat4 viewMatrices[] =
    {
        // Left/Right
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f,  0.0f,  0.0f), glm::vec3(0.0f,  -1.0f,  0.0f)),
        // Top/Bottom
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f,  0.0f), glm::vec3(0.0f,  0.0f,  1.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,  1.0f,  0.0f), glm::vec3(0.0f,  0.0f, -1.0f)),
        // Front/Back
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,  0.0f, -1.0f), glm::vec3(0.0f,  -1.0f,  0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,  0.0f,  1.0f), glm::vec3(0.0f,  -1.0f,  0.0f)),
    };

    Primitive* primitive = app.Skybox.mesh->primitives[0].get();

    for (int i = 0; i < SkyboxImage_Count; i++)
    {
        ASSERT_HRESULT(commandList->Reset(commandAllocator.Get(), PSO->Get()));

        primitive->constantData->MVP = projection * viewMatrices[i];
        primitive->constantData->MV = viewMatrices[i];

        DEBUG_VAR(primitive->constantData->MVP);
        DEBUG_VAR(viewMatrices[i]);

        CD3DX12_VIEWPORT viewport(0.0f, 0.0f, cubemapDesc.Width, cubemapDesc.Height);
        CD3DX12_RECT scissorRect(0, 0, static_cast<LONG>(cubemapDesc.Width), static_cast<LONG>(cubemapDesc.Height));

        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissorRect);

        ID3D12DescriptorHeap* ppHeaps[] = { app.descriptorArena.Heap() };
        commandList->SetDescriptorHeaps(1, ppHeaps);
        commandList->SetGraphicsRootSignature(app.rootSignature.Get());
        commandList->SetPipelineState(PSO->Get());

        UINT constantValues[5] = {
            primitive->perPrimitiveDescriptor.index,
            -1,
            -1,
            -1,
            app.Skybox.texcubeSRV.index,
        };
        commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);

        auto rtvHandle = rtv.CPUHandle(i);
        commandList->OMSetRenderTargets(1, &rtvHandle, false, nullptr);

        commandList->IASetPrimitiveTopology(primitive->primitiveTopology);
        commandList->IASetVertexBuffers(0, (UINT)primitive->vertexBufferViews.size(), primitive->vertexBufferViews.data());
        commandList->IASetIndexBuffer(&primitive->indexBufferView);
        commandList->DrawIndexedInstanced(primitive->indexCount, 1, 0, 0, 0);

        if (i == SkyboxImage_Count - 1) {
            // On the last cubemap face we can transition the cubemap to being a shader resource view.
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.Skybox.irradianceCubeMap->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &barrier);
        }

        // FIXME: It would be better to not block here, and create commands for every face at once.
        // However that would require creating constant buffers for each skybox face.
        commandList->Close();
        ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
        app.commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
        app.fence.SignalAndWait(app.commandQueue.Get());
    }

    app.rtvDescriptorArena.PopDescriptorStack(SkyboxImage_Count);


    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = cubemapDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;

    app.Skybox.irradianceCubeSRV = app.descriptorArena.AllocateDescriptors(1, "Diffuse Irradiance Cubemap SRV");
    app.device->CreateShaderResourceView(
        app.Skybox.irradianceCubeMap->GetResource(),
        &srvDesc,
        app.Skybox.irradianceCubeSRV.CPUHandle()
    );

    if (app.graphicsAnalysis) {
        app.graphicsAnalysis->EndCapture();
    }
}

void CreateSkybox(App& app, const SkyboxAssets& asset)
{
    if (!ValidateSkyboxAssets(asset)) {
        return;
    }

    ComPtr<D3D12MA::Allocation> cubemap;
    ComPtr<D3D12MA::Allocation> vertexBuffer;
    ComPtr<D3D12MA::Allocation> indexBuffer;
    ComPtr<D3D12MA::Allocation> perPrimitiveBuffer;

    auto cubemapDesc = GetImageResourceDesc(asset.images[0]);
    cubemapDesc.MipLevels = 1;
    cubemapDesc.DepthOrArraySize = SkyboxImage_Count;
    {
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &cubemapDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &cubemap,
                IID_NULL, nullptr
            )
        );
    }

    {
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(PerPrimitiveConstantData));

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &perPrimitiveBuffer,
                IID_NULL, nullptr
            )
        );
    }

    DescriptorRef perPrimitiveCbv = app.descriptorArena.AllocateDescriptors(1, "Skybox PerPrimitive CBV");
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = perPrimitiveBuffer->GetResource()->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(PerPrimitiveConstantData);
    app.device->CreateConstantBufferView(&cbvDesc, perPrimitiveCbv.CPUHandle());

    D3D12_SUBRESOURCE_DATA cubemapSubresourceData[SkyboxImage_Count] = {};
    for (int i = 0; i < SkyboxImage_Count; i++) {
        cubemapSubresourceData[i].pData = asset.images[i].image.data();
        cubemapSubresourceData[i].RowPitch = asset.images[i].width * asset.images[i].component;
        cubemapSubresourceData[i].SlicePitch = asset.images[i].height * cubemapSubresourceData[i].RowPitch;
    }

    UploadBatch uploadBatch;
    uploadBatch.Begin(app.mainAllocator.Get(), app.copyCommandQueue.Get(), &app.copyFence);
    uploadBatch.AddTexture(cubemap->GetResource(), cubemapSubresourceData, 0, SkyboxImage_Count);

    DescriptorRef texcubeSRV = app.descriptorArena.AllocateDescriptors(1, "Skybox SRV");

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = cubemapDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    app.device->CreateShaderResourceView(
        cubemap->GetResource(),
        &srvDesc,
        texcubeSRV.CPUHandle()
    );

    app.Skybox.cubemap = cubemap;
    app.Skybox.texcubeSRV = texcubeSRV;

    app.Skybox.inputLayout = {
        {
            "POSITION",                                 // SemanticName
            0,                                          // SemanticIndex
            DXGI_FORMAT_R32G32B32_FLOAT,                // Format
            0,                                          // InputSlot
            D3D12_APPEND_ALIGNED_ELEMENT,               // AlignedByteOffset
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, // InputSlotClass
            0                                           // InstanceDataStepRate
        },
    };

    float vertexData[] = {
        // front
        -1.0, -1.0,  1.0,
         1.0, -1.0,  1.0,
         1.0,  1.0,  1.0,
        -1.0,  1.0,  1.0,
        // back
        -1.0, -1.0, -1.0,
         1.0, -1.0, -1.0,
         1.0,  1.0, -1.0,
        -1.0,  1.0, -1.0
    };

    unsigned short indices[] =
    {
        // front
        0, 1, 2,
        2, 3, 0,
        // right
        1, 5, 6,
        6, 2, 1,
        // back
        7, 6, 5,
        5, 4, 7,
        // left
        4, 0, 3,
        3, 7, 4,
        // bottom
        4, 5, 1,
        1, 0, 4,
        // top
        3, 2, 6,
        6, 7, 3
    };

    {
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(vertexData));
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &vertexBuffer,
                IID_NULL, nullptr
            )
        );
    }

    {
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(indices));
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &indexBuffer,
                IID_NULL, nullptr
            )
        );
    }

    uploadBatch.AddBuffer(vertexBuffer->GetResource(), 0, vertexData, sizeof(vertexData));
    uploadBatch.AddBuffer(indexBuffer->GetResource(), 0, reinterpret_cast<void*>(indices), sizeof(indices));

    uploadBatch.Finish();

    PoolItem<Primitive> primitive = app.primitivePool.AllocateUnique();
    primitive->indexBufferView.BufferLocation = indexBuffer->GetResource()->GetGPUVirtualAddress();
    primitive->indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    primitive->indexBufferView.SizeInBytes = sizeof(indices);

    D3D12_VERTEX_BUFFER_VIEW vertexView;
    vertexView.BufferLocation = vertexBuffer->GetResource()->GetGPUVirtualAddress();
    vertexView.SizeInBytes = sizeof(vertexData);
    vertexView.StrideInBytes = sizeof(glm::vec3);
    primitive->vertexBufferViews.push_back(vertexView);

    primitive->PSO = CreateSkyboxPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        app.Skybox.inputLayout
    );

    primitive->perPrimitiveDescriptor = perPrimitiveCbv;

    primitive->primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    primitive->indexCount = _countof(indices);

    primitive->miscDescriptorParameter = texcubeSRV;

    perPrimitiveBuffer->GetResource()->Map(0, nullptr, reinterpret_cast<void**>(&primitive->constantData));

    app.Skybox.mesh = app.meshPool.AllocateUnique();
    app.Skybox.mesh->primitives.emplace_back(std::move(primitive));
    app.Skybox.mesh->baseModelTransform = glm::scale(glm::mat4(1.0f), glm::vec3(50.0f));
    app.Skybox.mesh->name = "Skybox";

    app.Skybox.cubemap = cubemap;
    app.Skybox.indexBuffer = indexBuffer;
    app.Skybox.vertexBuffer = vertexBuffer;
    app.Skybox.perPrimitiveConstantBuffer = perPrimitiveBuffer;

    RenderSkyboxDiffuseIrradianceMap(app, asset);

    app.Skybox.mesh->primitives[0]->miscDescriptorParameter = app.Skybox.texcubeSRV;
}

void ProcessAssets(App& app, AssetBundle& assets)
{
    ASSERT_HRESULT(app.commandList->Close());

    ASSERT_HRESULT(
        app.commandAllocator->Reset()
    );

    ASSERT_HRESULT(
        app.commandList->Reset(app.commandAllocator.Get(), app.pipelineState.Get())
    );

    // TODO: easy candidate for multithreading
    for (auto& gltfModel : assets.models) {
        if (!ValidateGLTFModel(gltfModel)) {
            continue;
        }

        std::vector<UINT64> uploadOffsets;

        std::span<ComPtr<ID3D12Resource>> geometryBuffers;
        std::span<ComPtr<ID3D12Resource>> textureBuffers;

        Model model;

        // Can only call this ONCE before command list executed
        // This will need to be adapted to handle N models.
        auto resourceBuffers = UploadModelBuffers(
            model,
            app,
            gltfModel,
            uploadOffsets,
            geometryBuffers,
            textureBuffers
        );

        std::vector<SharedPoolItem<Material>> modelMaterials;

        CreateModelDescriptors(app, gltfModel, model, textureBuffers);
        CreateModelMaterials(app, gltfModel, model, modelMaterials);
        FinalizeModel(model, app, gltfModel, modelMaterials);

        app.models.push_back(std::move(model));

        ASSERT_HRESULT(app.commandList->Close());

        ID3D12CommandList* ppCommandLists[] = { app.commandList.Get() };
        app.commandQueue->ExecuteCommandLists(1, ppCommandLists);
        WaitForPreviousFrame(app);

        ASSERT_HRESULT(
            app.commandAllocator->Reset()
        );

        ASSERT_HRESULT(
            app.commandList->Reset(app.commandAllocator.Get(), app.pipelineState.Get())
        );
    }

    if (assets.skybox.has_value()) {
        CreateSkybox(app, assets.skybox.value());
    }

    ASSERT_HRESULT(
        app.commandList->Close()
    );
}

void InitializeCamera(App& app)
{
    app.camera.translation = glm::vec3(0.0f, 0.5f, 3.0f);
    app.camera.pitch = 0.0f;
    app.camera.yaw = glm::pi<float>();
}

void InitializeLights(App& app)
{
    app.LightBuffer.count = 1;

    app.lights[0].lightType = LightType_Directional;
    app.lights[0].color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    app.lights[0].intensity = 1.5f;
    app.lights[0].position = glm::vec3(0.0f);
    app.lights[0].direction = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));
    app.lights[0].range = 5.0f;

    for (int i = 1; i < MaxLightCount; i++) {
        float angle = i * glm::two_pi<float>() / 4;
        float x = cos(angle);
        float z = sin(angle);
        app.lights[i].color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        app.lights[i].intensity = 8.0f;
        app.lights[i].position = glm::vec3(x, 2.0f, z);
        app.lights[i].direction = glm::vec3(0.0f, 0.0f, 0.0f);
        app.lights[i].range = 5.0f;
        app.lights[i].lightType = LightType_Point;
    }
}

void InitializeScene(App& app) {
    InitializeCamera(app);
    InitializeLights(app);
    app.models[1].meshes[0]->translation = glm::vec3(0, 0.5f, 0.0f);
}

glm::mat4 ApplyStandardTransforms(const glm::mat4& base, glm::vec3 translation, glm::vec3 euler, glm::vec3 scale)
{
    glm::mat4 transform = base;
    transform = glm::translate(transform, translation);
    transform = glm::scale(transform, scale);
    transform = transform * glm::eulerAngleXYZ(euler.x, euler.y, euler.z);

    return transform;
}

void UpdatePerPrimitiveConstantBuffers(App& app, const glm::mat4& projection, const glm::mat4& view)
{
    glm::mat4 viewProjection = projection * view;

    auto meshIter = app.meshPool.Begin();

    while (meshIter) {
        auto& mesh = meshIter.item;

        auto modelMatrix = ApplyStandardTransforms(
            mesh->baseModelTransform,
            mesh->translation,
            mesh->euler,
            mesh->scale
        );

        auto mvp = viewProjection * modelMatrix;
        auto mv = view * modelMatrix;
        for (const auto& primitive : mesh->primitives) {
            primitive->constantData->MVP = mvp;
            primitive->constantData->MV = mv;
        }
        meshIter = app.meshPool.Next(meshIter);
    }
}

void UpdateLightConstantBuffers(App& app, const glm::mat4& projection, const glm::mat4& view)
{
    app.LightBuffer.passData->inverseProjectionMatrix = glm::inverse(projection);
    app.LightBuffer.passData->inverseViewMatrix = glm::inverse(view);

    for (int i = 0; i < app.LightBuffer.count; i++) {
        app.lights[i].UpdateConstantData(view);
    }
}

// Updates the camera to behave like a flying camera.
// WASD moves laterally and Q and E lower and raise the camera.
// Only updates when the user is pressing the right mouse button.
glm::mat4 UpdateFlyCamera(App& app, float deltaSeconds)
{
    glm::vec3 cameraMovement(0.0f);

    if (!app.camera.locked) {
        app.camera.targetSpeed += app.mouseState.scrollDelta * 1.0f;
        app.camera.targetSpeed = glm::clamp(app.camera.targetSpeed, app.camera.minSpeed, app.camera.maxSpeed);
        const float RADIANS_PER_PIXEL = glm::radians(0.1f);
        app.camera.yaw -= (float)app.mouseState.xrel * RADIANS_PER_PIXEL;
        app.camera.pitch -= (float)app.mouseState.yrel * RADIANS_PER_PIXEL;
        app.camera.pitch = glm::clamp(app.camera.pitch, -app.camera.maxPitch, app.camera.maxPitch);
    }

    float yaw = app.camera.yaw;
    float pitch = app.camera.pitch;

    glm::vec3 cameraForward;
    cameraForward.z = cos(yaw) * cos(pitch);
    cameraForward.y = sin(pitch);
    cameraForward.x = sin(yaw) * cos(pitch);
    cameraForward = glm::normalize(cameraForward);

    float right = app.keyState[SDL_SCANCODE_D] ? 1.0f : 0.0f;
    float left = app.keyState[SDL_SCANCODE_A] ? -1.0f : 0.0f;
    float forward = app.keyState[SDL_SCANCODE_W] ? 1.0f : 0.0f;
    float backward = app.keyState[SDL_SCANCODE_S] ? -1.0f : 0.0f;
    float up = app.keyState[SDL_SCANCODE_E] ? 1.0f : 0.0f;
    float down = app.keyState[SDL_SCANCODE_Q] ? -1.0f : 0.0f;

    glm::vec3 inputVector = glm::vec3(right + left, up + down, forward + backward);


    glm::vec3 upVector = glm::vec3(0.0f, 1.0f, 0.0f);

    float speed = app.camera.targetSpeed * deltaSeconds;

    if (!app.camera.locked) {
        glm::vec3 vecUp(0.0f, 1.0f, 0.0f);
        cameraMovement += inputVector.z * cameraForward;
        cameraMovement += inputVector.x * glm::normalize(glm::cross(cameraForward, vecUp));
        cameraMovement.y += inputVector.y;
        cameraMovement *= speed;
        app.camera.translation += cameraMovement;
    }

    return glm::lookAt(app.camera.translation, app.camera.translation + cameraForward, glm::vec3(0, 1, 0));
}

void UpdateScene(App& app)
{
    long long currentTick = steady_clock::now().time_since_epoch().count();
    long long ticksSinceStart = currentTick - app.startTick;
    float timeSeconds = (float)ticksSinceStart / (float)1e9;
    long long deltaTicks = currentTick - app.lastFrameTick;
    float deltaSeconds = (float)deltaTicks / (float)1e9;

    glm::mat4 projection = glm::perspective(app.camera.fovY, (float)app.windowWidth / (float)app.windowHeight, 0.1f, 1000.0f);
    glm::mat4 view = UpdateFlyCamera(app, deltaSeconds);

    static std::once_flag onceFlag;
    std::call_once(onceFlag, [view] {DEBUG_VAR(view)});

    if (app.Skybox.mesh) {
        app.Skybox.mesh->translation = app.camera.translation;
    }

    UpdateLightConstantBuffers(app, projection, view);
    UpdatePerPrimitiveConstantBuffers(app, projection, view);
}

void DrawMeshesGBuffer(App& app, ID3D12GraphicsCommandList* commandList)
{
    auto meshIter = app.meshPool.Begin();

    while (meshIter) {
        for (const auto& primitive : meshIter->primitives) {
            const auto& material = primitive->material.get();
            // FIXME: if I am not lazy I will SORT by material type
            // Transparent materials drawn in different pass
            DescriptorRef materialDescriptor;

            if (material) {
                if (material->materialType == MaterialType_AlphaBlendPBR) {
                    continue;
                }
                materialDescriptor = material->cbvDescriptor;
            }

            // Set the per-primitive constant buffer
            UINT constantValues[5] = { primitive->perPrimitiveDescriptor.index, materialDescriptor.index, 0, 0, primitive->miscDescriptorParameter.index };
            commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
            commandList->IASetPrimitiveTopology(primitive->primitiveTopology);
            commandList->SetPipelineState(primitive->PSO->Get());
            commandList->IASetVertexBuffers(0, (UINT)primitive->vertexBufferViews.size(), primitive->vertexBufferViews.data());
            commandList->IASetIndexBuffer(&primitive->indexBufferView);
            commandList->DrawIndexedInstanced(primitive->indexCount, 1, 0, 0, 0);

            app.Stats.drawCalls++;
        }
        meshIter = app.meshPool.Next(meshIter);
    }
}

void DrawAlphaBlendedMeshes(App& app, ID3D12GraphicsCommandList* commandList)
{
    auto meshIter = app.meshPool.Begin();
    // FIXME: This seems like a bad looping order..
    while (meshIter) {
        auto& mesh = meshIter.item;
        for (const auto& primitive : mesh->primitives) {
            auto material = primitive->material.get();
            // FIXME: if I am not lazy I will SORT by material type
            // Only draw alpha blended materials in this pass.
            if (!material || material->materialType != MaterialType_AlphaBlendPBR) {
                continue;
            }
            auto materialDescriptor = material->cbvDescriptor;

            for (int lightIdx = 0; lightIdx < app.LightBuffer.count; lightIdx++) {
                int lightDescriptorIndex = app.LightBuffer.cbvHandle.index + lightIdx + 1;
                // Set the per-primitive constant buffer
                UINT constantValues[5] = { primitive->perPrimitiveDescriptor.index, materialDescriptor.index, lightDescriptorIndex, 0, primitive->miscDescriptorParameter.index };
                commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
                commandList->IASetPrimitiveTopology(primitive->primitiveTopology);
                commandList->SetPipelineState(primitive->PSO->Get());
                commandList->IASetVertexBuffers(0, (UINT)primitive->vertexBufferViews.size(), primitive->vertexBufferViews.data());
                commandList->IASetIndexBuffer(&primitive->indexBufferView);
                commandList->DrawIndexedInstanced(primitive->indexCount, 1, 0, 0, 0);

                app.Stats.drawCalls++;
            }
        }
        meshIter = app.meshPool.Next(meshIter);
    }
}

void DrawFullscreenQuad(App& app, ID3D12GraphicsCommandList* commandList)
{
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->DrawInstanced(4, 1, 0, 0);

    app.Stats.drawCalls++;
}

void BindAndClearGBufferRTVs(const App& app, ID3D12GraphicsCommandList* commandList)
{
    // For GBuffer pass, we bind the back buffer handle, followed by all the gbuffer handles.
    // Unlit/ambient data will be written directly to backbuffer, while all the PBR stuff will
    // be written to the GBuffer as normal.
    CD3DX12_CPU_DESCRIPTOR_HANDLE renderTargetHandles[1 + GBuffer_RTVCount] = {};
    auto backBufferHandle = app.frameBufferRTVs[app.frameIdx].CPUHandle();

    renderTargetHandles[0] = backBufferHandle;
    for (int i = 0; i < GBuffer_RTVCount; i++) {
        renderTargetHandles[i + 1] = app.GBuffer.rtvs[i].CPUHandle();
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(app.dsHeap->GetCPUDescriptorHandleForHeapStart());
    commandList->OMSetRenderTargets(_countof(renderTargetHandles), renderTargetHandles, FALSE, &dsvHandle);

    // Clear render targets
    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (const auto& handle : renderTargetHandles) {
        commandList->ClearRenderTargetView(handle, clearColor, 0, nullptr);
    }
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void TransitionRTVsForRendering(const App& app, ID3D12GraphicsCommandList* commandList)
{
    // Transition our GBuffers into being render targets.
    std::array<D3D12_RESOURCE_BARRIER, GBuffer_Count> barriers;
    // Back buffer
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(app.renderTargets[app.frameIdx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // GBuffer RTVs
    for (int i = 0; i < GBuffer_RTVCount; i++) {
        barriers[i + 1] = CD3DX12_RESOURCE_BARRIER::Transition(app.GBuffer.renderTargets[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    // Depth buffer is already in the correct state from AlphaBlendPass
    commandList->ResourceBarrier((UINT)barriers.size(), barriers.data());
}

void TransitionGBufferForLighting(const App& app, ID3D12GraphicsCommandList* commandList)
{
    // Transition them back to being shader resource views so that they can be used in the lighting shaders.
    std::array<D3D12_RESOURCE_BARRIER, GBuffer_Count> barriers;
    for (int i = 0; i < GBuffer_RTVCount; i++) {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(app.GBuffer.renderTargets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    // Depth buffer is special
    barriers[GBuffer_Depth] = CD3DX12_RESOURCE_BARRIER::Transition(app.depthStencilBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier((UINT)barriers.size(), barriers.data());
}

void GBufferPass(App& app, ID3D12GraphicsCommandList* commandList)
{
    TransitionRTVsForRendering(app, commandList);

    BindAndClearGBufferRTVs(app, commandList);

    ID3D12DescriptorHeap* mainDescriptorHeap = app.descriptorArena.Heap();
    ID3D12DescriptorHeap* ppHeaps[] = { mainDescriptorHeap };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    commandList->SetGraphicsRootSignature(app.rootSignature.Get());

    DrawMeshesGBuffer(app, commandList);
}

void LightPass(App& app, ID3D12GraphicsCommandList* commandList)
{
    TransitionGBufferForLighting(app, commandList);

    auto rtvHandle = app.frameBufferRTVs[app.frameIdx].CPUHandle();
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    {
        auto descriptorHeap = app.descriptorArena.descriptorHeap;
        ID3D12DescriptorHeap* ppHeaps[] = { descriptorHeap.Get() };
        commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
        // Root signature must be set AFTER heaps are set when CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED is set.
        commandList->SetGraphicsRootSignature(app.rootSignature.Get());

        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        // Point lights
        commandList->SetPipelineState(app.LightPass.pointLightPSO->Get());
        for (int i = 0; i < app.LightBuffer.count; i++) {
            if (app.lights[i].lightType == LightType_Point) {
                UINT constantValues[2] = {
                    app.LightBuffer.cbvHandle.index + i + 1,
                    app.LightBuffer.cbvHandle.index
                };
                commandList->SetGraphicsRoot32BitConstants(0, 2, constantValues, 2);
                DrawFullscreenQuad(app, commandList);
            }
        }

        // Directional lights
        commandList->SetPipelineState(app.LightPass.directionalLightPso->Get());
        for (int i = 0; i < app.LightBuffer.count; i++) {
            if (app.lights[i].lightType == LightType_Directional) {
                UINT constantValues[2] = {
                    app.LightBuffer.cbvHandle.index + i + 1,
                    app.LightBuffer.cbvHandle.index
                };
                commandList->SetGraphicsRoot32BitConstants(0, 2, constantValues, 2);
                DrawFullscreenQuad(app, commandList);
            }
        }

        // Environment cubemap
        commandList->SetPipelineState(app.LightPass.environentCubemapLightPso->Get());
        UINT constantValues[5] = {
            -1,
            -1,
            -1,
            app.LightBuffer.cbvHandle.index,
            app.Skybox.irradianceCubeSRV.index,
        };
        commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
        DrawFullscreenQuad(app, commandList);
    }
}

// Forward pass for meshes with transparency
void AlphaBlendPass(App& app, ID3D12GraphicsCommandList* commandList)
{
    // Transition depth buffer back to depth write for alpha blend
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.depthStencilBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commandList->ResourceBarrier(1, &barrier);

    auto rtvHandle = app.frameBufferRTVs[app.frameIdx].CPUHandle();
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(app.dsHeap->GetCPUDescriptorHandleForHeapStart());
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    ID3D12DescriptorHeap* mainDescriptorHeap = app.descriptorArena.Heap();
    ID3D12DescriptorHeap* ppHeaps[] = { mainDescriptorHeap };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    commandList->SetGraphicsRootSignature(app.rootSignature.Get());

    DrawAlphaBlendedMeshes(app, commandList);
}

void BuildCommandList(App& app)
{
    ID3D12GraphicsCommandList* commandList = app.commandList.Get();

    ASSERT_HRESULT(
        app.commandAllocator->Reset()
    );

    ASSERT_HRESULT(
        commandList->Reset(app.commandAllocator.Get(), app.pipelineState.Get())
    );

    commandList->SetGraphicsRootSignature(app.rootSignature.Get());

    commandList->RSSetViewports(1, &app.viewport);
    commandList->RSSetScissorRects(1, &app.scissorRect);

    GBufferPass(app, commandList);
    LightPass(app, commandList);
    AlphaBlendPass(app, commandList);

    ID3D12DescriptorHeap* ppHeaps[] = { app.ImGui.srvHeap.Get() };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);

    // Indicate that the back buffer will now be used to present.
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.renderTargets[app.frameIdx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    commandList->ResourceBarrier(1, &barrier);

    ASSERT_HRESULT(
        commandList->Close()
    );
}

void RenderFrame(App& app)
{
    app.Stats.drawCalls = 0;

    bool tdrOccurred = false;
    BuildCommandList(app);

    ID3D12CommandList* ppCommandLists[] = { app.commandList.Get() };
    app.commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);


    HRESULT hr = app.swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    if (!SUCCEEDED(hr)) {
        tdrOccurred = true;
        app.running = false;
        std::cout << "TDR occurred\n";
    }

    WaitForPreviousFrame(app);
}

void HandleResize(App& app, int newWidth, int newHeight)
{
    // Release references to the buffers before resizing.
    for (auto& renderTarget : app.renderTargets) {
        renderTarget = nullptr;
    }

    app.swapChain->ResizeBuffers(2, newWidth, newHeight, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    app.viewport.Width = (float)newWidth;
    app.viewport.Height = (float)newHeight;
    app.windowWidth = newWidth;
    app.windowHeight = newHeight;
    app.scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(newWidth), static_cast<LONG>(newHeight));

    SetupRenderTargets(app);
    SetupDepthStencil(app, true);
    SetupGBuffer(app, true);

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();
}

void InitImGui(App& app)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 1;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ASSERT_HRESULT(app.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&app.ImGui.srvHeap)));
    }

    CHECK(
        ImGui_ImplSDL2_InitForD3D(app.window)
    );

    CHECK(
        ImGui_ImplDX12_Init(
            app.device.Get(),
            FrameBufferCount,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            app.ImGui.srvHeap.Get(),
            app.ImGui.srvHeap->GetCPUDescriptorHandleForHeapStart(),
            app.ImGui.srvHeap->GetGPUDescriptorHandleForHeapStart()
        )
    );
}

void CleanImGui()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void DrawMeshEditor(App& app)
{
    static int selectedMeshIdx = -1;
    if (!app.ImGui.meshesOpen) {
        return;
    }
    if (ImGui::Begin("Mesh Editor", &app.ImGui.meshesOpen, 0)) {
        Mesh* selectedMesh = nullptr;

        if (ImGui::BeginListBox("Meshes")) {
            int meshIdx = 0;
            for (auto& model : app.models) {
                for (auto& mesh : model.meshes) {
                    std::string label = mesh->name;
                    bool isSelected = meshIdx == selectedMeshIdx;

                    if (isSelected) {
                        selectedMesh = mesh.get();
                    }

                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        selectedMeshIdx = meshIdx;
                        break;
                    }

                    meshIdx++;
                }
            }
            ImGui::EndListBox();
        }

        ImGui::Separator();

        if (selectedMesh != nullptr) {
            glm::vec3 eulerDegrees = glm::degrees(selectedMesh->euler);

            ImGui::DragFloat3("Position", (float*)&selectedMesh->translation, 0.1);
            ImGui::DragFloat3("Euler", (float*)&eulerDegrees, 0.1);
            ImGui::DragFloat3("Scale", (float*)&selectedMesh->scale, 0.1);

            selectedMesh->euler = glm::radians(eulerDegrees);
        }
    }
    ImGui::End();
}

void DrawMaterialEditor(App& app)
{
    if (!app.ImGui.materialsOpen) {
        return;
    }

    static int selectedMaterialIdx = -1;

    if (ImGui::Begin("Material Editor", &app.ImGui.materialsOpen, 0)) {
        Material* selectedMaterial = nullptr;

        if (ImGui::BeginListBox("Materials")) {
            int materialIdx = 0;
            auto materialIter = app.materials.Begin();

            while (materialIter) {
                bool isSelected = materialIdx == selectedMaterialIdx;

                if (isSelected) {
                    selectedMaterial = materialIter.item;
                }

                if (ImGui::Selectable(materialIter->name.c_str(), isSelected)) {
                    selectedMaterialIdx = materialIdx;
                    break;
                }

                materialIdx++;
                materialIter = app.materials.Next(materialIter);
            }
            ImGui::EndListBox();
        }

        ImGui::Separator();

        if (selectedMaterial != nullptr) {
            bool materialDirty = false;
            if (ImGui::ColorEdit4("Base Color Factor", &selectedMaterial->baseColorFactor[0])) {
                materialDirty = true;
            }

            if (ImGui::SliderFloat("Roughness", &selectedMaterial->metalRoughnessFactor.g, 0.0f, 1.0f)) {
                materialDirty = true;
            }

            if (ImGui::SliderFloat("Metallic", &selectedMaterial->metalRoughnessFactor.b, 0.0f, 1.0f)) {
                materialDirty = true;
            }

            if (materialDirty) {
                selectedMaterial->UpdateConstantData();
            }
        }
    }

    ImGui::End();
}

void DrawLightEditor(App& app)
{
    static int selectedLightIdx = 0;

    if (!app.ImGui.lightsOpen) {
        return;
    }

    if (ImGui::Begin("Lights", &app.ImGui.lightsOpen, 0)) {
        // const char* const* pLabels = (const char* const*)labels;
        if (ImGui::BeginListBox("Lights")) {
            for (int i = 0; i < app.LightBuffer.count; i++) {
                std::string label = "Light #" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), i == selectedLightIdx))
                {
                    selectedLightIdx = i;
                    break;
                }
            }
            ImGui::EndListBox();
        }

        if (ImGui::Button("New light")) {
            app.LightBuffer.count = glm::min(app.LightBuffer.count + 1, MaxLightCount);
            selectedLightIdx = app.LightBuffer.count - 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove light")) {
            app.LightBuffer.count = glm::max(app.LightBuffer.count - 1, 0);
            if (selectedLightIdx == app.LightBuffer.count) {
                selectedLightIdx--;
            }
        }

        ImGui::Separator();
        if (selectedLightIdx != -1) {
            auto& light = app.lights[selectedLightIdx];

            static const char* LightTypeLabels[] = {
                "Point",
                "Directional"
            };

            if (ImGui::BeginCombo("Light Type", LightTypeLabels[light.lightType])) {
                for (int i = 0; i < _countof(LightTypeLabels); i++) {
                    if (ImGui::Selectable(LightTypeLabels[i], i == light.lightType)) {
                        light.lightType = (LightType)i;
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::ColorEdit3("Color", (float*)&light.color, ImGuiColorEditFlags_PickerHueWheel);
            if (light.lightType == LightType_Point) {
                ImGui::DragFloat3("Position", (float*)&light.position, 0.1);
            }
            if (light.lightType == LightType_Directional) {
                ImGui::DragFloat3("Direction", (float*)&light.direction, 0.1);
            }
            ImGui::DragFloat("Range", &light.range, 0.1f, 0.0f, 1000.0f, nullptr, 1.0f);
            ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f, nullptr, 1.0f);
        } else {
            ImGui::Text("No light selected");
        }

        ImGui::Separator();
        ImGui::DragFloat3("Environment Intensity", &app.LightBuffer.passData->environmentIntensity[0]);
    }

    ImGui::End();
}

void DrawStats(App& app)
{
    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoMove;

    if (app.ImGui.showStats) {
        float frameTimeMS = (float)app.Stats.lastFrameTimeNS / (float)1E6;
        if (ImGui::Begin("Stats", &app.ImGui.showStats, windowFlags)) {
            ImGui::SetWindowPos({ 0.0f, 20.0f });
            ImGui::Text("Frame time: %.2fms", frameTimeMS);
            ImGui::Text("FPS: %.0f", 1000.0f / frameTimeMS);
            ImGui::Text("Triangles: %d", app.Stats.triangleCount);
            ImGui::Text("Draw calls: %d", app.Stats.drawCalls);
        }
        ImGui::End();
    }
}

void DrawMenuBar(App& app)
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Add GLTF")) {
                const char* filters[] = { "*.gltf" };
                char* gltfFile = tinyfd_openFileDialog(
                    "Choose GLTF File",
                    app.dataDir.c_str(),
                    _countof(filters),
                    filters,
                    NULL,
                    0
                );

                if (gltfFile != nullptr) {
                    AssetBundle assets;
                    tinygltf::TinyGLTF loader;
                    LoadModelAsset(app, assets, loader, gltfFile);
                    ProcessAssets(app, assets);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Reload Shaders")) {
                app.psoManager.reload(app.device.Get());
            }
            if (ImGui::MenuItem("Reset Camera")) {
                InitializeCamera(app);
            }
            if (ImGui::Checkbox("Shader Debug Flag", (bool*)&app.LightBuffer.passData->debug)) {}
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            ImGui::Checkbox("Lights", &app.ImGui.lightsOpen);
            ImGui::Checkbox("Meshes", &app.ImGui.meshesOpen);
            ImGui::Checkbox("Materials", &app.ImGui.materialsOpen);
            ImGui::Checkbox("Geek Menu", &app.ImGui.geekOpen);
            ImGui::Checkbox("Show stats", &app.ImGui.showStats);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void DrawGeekMenu(App& app)
{
    if (!app.ImGui.geekOpen) {
        return;
    }

    if (ImGui::Begin("Geek Menu", &app.ImGui.geekOpen)) {
        static bool debugSkybox = false;
        if (ImGui::Checkbox("Debug Diffuse IBL", &debugSkybox)) {
            app.Skybox.mesh->primitives[0]->miscDescriptorParameter =
                debugSkybox ? app.Skybox.irradianceCubeSRV : app.Skybox.texcubeSRV;
        }

        float degreesFOV = glm::degrees(app.camera.fovY);
        ImGui::DragFloat("Camera FOVy Degrees", &degreesFOV, 0.05f, 0.01f, 180.0f);
        app.camera.fovY = glm::radians(degreesFOV);
    }

    ImGui::End();
}

void BeginGUI(App& app)
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    DrawLightEditor(app);
    DrawMaterialEditor(app);
    DrawMeshEditor(app);
    DrawStats(app);
    DrawMenuBar(app);
    DrawGeekMenu(app);
}

void ReloadIfShaderChanged(App& app)
{
    auto status = WaitForSingleObject(app.shaderWatchHandle, 0);
    if (status == WAIT_OBJECT_0) {
        std::cout << "Data directory changed. Reloading shaders.\n";
        app.psoManager.reload(app.device.Get());
        CreateDataDirWatchHandle(app);
    }
}

void ToggleBorderlessWindow(App& app)
{
    int flags = !app.borderlessFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    SDL_SetWindowFullscreen(app.window, flags);

    int width, height;
    SDL_GetWindowSize(app.window, &width, &height);
    HandleResize(app, width, height);


    app.borderlessFullscreen = !app.borderlessFullscreen;
}

int RunApp(int argc, char** argv)
{
    App app;

    InitApp(app, argc, argv);
    InitWindow(app);
    InitD3D(app);
    InitImGui(app);

    {
        AssetBundle assets = LoadAssets(app);
        ProcessAssets(app, assets);
    }

    InitializeScene(app);

    SDL_Event e;

    app.startTick = steady_clock::now().time_since_epoch().count();
    app.lastFrameTick = app.startTick;

    int mouseX, mouseY;
    int buttonState = SDL_GetMouseState(&mouseX, &mouseY);

    app.running = true;
    while (app.running) {
        long long frameTick = steady_clock::now().time_since_epoch().count();
        app.mouseState.xrel = 0;
        app.mouseState.yrel = 0;
        app.mouseState.scrollDelta = 0;
        while (SDL_PollEvent(&e) > 0) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                app.running = false;
            } else if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                    int newWidth = e.window.data1;
                    int newHeight = e.window.data2;
                    HandleResize(app, newWidth, newHeight);
                }
            } else if (e.type == SDL_MOUSEMOTION) {
                app.mouseState.xrel += e.motion.xrel;
                app.mouseState.yrel += e.motion.yrel;
            } else if (e.type == SDL_MOUSEWHEEL) {
                app.mouseState.scrollDelta = e.wheel.preciseY;
            } else if (e.type == SDL_KEYUP) {
                if (e.key.keysym.sym == SDLK_F5) {
                    app.psoManager.reload(app.device.Get());
                } else if (e.key.keysym.sym == SDLK_RETURN && e.key.keysym.mod & KMOD_ALT) {
                    ToggleBorderlessWindow(app);
                }
            }
        }

        ReloadIfShaderChanged(app);

        buttonState = SDL_GetMouseState(&mouseX, &mouseY);

        app.camera.locked = (buttonState & SDL_BUTTON_RMASK) == 0;
        if (!app.camera.locked) {
            if (!SDL_GetRelativeMouseMode()) {
                SDL_SetRelativeMouseMode(SDL_TRUE);
                SDL_SetWindowGrab(app.window, SDL_TRUE);
            }
        } else {
            if (SDL_GetRelativeMouseMode()) {
                SDL_SetRelativeMouseMode(SDL_FALSE);
                SDL_SetWindowGrab(app.window, SDL_FALSE);
            }
        }

        BeginGUI(app);
        UpdateScene(app);
        ImGui::Render();

        RenderFrame(app);

        long long endTick = steady_clock::now().time_since_epoch().count();
        app.Stats.lastFrameTimeNS = endTick - frameTick;

        app.lastFrameTick = frameTick;
    }

    WaitForPreviousFrame(app);

    CleanImGui();
    SDL_DestroyWindow(app.window);

    return 0;
}

int RunMDXR(int argc, char** argv)
{
    int status = RunApp(argc, argv);

#if defined(_DEBUG)
    {
        ComPtr<IDXGIDebug1> dxgiDebug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
        {
            dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        }
    }
#endif

    return status;
}
