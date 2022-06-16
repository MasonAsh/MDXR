#include "mdxr.h"
#include "util.h"
#include <iostream>
#include <assert.h>
#include <d3dcompiler.h>
#include <locale>
#include <codecvt>
#include <string>
#include "tiny_gltf.h"
#include "glm/gtc/matrix_transform.hpp"
#include "dxgidebug.h"
#include "stb_image.h"
#include <chrono>
#include <algorithm>
#include <span>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_sdl.h"

using namespace std::chrono;

const int FrameBufferCount = 2;
const int MaxLightCount = 10;
const int MaxMaterialCount = 64;
const int MaxDescriptors = 2048;
const DXGI_FORMAT DepthFormat = DXGI_FORMAT_D32_FLOAT;

enum ConstantIndex {
    ConstantIndex_PrimitiveData,
    ConstantIndex_MaterialData,
    ConstantIndex_Light,
    ConstantIndex_LightPassData,

    ConstantIndex_Count
};

struct PerPrimitiveConstantData {
    // MVP & MV are PerMesh, but most meshes only have one primitive.
    glm::mat4 MVP;
    glm::mat4 MV;
    float padding[32];
};
static_assert((sizeof(PerPrimitiveConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct ManagedPSO {
    ComPtr<ID3D12PipelineState> PSO;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    std::wstring vertexShaderPath;
    std::wstring pixelShaderPath;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;

    ID3D12PipelineState* Get()
    {
        return PSO.Get();
    }

    // FIXME: This leaks. Shouldn't be a big deal since this is just for debugging, but something to keep in mind.
    void reload(ID3D12Device* device)
    {
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;
        ASSERT_HRESULT(
            D3DReadFileToBlob(vertexShaderPath.c_str(), &vertexShader)
        );
        ASSERT_HRESULT(
            D3DReadFileToBlob(pixelShaderPath.c_str(), &pixelShader)
        );
        desc.VS = { reinterpret_cast<UINT8*>(vertexShader->GetBufferPointer()), vertexShader->GetBufferSize() };
        desc.PS = { reinterpret_cast<UINT8*>(pixelShader->GetBufferPointer()), pixelShader->GetBufferSize() };
        ID3D12PipelineState* NewPSO;
        if (!SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&NewPSO)))) {
            std::wcout << L"Error: PSO reload failed for PSO with\n"
                << L"Vertex shader: " << vertexShaderPath << L"\n"
                << L"Pixel shader: " << pixelShaderPath << L"\n";
            return;
        }
        PSO = NewPSO;
    }
};

typedef std::shared_ptr<ManagedPSO> ManagedPSORef;

struct PSOManager {
    std::vector<std::weak_ptr<ManagedPSO>> PSOs;

    void reload(ID3D12Device* device)
    {
        for (auto& PSO : PSOs) {
            if (std::shared_ptr<ManagedPSO> pso = PSO.lock()) {
                pso->reload(device);
            }
        }
    }
};

enum MaterialType {
    MaterialType_Unlit,
    MaterialType_PBR,
};

struct MaterialConstantData {
    UINT diffuseTextureIdx;
    UINT normalTextureIdx;

    glm::vec3 ambientFactor;

    UINT materialType;

    float padding[58];
};
static_assert((sizeof(MaterialConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct DescriptorReference {
    ID3D12DescriptorHeap* heap;
    int incrementSize;
    int index;

    DescriptorReference()
        : heap(nullptr)
        , incrementSize(0)
        , index(-1)
    {
    }

    DescriptorReference(ID3D12DescriptorHeap* heap, int index, int incrementSize)
        : heap(heap)
        , index(index)
        , incrementSize(incrementSize)
    {
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE CPUHandle() const {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(), index, incrementSize);
    }

    CD3DX12_GPU_DESCRIPTOR_HANDLE GPUHandle() const {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), index, incrementSize);
    }
};

struct Primitive {
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    std::vector<D3D12_VERTEX_BUFFER_VIEW> vertexBufferViews;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    D3D12_PRIMITIVE_TOPOLOGY primitiveTopology;
    ManagedPSORef PSO;
    UINT indexCount;
    PerPrimitiveConstantData constantData;

    UINT materialIndex;
};

struct Mesh {
    std::vector<Primitive> primitives;

    glm::mat4 modelMatrix;
};

struct Model {
    std::vector<ComPtr<ID3D12Resource>> buffers;
    std::vector<Mesh> meshes;

    DescriptorReference primitiveDataDescriptors;
    DescriptorReference baseTextureDescriptor;

    // All of the child mesh constant buffers stored in this constant buffer
    ComPtr<ID3D12Resource> perPrimitiveConstantBuffer;
    UINT8* perPrimitiveBufferPtr;
};

struct AppAssets {
    tinygltf::Model models[4];
};

// Fixed capacity descriptor heap. Aborts when the size outgrows initial capacity.
struct DescriptorArena {
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    size_t capacity;
    size_t size;
    UINT descriptorIncrementSize;
    std::string debugName;

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
    }

    DescriptorReference AllocateDescriptors(UINT count, const char* debugName) {
        if (debugName != nullptr) {
            std::cout << this->debugName << " allocation info: " <<
                "\n\tIndex: " << this->size <<
                "\n\tCount: " << count <<
                "\n\tReason: " << debugName << "\n";
        }
        DescriptorReference reference(descriptorHeap.Get(), size, descriptorIncrementSize);
        size += count;
        if (size > capacity) {
            std::cout << "Error: descriptor heap is not large enough\n";
            abort();
        }
        return reference;
    }
};

struct IncrementalFence {
    ComPtr<ID3D12Fence> fence;
    int nextFenceValue;
    HANDLE event;

    void Initialize(ID3D12Device* device)
    {
        ASSERT_HRESULT(
            device->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&fence)
            )
        );
        nextFenceValue = 1;

        event = CreateEvent(nullptr, false, false, nullptr);
        if (event == nullptr) {
            ASSERT_HRESULT(
                HRESULT_FROM_WIN32(
                    GetLastError()
                )
            );
        }
    }

    void SignalAndWaitQueue(ID3D12CommandQueue* commandQueue)
    {
        // Signal and increment the fence value.
        const UINT64 targetFenceValue = nextFenceValue;
        ASSERT_HRESULT(
            commandQueue->Signal(fence.Get(), targetFenceValue)
        );
        nextFenceValue++;

        // Wait until the previous frame is finished.
        if (fence->GetCompletedValue() < targetFenceValue)
        {
            ASSERT_HRESULT(
                fence->SetEventOnCompletion(targetFenceValue, event)
            );

            WaitForSingleObject(event, INFINITE);
        }
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
};

struct MouseState {
    int xrel, yrel;
    float scrollDelta;
};


enum GBufferTarget {
    GBuffer_Diffuse,
    GBuffer_Normal,
    GBuffer_Depth,
    GBuffer_Count,
};

const int GBuffer_RTVCount = GBuffer_Depth;

struct LightPassConstantData {
    glm::mat4 inverseProjectionMatrix;
    UINT baseGBufferIndex;
    UINT debug;
    float pad[46];
};
static_assert((sizeof(LightPassConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct LightConstantData {
    glm::vec4 position;
    glm::vec4 direction;

    glm::vec4 positionViewSpace;
    glm::vec4 directionViewSpace;

    glm::vec4 color;

    float range;
    float intensity;

    float pad[42];
};
static_assert((sizeof(LightConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct App {
    std::string dataDir;
    std::wstring wDataDir;

    SDL_Window* window;
    HWND hwnd;

    bool running;
    long long startTick;
    long long lastFrameTick;

    int windowWidth = 1280;
    int windowHeight = 720;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> renderTargets[FrameBufferCount];
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12CommandAllocator> copyCommandAllocator;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    CD3DX12_VIEWPORT viewport;
    CD3DX12_RECT scissorRect;

    ComPtr<ID3D12Resource> depthStencilBuffer;
    ComPtr<ID3D12DescriptorHeap> dsHeap;

    IncrementalFence fence;

    tinygltf::TinyGLTF loader;

    ComPtr<ID3D12CommandQueue> copyCommandQueue;
    ComPtr<ID3D12GraphicsCommandList> copyCommandList;
    IncrementalFence copyFence;

    ComPtr<ID3D12DescriptorHeap> srvHeap;

    // FIXME: Models should only exist as a concept in the scene graph.
    // there is no reason the renderer itself has to group it like this.
    // All the model data should be handled by special allocators and grouped into
    // resource pools.
    std::vector<Model> models;

    unsigned int frameIdx;
    unsigned int rtvDescriptorSize = 0;
    unsigned int cbvSrvUavDescriptorSize = 0;

    Camera camera;
    const UINT8* keyState;
    MouseState mouseState;

    struct {
        ComPtr<ID3D12DescriptorHeap> srvHeap;
        bool aboutWindowOpen = true;
    } ImGui;

    struct {
        DescriptorArena descriptorHeap;
    } GBufferPass;

    struct {
        ComPtr<ID3D12Resource> constantBuffer;
        MaterialConstantData* materials;
        int count;
        DescriptorReference descriptorReference;
    } MaterialBuffer;

    DescriptorArena lightingPassDescriptorArena;
    struct {
        std::array<ComPtr<ID3D12Resource>, GBuffer_Count - 1> renderTargets;
        DescriptorReference baseSrvReference;
    } GBuffer;

    struct {
        ManagedPSORef PSO;
        ComPtr<ID3D12RootSignature> rootSignature;
    } LightPass;

    struct {
        ComPtr<ID3D12Resource> constantBuffer;

        // These two are stored in the same constant buffer.
        // The lights are stored at offset (LightPassConstantData)
        LightPassConstantData* passData;
        LightConstantData* lights;

        DescriptorReference cbvHandle;

        int count;
    } LightBuffer;

    PSOManager psoManager;
};

void SetupDepthStencil(App& app, bool isResize)
{
    if (!isResize)
    {
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
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
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
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(app.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (int i = 0; i < FrameBufferCount; i++) {
        ASSERT_HRESULT(
            app.swapChain->GetBuffer(i, IID_PPV_ARGS(&app.renderTargets[i]))
        );

        app.device->CreateRenderTargetView(app.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, app.rtvDescriptorSize);
    }
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC DefaultPSODesc()
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

ManagedPSORef CreatePSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    const std::string& shaderName,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc
)
{
    std::wstring wDataDir = convert_to_wstring(dataDir);
    std::wstring wShaderName = convert_to_wstring(shaderName);

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    const std::wstring vertexShaderPath = (wDataDir + L"/" + wShaderName + L".cvert");
    const std::wstring pixelShaderPath = (wDataDir + L"/" + wShaderName + L".cpixel");
    ASSERT_HRESULT(
        D3DReadFileToBlob(vertexShaderPath.c_str(), &vertexShader)
    );
    ASSERT_HRESULT(
        D3DReadFileToBlob(pixelShaderPath.c_str(), &pixelShader)
    );

    auto mPso = std::make_shared<ManagedPSO>();
    mPso->desc = psoDesc;
    mPso->inputLayout = inputLayout;

    mPso->desc.pRootSignature = rootSignature;
    mPso->desc.VS = { reinterpret_cast<UINT8*>(vertexShader->GetBufferPointer()), vertexShader->GetBufferSize() };
    mPso->desc.PS = { reinterpret_cast<UINT8*>(pixelShader->GetBufferPointer()), pixelShader->GetBufferSize() };
    mPso->desc.InputLayout = { mPso->inputLayout.data(), (UINT)mPso->inputLayout.size() };

    ASSERT_HRESULT(
        device->CreateGraphicsPipelineState(
            &mPso->desc,
            IID_PPV_ARGS(&mPso->PSO)
        )
    );
    mPso->vertexShaderPath = vertexShaderPath;
    mPso->pixelShaderPath = pixelShaderPath;

    manager.PSOs.emplace_back(mPso);

    return mPso;
}

ManagedPSORef CreateUnlitMeshPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultPSODesc();
    return CreatePSO(
        manager,
        device,
        dataDir,
        "unlit_mesh",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateDiffuseMeshPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultPSODesc();
    return CreatePSO(
        manager,
        device,
        dataDir,
        "diffuse_mesh",
        rootSignature,
        inputLayout,
        psoDesc
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
    auto psoDesc = DefaultPSODesc();
    psoDesc.NumRenderTargets = GBuffer_RTVCount + 1;
    for (int i = 1; i < psoDesc.NumRenderTargets; i++) {
        psoDesc.RTVFormats[i] = GBufferResourceDesc((GBufferTarget)i, 0, 0).Format;
    }
    return CreatePSO(
        manager,
        device,
        dataDir,
        "mesh_gbuffer_pbr",
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
    auto psoDesc = DefaultPSODesc();
    psoDesc.NumRenderTargets = GBuffer_RTVCount + 1;
    for (int i = 1; i < psoDesc.NumRenderTargets; i++) {
        psoDesc.RTVFormats[i] = GBufferResourceDesc((GBufferTarget)i, 0, 0).Format;
    }
    return CreatePSO(
        manager,
        device,
        dataDir,
        "mesh_gbuffer_unlit",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateGBufferLightingPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    // Unlike other PSOs, we go clockwise here.
    CD3DX12_RASTERIZER_DESC rasterizerState(D3D12_DEFAULT);
    auto psoDesc = DefaultPSODesc();
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

    return CreatePSO(
        manager,
        device,
        dataDir,
        "gbuffer_lighting",
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
    case GBuffer_Diffuse:
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
        app.lightingPassDescriptorArena.Initialize(app.device.Get(), heapDesc, "LightPassArena");
        app.GBuffer.baseSrvReference = app.lightingPassDescriptorArena.AllocateDescriptors(GBuffer_Count, "GBuffer SRVs");
    } else {
        // If we're resizing then we need to release existing gbuffer
        for (auto& renderTarget : app.GBuffer.renderTargets) {
            renderTarget = nullptr;
        }
    }


    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(app.rtvHeap->GetCPUDescriptorHandleForHeapStart(), FrameBufferCount, app.rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle = app.GBuffer.baseSrvReference.CPUHandle();
    for (int i = 0; i < GBuffer_Depth; i++) {
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

        rtvHandle.Offset(1, app.rtvDescriptorSize);
        srvHandle.Offset(1, app.cbvSrvUavDescriptorSize);
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
        cpuHandle.Offset(1, app.cbvSrvUavDescriptorSize);
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
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 4;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
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

void SetupLightBuffer(App& app)
{
    auto descriptorHandle = app.lightingPassDescriptorArena.AllocateDescriptors(MaxLightCount + 1, "light pass and light buffer");

    CreateConstantBufferAndViews(
        app,
        app.LightBuffer.constantBuffer,
        sizeof(LightConstantData),
        MaxLightCount + 1,
        descriptorHandle.CPUHandle()
    );

    app.LightBuffer.constantBuffer->Map(0, nullptr, (void**)&app.LightBuffer.passData);
    // Lights are stored immediately after the pass data
    app.LightBuffer.lights = reinterpret_cast<LightConstantData*>(app.LightBuffer.passData + 1);
    app.LightBuffer.cbvHandle = descriptorHandle;

    app.LightBuffer.passData->baseGBufferIndex = app.GBuffer.baseSrvReference.index;
}

void SetupMaterialBuffer(App& app)
{
    auto& materialBuffer = app.MaterialBuffer;
    materialBuffer.count = 0;

    materialBuffer.descriptorReference = app.GBufferPass.descriptorHeap.AllocateDescriptors(MaxMaterialCount, "MaterialBuffer");
    CreateConstantBufferAndViews(
        app,
        app.MaterialBuffer.constantBuffer,
        sizeof(MaterialConstantData),
        MaxMaterialCount,
        materialBuffer.descriptorReference.CPUHandle()
    );

    ASSERT_HRESULT(materialBuffer.constantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&app.MaterialBuffer.materials)));
}

void SetupGBufferPass(App& app)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = MaxDescriptors;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    app.GBufferPass.descriptorHeap.Initialize(app.device.Get(), heapDesc, "GBufferPassArena");

    SetupMaterialBuffer(app);
}

void SetupLightingPass(App& app)
{
    // {
    //     D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    //     featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    //     if (FAILED(app.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData)))) {
    //         featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    //     }

    //     CD3DX12_DESCRIPTOR_RANGE1 ranges[3];
    //     CD3DX12_ROOT_PARAMETER1 rootParameters[3];

    //     ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer_Count, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
    //     ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
    //     ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, -1, 2, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

    //     // GBuffer
    //     rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
    //     // Light index
    //     rootParameters[1].InitAsConstants(1, 0);
    //     // Pass data & lights buffer
    //     rootParameters[2].InitAsDescriptorTable(2, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);

    //     D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    //     D3D12_STATIC_SAMPLER_DESC sampler = {};
    //     sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    //     sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    //     sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    //     sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    //     sampler.MipLODBias = 0;
    //     sampler.MaxAnisotropy = 0;
    //     sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    //     sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    //     sampler.MinLOD = 0.0f;
    //     sampler.MaxLOD = D3D12_FLOAT32_MAX;
    //     sampler.ShaderRegister = 0;
    //     sampler.RegisterSpace = 0;
    //     sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    //     CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    //     rootSignatureDesc.Init_1_1(
    //         _countof(rootParameters),
    //         rootParameters,
    //         1,
    //         &sampler,
    //         rootSignatureFlags
    //     );

    //     ComPtr<ID3DBlob> signature;
    //     ComPtr<ID3DBlob> error;
    //     if (!SUCCEEDED(
    //         D3DX12SerializeVersionedRootSignature(
    //             &rootSignatureDesc,
    //             featureData.HighestVersion,
    //             &signature,
    //             &error
    //         )
    //     )) {
    //         std::cout << "Error: root signature compilation failed\n";
    //         std::cout << (char*)error->GetBufferPointer();
    //         abort();
    //     }
    //     ASSERT_HRESULT(
    //         app.device->CreateRootSignature(
    //             0,
    //             signature->GetBufferPointer(),
    //             signature->GetBufferSize(),
    //             IID_PPV_ARGS(&app.LightPass.rootSignature)
    //         )
    //     );
    // }

    SetupLightBuffer(app);

    // GBuffer lighting does not need an input layout, as the vertices are created
    // entirely in the vertex buffer without any input vertices.
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;

    app.LightPass.PSO = CreateGBufferLightingPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );
}

void PrintCapabilities(ID3D12Device* device)
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
}

void InitD3D(App& app)
{
    // #ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    } else {
        std::cout << "Failed to enable D3D12 debug layer\n";
    }

    ComPtr<ID3D12Debug1> debugController1;
    ASSERT_HRESULT(debugController->QueryInterface(IID_PPV_ARGS(&debugController1)));
    debugController1->SetEnableGPUBasedValidation(true);
    // #endif

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

    PrintCapabilities(device);

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

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();

    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameBufferCount + GBuffer_RTVCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ASSERT_HRESULT(
            device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&app.rtvHeap))
        );
    }

    app.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    app.cbvSrvUavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    SetupRenderTargets(app);
    SetupDepthStencil(app, false);
    SetupGBuffer(app, false);

    CreateMainRootSignature(app);

    SetupGBufferPass(app);
    SetupLightingPass(app);

    ASSERT_HRESULT(
        device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COPY,
            IID_PPV_ARGS(&app.copyCommandAllocator)
        )
    );

    ASSERT_HRESULT(
        device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&app.commandAllocator)
        )
    );
}

void InitWindow(App& app)
{
    SDL_Window* window = SDL_CreateWindow("MDXR",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        app.windowWidth, app.windowHeight,
        SDL_WINDOW_RESIZABLE
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

void InitApp(App& app, int argc, char** argv)
{
    app.viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(app.windowWidth), static_cast<float>(app.windowHeight));
    app.scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(app.windowWidth), static_cast<LONG>(app.windowHeight));
    app.rtvDescriptorSize = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--datadir")) {
            if (i + 1 < argc) {
                app.dataDir = argv[i + 1];
            }
        }
    }

    if (app.dataDir.empty()) {
        std::cout << "Error: no data directory specified\n";
        std::cout << "The program will now exit" << std::endl;
        abort();
    }

    app.wDataDir = convert_to_wstring(app.dataDir);
}


void WaitForPreviousFrame(App& app)
{
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity. More advanced samples 
    // illustrate how to use fences for efficient resource usage.

    app.fence.SignalAndWaitQueue(app.commandQueue.Get());

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();
}

AppAssets LoadAssets(App& app)
{
    AppAssets assets;

    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    CHECK(
        loader.LoadASCIIFromFile(
            &assets.models[0],
            &err,
            &warn,
            app.dataDir + "/floor/floor.gltf"
        )
    );

    CHECK(
        loader.LoadASCIIFromFile(
            &assets.models[1],
            &err,
            &warn,
            app.dataDir + "/FlightHelmet/FlightHelmet.gltf"
        )
    );

    CHECK(
        loader.LoadASCIIFromFile(
            &assets.models[2],
            &err,
            &warn,
            app.dataDir + "/Duck.gltf"
        )
    );
    glm::dmat4 boxTransform(1.0);
    boxTransform = glm::scale(boxTransform, glm::dvec3(0.08));
    boxTransform = glm::translate(boxTransform, glm::dvec3(-120.0, 120.0, 160.0));
    boxTransform = glm::rotate(boxTransform, glm::radians(290.0), glm::dvec3(0.0, 1.0, 0.0));
    auto valuePtr = glm::value_ptr(boxTransform);
    std::vector<double> vectorValues(valuePtr, valuePtr + 16);
    assets.models[2].nodes[2].matrix = vectorValues;

    CHECK(
        loader.LoadASCIIFromFile(
            &assets.models[3],
            &err,
            &warn,
            app.dataDir + "/skybox/skybox.gltf"
        )
    );

    return assets;
}

ComPtr<ID3D12Resource> CreateUploadHeap(App& app, const std::vector<D3D12_RESOURCE_DESC>& resourceDescs, std::vector<UINT64>& gltfBufferOffsets)
{
    UINT64 uploadHeapSize = 0;

    // Compute upload heap size
    for (const auto& desc : resourceDescs) {
        // For textures we need to skip to the next multiple of D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT.
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            uploadHeapSize += D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - (uploadHeapSize % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        }
        UINT64 requiredSize;
        app.device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &requiredSize);
        gltfBufferOffsets.push_back(uploadHeapSize);
        uploadHeapSize += requiredSize;
    }

    ID3D12Device* device = app.device.Get();

    ComPtr<ID3D12Resource> uploadHeap;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadHeapSize);
    ASSERT_HRESULT(
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&uploadHeap)
        )
    );

    return uploadHeap;
}

void CreateModelDescriptors(
    Model& outputModel,
    App& app,
    const tinygltf::Model& inputModel,
    const std::span<ComPtr<ID3D12Resource>> textureResources
)
{
    // Allocate 1 descriptor for the constant buffer and the rest for the textures.
    size_t numConstantBuffers = 0;
    for (const auto& mesh : inputModel.meshes) {
        numConstantBuffers += mesh.primitives.size();
    }
    size_t numDescriptors = numConstantBuffers + inputModel.textures.size();

    UINT incrementSize = app.cbvSrvUavDescriptorSize;

    // Create per-primitive constant buffer
    ComPtr<ID3D12Resource> perPrimitiveConstantBuffer;
    {
        auto descriptorRef = app.GBufferPass.descriptorHeap.AllocateDescriptors(numConstantBuffers, "PerPrimitiveConstantBuffer");
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
        auto descriptorRef = app.GBufferPass.descriptorHeap.AllocateDescriptors(textureResources.size(), "MeshTextures");
        auto cpuHandle = descriptorRef.CPUHandle();
        //CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), (int)inputModel.meshes.size(), incrementSize);
        for (const auto& textureResource : textureResources) {
            auto textureDesc = textureResource->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = textureDesc.Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            app.device->CreateShaderResourceView(textureResource.Get(), &srvDesc, cpuHandle);
            cpuHandle.Offset(1, incrementSize);
        }
        outputModel.baseTextureDescriptor = descriptorRef;
    }

    outputModel.perPrimitiveConstantBuffer = perPrimitiveConstantBuffer;
    CD3DX12_RANGE readRange(0, 0);
    ASSERT_HRESULT(perPrimitiveConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&outputModel.perPrimitiveBufferPtr)));
}

D3D12_RESOURCE_DESC GetImageResourceDesc(const tinygltf::Image& image)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.MipLevels = 1;
    desc.Width = image.width;
    desc.Height = image.height;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.DepthOrArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

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

std::vector<D3D12_RESOURCE_DESC> CreateResourceDescriptions(const tinygltf::Model& model)
{
    std::vector<D3D12_RESOURCE_DESC> descs;
    for (const auto& buffer : model.buffers) {
        D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(buffer.data.size());
        descs.push_back(desc);
    }
    for (const auto& image : model.images) {
        auto desc = GetImageResourceDesc(image);
        descs.push_back(desc);
    }
    return descs;
}

std::vector<ComPtr<ID3D12Resource>> UploadModelBuffers(
    Model& outputModel,
    App& app,
    const tinygltf::Model& inputModel,
    ComPtr<ID3D12Resource> uploadHeap,
    const std::vector<D3D12_RESOURCE_DESC>& resourceDescs,
    const std::vector<UINT64>& uploadOffsets,
    std::span<ComPtr<ID3D12Resource>>& outGeometryResources,
    std::span<ComPtr<ID3D12Resource>>& outTextureResources
)
{
    std::vector<ComPtr<ID3D12Resource>> resourceBuffers;
    resourceBuffers.reserve(inputModel.buffers.size() + inputModel.images.size());

    // Copy all GLTF buffer data to the upload heap
    size_t uploadHeapPosition = 0;
    UINT8* uploadHeapPtr;
    CD3DX12_RANGE readRange(0, 0);
    uploadHeap->Map(0, &readRange, reinterpret_cast<void**>(&uploadHeapPtr));
    int bufferIdx = 0;
    for (bufferIdx = 0; bufferIdx < inputModel.buffers.size(); bufferIdx++) {
        auto buffer = inputModel.buffers[bufferIdx];
        auto uploadHeapPosition = uploadOffsets[bufferIdx];
        memcpy(uploadHeapPtr + uploadHeapPosition, buffer.data.data(), buffer.data.size());
    }
    int imageIdx = 0;
    for (; bufferIdx < uploadOffsets.size(); imageIdx++, bufferIdx++)
    {
        auto image = inputModel.images[imageIdx];
        auto uploadHeapPosition = uploadOffsets[bufferIdx];
        memcpy(uploadHeapPtr + uploadHeapPosition, image.image.data(), image.image.size());
    }
    uploadHeap->Unmap(0, nullptr);

    std::vector<CD3DX12_RESOURCE_BARRIER> resourceBarriers;

    // Copy all the gltf buffer data to a dedicated geometry buffer
    for (bufferIdx = 0; bufferIdx < inputModel.buffers.size(); bufferIdx++) {
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

        app.commandList->CopyBufferRegion(
            geometryBuffer.Get(), 0, uploadHeap.Get(), uploadOffsets[bufferIdx], gltfBuffer.data.size());

        resourceBuffers.push_back(geometryBuffer);

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            geometryBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER
        ));
    }

    // Upload images to buffers
    for (; bufferIdx < inputModel.buffers.size() + inputModel.images.size(); bufferIdx++) {
        const auto& gltfImage = inputModel.images[bufferIdx - inputModel.buffers.size()];
        ComPtr<ID3D12Resource> buffer;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = GetImageResourceDesc(gltfImage);
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

        UINT64 requiredSize;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        app.device->GetCopyableFootprints(
            &resourceDesc,
            0,
            1,
            uploadOffsets[bufferIdx],
            &footprint,
            nullptr,
            nullptr,
            &requiredSize
        );
        // app.commandList->CopyBufferRegion(buffer.Get(), 0, uploadHeap.Get(), gltfBufferOffsets[bufferIdx], gltfImage.image.size());
        const CD3DX12_TEXTURE_COPY_LOCATION Dst(buffer.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION Src(uploadHeap.Get(), footprint);
        app.commandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);

        resourceBuffers.push_back(buffer);

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            buffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        ));
    }

    auto endGeometryBuffer = resourceBuffers.begin() + inputModel.buffers.size();
    outGeometryResources = std::span(resourceBuffers.begin(), endGeometryBuffer);
    outTextureResources = std::span(endGeometryBuffer, resourceBuffers.end());

    outputModel.buffers = resourceBuffers;

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

        glm::mat4 T = glm::translate(glm::mat4(1.0f), translate);
        glm::mat4 R = glm::toMat4(rotation);
        glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
        return S * R * T;
    }
}

void TraverseNode(const tinygltf::Model& model, const tinygltf::Node& node, std::vector<Mesh>& meshes, const glm::mat4& accumulator) {
    glm::mat4 transform = accumulator * GetNodeTransfomMatrix(node);
    if (node.mesh != -1) {
        meshes[node.mesh].modelMatrix = transform;
    }
    for (const auto& child : node.children) {
        TraverseNode(model, model.nodes[child], meshes, transform);
    }
}

// Traverse the GLTF scene to get the correct model matrix for each mesh.
void ResolveMeshTransforms(
    const tinygltf::Model& model,
    std::vector<Mesh>& meshes
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

void FinalizeModel(
    Model& outputModel,
    App& app,
    const tinygltf::Model& inputModel
)
{
    // Just storing these strings so that we don't have to keep the Model object around.
    static std::array SEMANTIC_NAMES{
        std::string("POSITION"),
        std::string("NORMAL"),
        std::string("TEXCOORD"),
        std::string("TANGENT"),
        std::string("COLOR"),
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

    for (const auto& mesh : inputModel.meshes) {
        std::vector<Primitive> primitives;
        for (const auto& primitive : mesh.primitives) {
            Primitive drawCall;

            std::vector<D3D12_VERTEX_BUFFER_VIEW>& vertexBufferViews = drawCall.vertexBufferViews;

            std::map<int, int> accessorToD3DBufferMap;

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
            std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout = drawCall.inputLayout;
            inputLayout.reserve(primitive.attributes.size());
            int InputSlotCount = 0;
            std::map<int, int> bufferViewToInputSlotMap;
            for (const auto& attrib : primitive.attributes) {
                auto [targetSemantic, semanticIndex] = ParseAttribToSemantic(attrib.first);
                auto semanticName = std::find(SEMANTIC_NAMES.begin(), SEMANTIC_NAMES.end(), targetSemantic);
                if (semanticName != SEMANTIC_NAMES.end()) {
                    D3D12_INPUT_ELEMENT_DESC desc;
                    int accessorIdx = attrib.second;
                    auto& accessor = inputModel.accessors[accessorIdx];
                    desc.SemanticName = semanticName->c_str();
                    desc.SemanticIndex = semanticIndex;
                    desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                    desc.InstanceDataStepRate = 0;
                    UINT byteStride;
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
                    UINT vertexStartOffset = (UINT)bufferView.byteOffset + (UINT)accessor.byteOffset - (UINT)(accessor.byteOffset % byteStride);
                    D3D12_GPU_VIRTUAL_ADDRESS vertexStartAddress = buffer->GetGPUVirtualAddress() + vertexStartOffset;

                    desc.AlignedByteOffset = (UINT)accessor.byteOffset - vertexStartOffset + (UINT)bufferView.byteOffset;

                    // No d3d buffer view attached to this range of vertices yet, add one
                    if (!vertexStartOffsetToBufferView.contains(vertexStartAddress)) {
                        D3D12_VERTEX_BUFFER_VIEW view;
                        view.BufferLocation = vertexStartAddress;
                        view.SizeInBytes = (UINT)accessor.count * byteStride;
                        view.StrideInBytes = byteStride;
                        vertexBufferViews.push_back(view);
                        vertexStartOffsetToBufferView[vertexStartAddress] = (UINT)vertexBufferViews.size() - 1;
                    }
                    desc.InputSlot = vertexStartOffsetToBufferView[vertexStartAddress];

                    inputLayout.push_back(desc);

                    D3D12_PRIMITIVE_TOPOLOGY& topology = drawCall.primitiveTopology;
                    switch (primitive.mode)
                    {
                    case TINYGLTF_MODE_POINTS:
                        topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
                        break;
                    case TINYGLTF_MODE_LINE:
                        topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
                        break;
                    case TINYGLTF_MODE_LINE_LOOP:
                        std::cout << "Error: line loops are not supported";
                        abort();
                        break;
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
                        abort();
                        break;
                    };

                } else {
                    std::cout << "Unsupported semantic in " << mesh.name << " " << targetSemantic;
                }
            }

            int diffuseIdx = -1;
            int normalIdx = -1;
            if (primitive.material != -1) {
                auto& materialBuffer = app.MaterialBuffer;
                auto materialIndex = materialBuffer.count++;
                drawCall.materialIndex = materialIndex;
                auto& material = materialBuffer.materials[materialIndex];
                const auto& gltfMaterial = inputModel.materials[primitive.material];
                diffuseIdx = gltfMaterial.pbrMetallicRoughness.baseColorTexture.index;
                normalIdx = gltfMaterial.normalTexture.index;
                if (diffuseIdx != -1) {
                    material.diffuseTextureIdx = outputModel.baseTextureDescriptor.index + diffuseIdx;
                } else {
                    material.diffuseTextureIdx = -1;
                }
                if (normalIdx != -1) {
                    material.normalTextureIdx = outputModel.baseTextureDescriptor.index + normalIdx;
                } else {
                    material.normalTextureIdx = -1;
                }
                if (gltfMaterial.extensions.contains("KHR_materials_unlit")) {
                    material.materialType = MaterialType_Unlit;
                } else {
                    material.materialType = MaterialType_PBR;
                }
            } else {
                drawCall.materialIndex = -1;
            }

            {
                D3D12_INDEX_BUFFER_VIEW& ibv = drawCall.indexBufferView;
                int accessorIdx = primitive.indices;
                auto& accessor = inputModel.accessors[accessorIdx];
                int indexBufferViewIdx = accessor.bufferView;
                auto bufferView = inputModel.bufferViews[indexBufferViewIdx];
                ibv.BufferLocation = resourceBuffers[bufferView.buffer]->GetGPUVirtualAddress() + bufferView.byteOffset + accessor.byteOffset;
                ibv.SizeInBytes = (UINT)(bufferView.byteLength - accessor.byteOffset);
                switch (accessor.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    ibv.Format = DXGI_FORMAT_R8_UINT;
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    ibv.Format = DXGI_FORMAT_R16_UINT;
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    ibv.Format = DXGI_FORMAT_R32_UINT;
                    break;
                };
                drawCall.indexCount = (UINT)accessor.count;
            }

            primitives.push_back(drawCall);
        }

        outputModel.meshes.push_back(Mesh{ primitives });
    }

    ResolveMeshTransforms(inputModel, outputModel.meshes);

    // FIXME: So many wasted PSOs.
    for (auto& mesh : outputModel.meshes) {
        for (auto& primitive : mesh.primitives) {
            MaterialConstantData& material = app.MaterialBuffer.materials[primitive.materialIndex];
            if (material.materialType == MaterialType_PBR) {
                primitive.PSO = CreateMeshPBRPSO(
                    app.psoManager,
                    app.device.Get(),
                    app.dataDir,
                    app.rootSignature.Get(),
                    primitive.inputLayout
                );
            } else if (material.materialType == MaterialType_Unlit) {
                primitive.PSO = CreateMeshUnlitPSO(
                    app.psoManager,
                    app.device.Get(),
                    app.dataDir,
                    app.rootSignature.Get(),
                    primitive.inputLayout
                );
            } else {
                std::cout << "Unimplemented material type";
                abort();
            }
        }
    }
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

void ProcessAssets(App& app, const AppAssets& assets)
{
    // TODO: easy candidate for multithreading
    for (const auto& gltfModel : assets.models) {
        if (!ValidateGLTFModel(gltfModel)) {
            continue;
        }

        auto resourceDescs = CreateResourceDescriptions(gltfModel);

        std::vector<UINT64> uploadOffsets;
        auto uploadHeap = CreateUploadHeap(app, resourceDescs, uploadOffsets);

        std::span<ComPtr<ID3D12Resource>> geometryBuffers;
        std::span<ComPtr<ID3D12Resource>> textureBuffers;

        Model model;

        // Can only call this ONCE before command list executed
        // This will need to be adapted to handle N models.
        auto resourceBuffers = UploadModelBuffers(
            model,
            app,
            gltfModel,
            uploadHeap,
            resourceDescs,
            uploadOffsets,
            geometryBuffers,
            textureBuffers
        );

        CreateModelDescriptors(model, app, gltfModel, textureBuffers);
        FinalizeModel(model, app, gltfModel);

        app.models.push_back(model);

        app.commandList->Close();

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

    app.commandList->Close();
}

void LoadScene(App& app, const AppAssets& assets)
{
    ID3D12Device* device = app.device.Get();

    // Create command lists
    {
        ASSERT_HRESULT(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_COPY,
                app.copyCommandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&app.copyCommandList)
            )
        );

        ASSERT_HRESULT(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                app.commandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&app.commandList)
            )
        );
    }

    ID3D12GraphicsCommandList* commandList = app.commandList.Get();

    {
        app.fence.Initialize(app.device.Get());
        app.copyFence.Initialize(app.device.Get());
    }

    ProcessAssets(app, assets);
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
    for (int i = 0; i < MaxLightCount; i++) {
        float angle = i * glm::two_pi<float>() / 4;
        float x = cos(angle);
        float z = sin(angle);
        app.LightBuffer.lights[i].color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        app.LightBuffer.lights[i].intensity = 1.5f;
        app.LightBuffer.lights[i].position = glm::vec4(x, 2.0f, z, 1.0f);
        app.LightBuffer.lights[i].range = 5.0f;
    }
}

void InitializeScene(App& app) {
    InitializeCamera(app);
    InitializeLights(app);
}

void UpdatePerPrimitiveConstantBuffers(Model& model, const glm::mat4& projection, const glm::mat4& view)
{
    glm::mat4 viewProjection = projection * view;
    PerPrimitiveConstantData* perPrimitiveBuffer = reinterpret_cast<PerPrimitiveConstantData*>(model.perPrimitiveBufferPtr);
    for (int i = 0; i < model.meshes.size(); i++) {
        auto& mesh = model.meshes[i];
        auto modelMatrix = mesh.modelMatrix;
        auto mvp = viewProjection * modelMatrix;
        auto mv = view * modelMatrix;
        for (const auto& primitive : mesh.primitives) {
            perPrimitiveBuffer->MVP = mvp;
            perPrimitiveBuffer->MV = mv;
            perPrimitiveBuffer++;
        }
    }
}

void UpdateLightConstantBuffers(App& app, const glm::mat4& projection, const glm::mat4& view) {
    app.LightBuffer.passData->inverseProjectionMatrix = glm::inverse(projection);
    for (int i = 0; i < app.LightBuffer.count; i++) {
        auto& lightData = app.LightBuffer.lights[i];
        lightData.positionViewSpace = view * lightData.position;
        lightData.directionViewSpace = view * lightData.direction;
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
    long long currentTick = high_resolution_clock::now().time_since_epoch().count();
    long long ticksSinceStart = currentTick - app.startTick;
    float timeSeconds = (float)ticksSinceStart / (float)1e9;
    long long deltaTicks = currentTick - app.lastFrameTick;
    float deltaSeconds = (float)deltaTicks / (float)1e9;

    glm::mat4 projection = glm::perspective(glm::pi<float>() * 0.2f, (float)app.windowWidth / (float)app.windowHeight, 0.1f, 1000.0f);
    glm::mat4 view = UpdateFlyCamera(app, deltaSeconds);

    UpdateLightConstantBuffers(app, projection, view);

    for (auto& model : app.models) {
        UpdatePerPrimitiveConstantBuffers(model, projection, view);
    }
}

void DrawModelCommandList(const Model& model, ID3D12GraphicsCommandList* commandList, int cbvOffsetIncrement)
{
    // Set the texture array. Textures are dynamically indexed from the constant buffer.
    // {
    //     CD3DX12_GPU_DESCRIPTOR_HANDLE texturesHandle(model.mainDescriptorHeap->GetGPUDescriptorHandleForHeapStart(), model.baseTextureDescriptorIdx, cbvOffsetIncrement);
    //     commandList->SetGraphicsRootDescriptorTable(1, texturesHandle);
    // }
    int constantBufferIdx = model.primitiveDataDescriptors.index;
    for (const auto& mesh : model.meshes) {
        for (const auto& primitive : mesh.primitives) {
            // Set the per-primitive constant buffer
            UINT constantValues[2] = { constantBufferIdx, primitive.materialIndex };
            commandList->SetGraphicsRoot32BitConstants(0, 2, constantValues, 0);
            commandList->IASetPrimitiveTopology(primitive.primitiveTopology);
            commandList->SetPipelineState(primitive.PSO->Get());
            commandList->IASetVertexBuffers(0, (UINT)primitive.vertexBufferViews.size(), primitive.vertexBufferViews.data());
            commandList->IASetIndexBuffer(&primitive.indexBufferView);
            commandList->DrawIndexedInstanced(primitive.indexCount, 1, 0, 0, 0);
            constantBufferIdx++;
        }
    }
}

void DrawFullscreenQuad(const App& app, ID3D12GraphicsCommandList* commandList)
{
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->DrawInstanced(4, 1, 0, 0);
}

void BindAndClearGBufferRTVs(const App& app, ID3D12GraphicsCommandList* commandList)
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE renderTargetHandles[1 + GBuffer_RTVCount] = {};
    CD3DX12_CPU_DESCRIPTOR_HANDLE backBufferHandle(app.rtvHeap->GetCPUDescriptorHandleForHeapStart(), app.frameIdx, app.rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE gBufferHandle(app.rtvHeap->GetCPUDescriptorHandleForHeapStart(), FrameBufferCount, app.rtvDescriptorSize);
    renderTargetHandles[0] = backBufferHandle;
    for (int i = 0; i < GBuffer_RTVCount; i++) {
        renderTargetHandles[i + 1] = gBufferHandle;
        gBufferHandle.Offset(1, app.rtvDescriptorSize);
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

// Draws all unlit meshes directly to the backbuffer.
// void UnlitPass(const App& app, ID3D12GraphicsCommandList* commandList)
// {
//     auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.renderTargets[app.frameIdx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
//     commandList->ResourceBarrier(1, &barrier);

//     for (const auto& model : app.models) {
//         DrawModelCommandList(model, commandList, app.cbvSrvUavDescriptorSize);
//     }
// }

void TransitionRTVsForRendering(const App& app, ID3D12GraphicsCommandList* commandList)
{
    // Transition our GBuffers into being render targets.
    std::array<D3D12_RESOURCE_BARRIER, GBuffer_Count + 1> barriers;
    // Back buffer
    barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(app.renderTargets[app.frameIdx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // GBuffer RTVs
    for (int i = 0; i < GBuffer_RTVCount; i++) {
        barriers[i + 1] = CD3DX12_RESOURCE_BARRIER::Transition(app.GBuffer.renderTargets[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    // Depth buffer is special
    barriers[GBuffer_Depth + 1] = CD3DX12_RESOURCE_BARRIER::Transition(app.depthStencilBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
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

void GBufferPass(const App& app, ID3D12GraphicsCommandList* commandList)
{
    TransitionRTVsForRendering(app, commandList);

    BindAndClearGBufferRTVs(app, commandList);

    ID3D12DescriptorHeap* mainDescriptorHeap = app.GBufferPass.descriptorHeap.descriptorHeap.Get();
    ID3D12DescriptorHeap* ppHeaps[] = { mainDescriptorHeap };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    commandList->SetGraphicsRootSignature(app.rootSignature.Get());

    for (const auto& model : app.models) {
        DrawModelCommandList(model, commandList, app.cbvSrvUavDescriptorSize);
    }

    TransitionGBufferForLighting(app, commandList);
}

void FinalPass(const App& app, ID3D12GraphicsCommandList* commandList)
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(app.rtvHeap->GetCPUDescriptorHandleForHeapStart(), app.frameIdx, app.rtvDescriptorSize);

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    {
        auto descriptorHeap = app.lightingPassDescriptorArena.descriptorHeap;
        ID3D12DescriptorHeap* ppHeaps[] = { descriptorHeap.Get() };
        commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
        // Root signature must be set AFTER heaps are set when CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED is set.
        commandList->SetGraphicsRootSignature(app.rootSignature.Get());

        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        commandList->SetPipelineState(app.LightPass.PSO->Get());
        for (int i = 0; i < app.LightBuffer.count; i++) {
            UINT constantValues[2] = {
                app.LightBuffer.cbvHandle.index + i + 1,
                app.LightBuffer.cbvHandle.index
            };
            commandList->SetGraphicsRoot32BitConstants(0, 2, constantValues, 2);
            // commandList->SetGraphicsRoot32BitConstant(ConstantIndex_Light, app.LightBuffer.cbvHandle.index + i + 1, 0);
            // commandList->SetGraphicsRoot32BitConstant(ConstantIndex_LightPassData, app.LightBuffer.cbvHandle.index, 0);
            DrawFullscreenQuad(app, commandList);
        }
    }

    ID3D12DescriptorHeap* ppHeaps[] = { app.ImGui.srvHeap.Get() };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);

    // Indicate that the back buffer will now be used to present.
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.renderTargets[app.frameIdx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    commandList->ResourceBarrier(1, &barrier);
}

void BuildCommandList(const App& app)
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
    FinalPass(app, commandList);

    ASSERT_HRESULT(
        commandList->Close()
    );
}

void RenderFrame(App& app)
{
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

void DrawToolsWindow(App& app)
{
    static bool toolsOpen = true;
    if (ImGui::Begin("Tools", &toolsOpen)) {
        if (ImGui::Button("Reset Camera")) {
            InitializeCamera(app);
        }

        if (ImGui::Button("Reload Shaders")) {
            app.psoManager.reload(app.device.Get());
        }

        if (ImGui::Checkbox("Shader Debug Flag", (bool*)&app.LightBuffer.passData->debug)) {

        }

        ImGui::End();
    }
}

void DrawLightsEditor(App& app)
{
    static bool lightsOpen = true;
    static int selectedLightIdx = 0;

    char labels[MaxLightCount][20];
    for (int i = 0; i < MaxLightCount; i++) {
        std::string label = "Light #" + std::to_string(i);
        strcpy_s(labels[i], label.c_str());
    }

    if (ImGui::Begin("Lights", &lightsOpen)) {
        ImGui::BeginGroup();
        if (ImGui::Button("New light")) {
            app.LightBuffer.count = glm::min(app.LightBuffer.count + 1, MaxLightCount);
        }
        if (ImGui::Button("Remove light")) {
            app.LightBuffer.count = glm::max(app.LightBuffer.count - 1, 0);
        }
        ImGui::EndGroup();

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

        ImGui::Separator();
        if (selectedLightIdx != -1) {
            auto& light = app.LightBuffer.lights[selectedLightIdx];
            ImGui::ColorEdit3("Color", (float*)&light.color, ImGuiColorEditFlags_PickerHueWheel);
            ImGui::DragFloat3("Position", (float*)&light.position, 0.1);
            ImGui::DragFloat("Range", &light.range, 0.1f, 0.0f, 1000.0f, nullptr, 1.0f);
            ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f, nullptr, 1.0f);
        } else {
            ImGui::Text("No light selected");
        }

        ImGui::End();
    }
}

void BeginGUI(App& app)
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    DrawToolsWindow(app);
    DrawLightsEditor(app);
}

int RunApp(int argc, char** argv)
{
    App app;

    InitApp(app, argc, argv);
    InitWindow(app);
    InitD3D(app);
    InitImGui(app);

    {
        AppAssets assets = LoadAssets(app);
        LoadScene(app, assets);
    }

    InitializeScene(app);

    SDL_Event e;

    app.startTick = high_resolution_clock().now().time_since_epoch().count();
    app.lastFrameTick = app.startTick;

    int mouseX, mouseY;
    int buttonState = SDL_GetMouseState(&mouseX, &mouseY);

    app.running = true;
    while (app.running) {
        long long frameTick = high_resolution_clock().now().time_since_epoch().count();
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
                app.mouseState.xrel = e.motion.xrel;
                app.mouseState.yrel = e.motion.yrel;
            } else if (e.type == SDL_MOUSEWHEEL) {
                app.mouseState.scrollDelta = e.wheel.preciseY;
            } else if (e.type == SDL_KEYUP) {
                if (e.key.keysym.sym == SDLK_F5) {
                    app.psoManager.reload(app.device.Get());
                }
            }
        }

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
