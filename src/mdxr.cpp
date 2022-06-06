#include "mdxr.h"
#include "util.h"
#include <iostream>
#include <assert.h>
#include <d3dcompiler.h>
#include <locale>
#include <codecvt>
#include <string>
#include "tiny_gltf.h"
#include "mesh.h"
#include "glm/gtc/matrix_transform.hpp"
#include "dxgidebug.h"
#include "stb_image.h"
#include <chrono>
#include <algorithm>
#include <span>

using namespace std::chrono;

using namespace DirectX;

const int FrameBufferCount = 2;

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT4 color;
};

struct ConstantBufferData {
    glm::mat4 MVP;
    float padding[48]; // 256 aligned
};

static_assert((sizeof(ConstantBufferData) % 256) == 0, "Constant buffer must be 256-byte aligned");

struct AppAssets {
    tinygltf::Model gltfModel;
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

struct App {
    std::string dataDir;
    std::wstring wDataDir;

    SDL_Window* window;
    HWND hwnd;

    int windowWidth = 640;
    int windowHeight = 480;

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

    IncrementalFence fence;

    tinygltf::TinyGLTF loader;

    ComPtr<ID3D12CommandQueue> copyCommandQueue;
    ComPtr<ID3D12GraphicsCommandList> copyCommandList;
    IncrementalFence copyFence;

    ComPtr<ID3D12PipelineState> unlitMeshPSO;

    ComPtr<ID3D12DescriptorHeap> mainDescriptorHeap;
    ConstantBufferData constantBufferData;
    ComPtr<ID3D12Resource> constantBuffer;
    UINT8* constantBufferDataPtr = nullptr;

    ComPtr<ID3D12DescriptorHeap> srvHeap;

    Model model;

    std::vector<ComPtr<ID3D12Resource>> geometryBuffers;

    unsigned int frameIdx;
    unsigned int rtvDescriptorSize = 0;
};

void InitD3D(App& app)
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    } else {
        std::cout << "Failed to enable D3D12 debug layer\n";
    }

    ComPtr<ID3D12Debug1> debugController1;
    ASSERT_HRESULT(debugController->QueryInterface(IID_PPV_ARGS(&debugController1)));
    debugController1->SetEnableGPUBasedValidation(true);
#endif

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
        rtvHeapDesc.NumDescriptors = FrameBufferCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ASSERT_HRESULT(
            device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&app.rtvHeap))
        );

        D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = {};
        cbvHeapDesc.NumDescriptors = 2;
        cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        ASSERT_HRESULT(
            device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&app.mainDescriptorHeap))
        );
    }

    app.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(app.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (int i = 0; i < FrameBufferCount; i++) {
        ASSERT_HRESULT(
            app.swapChain->GetBuffer(i, IID_PPV_ARGS(&app.renderTargets[i]))
        );

        device->CreateRenderTargetView(app.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, app.rtvDescriptorSize);
    }

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
        0
    );

    assert(window);

    app.window = window;

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    assert(SDL_GetWindowWMInfo(window, &wmInfo));
    app.hwnd = wmInfo.info.win.window;
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

ComPtr<ID3D12PipelineState> CreatePSO(
    ID3D12Device* device,
    const std::string& dataDir,
    const std::string& shaderName,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    std::wstring wDataDir = convert_to_wstring(dataDir);
    std::wstring wShaderName = convert_to_wstring(shaderName);

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ASSERT_HRESULT(
        D3DReadFileToBlob((wDataDir + L"/" + wShaderName + L".cvert").c_str(), &vertexShader)
    );
    ASSERT_HRESULT(
        D3DReadFileToBlob((wDataDir + L"/" + wShaderName + L".cpixel").c_str(), &pixelShader)
    );

    // Describe and create the graphics pipeline state object (PSO).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout.data(), (UINT)inputLayout.size() };
    psoDesc.pRootSignature = rootSignature;
    psoDesc.VS = { reinterpret_cast<UINT8*>(vertexShader->GetBufferPointer()), vertexShader->GetBufferSize() };
    psoDesc.PS = { reinterpret_cast<UINT8*>(pixelShader->GetBufferPointer()), pixelShader->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> PSO;

    ASSERT_HRESULT(
        device->CreateGraphicsPipelineState(
            &psoDesc,
            IID_PPV_ARGS(&PSO)
        )
    );

    return PSO;
}

ComPtr<ID3D12PipelineState> CreateUnlitMeshPSO(
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    return CreatePSO(
        device,
        dataDir,
        "unlit_mesh",
        rootSignature,
        inputLayout
    );
}

ComPtr<ID3D12PipelineState> CreateDiffuseMeshPSO(
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    return CreatePSO(
        device,
        dataDir,
        "diffuse_mesh",
        rootSignature,
        inputLayout
    );
}

void UpdateConstantBuffer(ConstantBufferData& constantBufferData, void* constantBufferPtr)
{
    memcpy(constantBufferPtr, &constantBufferData, sizeof(ConstantBufferData));
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
    tinygltf::Model gltf;
    std::string err;
    std::string warn;
    assert(
        loader.LoadASCIIFromFile(
            &assets.gltfModel,
            &err,
            &warn,
            app.dataDir + "/BoxTextured.gltf"
        )
    );

    return assets;
}

ComPtr<ID3D12Resource> CreateUploadHeap(App& app, const std::vector<D3D12_RESOURCE_DESC>& resourceDescs, std::vector<UINT64>& gltfBufferOffsets)
{
    UINT64 uploadHeapSize = 0;

    for (const auto& desc : resourceDescs) {
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

void CreateDescriptorHeap(
    App& app,
    const tinygltf::Model& model,
    const std::span<ComPtr<ID3D12Resource>> textureResources,
    ComPtr<ID3D12DescriptorHeap>& outHeap,
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& outHandles
)
{
    const int NUM_CONSTANT_BUFFERS = 1;
    ID3D12DescriptorHeap* heap;
    int numDescriptors = NUM_CONSTANT_BUFFERS + model.textures.size();
    outHandles.reserve(numDescriptors);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = numDescriptors;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ASSERT_HRESULT(
        app.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&outHeap))
    );

    UINT incrementSize = app.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(outHeap->GetCPUDescriptorHandleForHeapStart(), 1, incrementSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(outHeap->GetGPUDescriptorHandleForHeapStart(), 1, incrementSize);
    for (int i = 0; i < textureResources.size(); i++) {
        auto& textureResource = textureResources[i];
        auto textureDesc = textureResource->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        app.device->CreateShaderResourceView(textureResource.Get(), &srvDesc, cpuHandle);
        outHandles.push_back((D3D12_GPU_DESCRIPTOR_HANDLE)gpuHandle);
        cpuHandle.Offset(1, incrementSize);
        gpuHandle.Offset(1, incrementSize);
    }
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

    assert(image.component == STBI_rgb_alpha);

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
    App& app,
    const tinygltf::Model& model,
    ComPtr<ID3D12Resource> uploadHeap,
    const std::vector<D3D12_RESOURCE_DESC>& resourceDescs,
    const std::vector<UINT64>& uploadOffsets,
    std::span<ComPtr<ID3D12Resource>>& outGeometryResources,
    std::span<ComPtr<ID3D12Resource>>& outTextureResources
)
{
    std::vector<ComPtr<ID3D12Resource>> resourceBuffers;
    resourceBuffers.reserve(model.buffers.size() + model.images.size());

    // Copy all GLTF buffer data to the upload heap
    size_t uploadHeapPosition = 0;
    UINT8* uploadHeapPtr;
    CD3DX12_RANGE readRange(0, 0);
    uploadHeap->Map(0, &readRange, reinterpret_cast<void**>(&uploadHeapPtr));
    int bufferIdx = 0;
    for (bufferIdx = 0; bufferIdx < model.buffers.size(); bufferIdx++) {
        auto buffer = model.buffers[bufferIdx];
        auto uploadHeapPosition = uploadOffsets[bufferIdx];
        memcpy(uploadHeapPtr + uploadHeapPosition, buffer.data.data(), buffer.data.size());
    }
    int imageIdx = 0;
    for (; bufferIdx < uploadOffsets.size(); imageIdx++, bufferIdx++)
    {
        auto image = model.images[imageIdx];
        auto uploadHeapPosition = uploadOffsets[bufferIdx];
        memcpy(uploadHeapPtr + uploadHeapPosition, image.image.data(), image.image.size());
    }
    uploadHeap->Unmap(0, nullptr);

    std::vector<CD3DX12_RESOURCE_BARRIER> resourceBarriers;

    // Copy all the gltf buffer data to a dedicated geometry buffer
    for (bufferIdx = 0; bufferIdx < model.buffers.size(); bufferIdx++) {
        const auto& gltfBuffer = model.buffers[bufferIdx];
        ComPtr<ID3D12Resource> geometryBuffer;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(gltfBuffer.data.size());
        auto resourceState = D3D12_RESOURCE_STATE_COPY_DEST;
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
    for (; bufferIdx < model.buffers.size() + model.images.size(); bufferIdx++) {
        const auto& gltfImage = model.images[bufferIdx - model.buffers.size()];
        ComPtr<ID3D12Resource> buffer;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = GetImageResourceDesc(gltfImage);
        auto resourceState = D3D12_RESOURCE_STATE_COPY_DEST;
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

    auto endGeometryBuffer = resourceBuffers.begin() + model.buffers.size();
    outGeometryResources = std::span(resourceBuffers.begin(), endGeometryBuffer);
    outTextureResources = std::span(endGeometryBuffer, resourceBuffers.end());

    app.commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());

    return resourceBuffers;
}

Model FinalizeModel(
    const tinygltf::Model& model,
    const std::vector<ComPtr<ID3D12Resource>>& resourceBuffers,
    const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& srvHandles
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

    Model result;

    result.buffers = resourceBuffers;

    // textures begin immediately after the geometry buffers
    int baseTextureIdx = model.buffers.size();

    std::vector<Mesh> meshes;
    meshes.reserve(model.meshes.size());
    for (const auto& mesh : model.meshes) {
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
            // doesn't allow us to have a one to one relationship between gltf buffer views 
            // and d3d buffer views.
            std::map<int, int> vertexStartOffsetToBufferView;

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
                    auto& accessor = model.accessors[accessorIdx];
                    desc.SemanticName = semanticName->c_str();
                    desc.SemanticIndex = semanticIndex;
                    desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                    desc.InstanceDataStepRate = 0;
                    int byteStride;
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
                    auto bufferView = model.bufferViews[bufferViewIdx];

                    byteStride = bufferView.byteStride > 0 ? bufferView.byteStride : byteStride;

                    auto buffer = resourceBuffers[bufferView.buffer];
                    int vertexStartOffset = bufferView.byteOffset + accessor.byteOffset - (accessor.byteOffset % byteStride);
                    int vertexStartAddress = buffer->GetGPUVirtualAddress() + vertexStartOffset;

                    desc.AlignedByteOffset = accessor.byteOffset - vertexStartOffset + bufferView.byteOffset;

                    // No d3d buffer view attached to this range of vertices yet, add one
                    if (!vertexStartOffsetToBufferView.contains(vertexStartAddress)) {
                        D3D12_VERTEX_BUFFER_VIEW view;
                        view.BufferLocation = vertexStartAddress;
                        view.SizeInBytes = accessor.count * byteStride;
                        view.StrideInBytes = byteStride;
                        vertexBufferViews.push_back(view);
                        vertexStartOffsetToBufferView[vertexStartAddress] = vertexBufferViews.size() - 1;
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

            if (primitive.material != -1)
            {
                auto material = model.materials[primitive.material];
                int texIdx = material.pbrMetallicRoughness.baseColorTexture.index;
                // int texcoord = material.pbrMetallicRoughness.baseColorTexture.texCoord;
                // int imageBufferIdx = baseTextureIdx + model.textures[texIdx].source;
                // auto resourceBuffer = resourceBuffers[imageBufferIdx];
                // D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
                // srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                // srvDesc.Format = resourceBuffer->GetDesc().Format;
                // srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                // srvDesc.Texture2D.MipLevels = 1;
                drawCall.srvHandle = srvHandles[texIdx];
            }

            {
                D3D12_INDEX_BUFFER_VIEW& ibv = drawCall.indexBufferView;
                int accessorIdx = primitive.indices;
                auto& accessor = model.accessors[accessorIdx];
                int indexBufferViewIdx = accessor.bufferView;
                auto bufferView = model.bufferViews[indexBufferViewIdx];
                ibv.BufferLocation = resourceBuffers[bufferView.buffer]->GetGPUVirtualAddress() + bufferView.byteOffset + accessor.byteOffset;
                ibv.SizeInBytes = bufferView.byteLength - accessor.byteOffset;
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
                drawCall.indexCount = accessor.count;
            }

            primitives.push_back(drawCall);
        }

        result.meshes.push_back(Mesh{ primitives });
    }

    return result;
}

void ProcessAssets(App& app, const AppAssets& assets)
{
    auto resourceDescs = CreateResourceDescriptions(assets.gltfModel);

    std::vector<UINT64> uploadOffsets;
    auto uploadHeap = CreateUploadHeap(app, resourceDescs, uploadOffsets);

    std::span<ComPtr<ID3D12Resource>> geometryBuffers;
    std::span<ComPtr<ID3D12Resource>> textureBuffers;

    // Can only call this ONCE before command list executed
    // This will need to be adapted to handle N models.
    auto resourceBuffers = UploadModelBuffers(
        app,
        assets.gltfModel,
        uploadHeap,
        resourceDescs,
        uploadOffsets,
        geometryBuffers,
        textureBuffers
    );

    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> srvHandles;
    ComPtr<ID3D12DescriptorHeap> mainDescriptorHeap;
    CreateDescriptorHeap(app, assets.gltfModel, textureBuffers, mainDescriptorHeap, srvHandles);
    app.mainDescriptorHeap = mainDescriptorHeap;

    app.model = FinalizeModel(assets.gltfModel, resourceBuffers, srvHandles);
    app.model.transform = glm::scale(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0)), glm::vec3(10.0f));

    for (auto& mesh : app.model.meshes) {
        for (auto& primitive : mesh.primitives) {
            primitive.PSO = CreateDiffuseMeshPSO(
                app.device.Get(),
                app.dataDir,
                app.rootSignature.Get(),
                primitive.inputLayout
            );
        }
    }

    app.commandList->Close();

    ID3D12CommandList* ppCommandLists[] = { app.commandList.Get() };
    app.commandQueue->ExecuteCommandLists(1, ppCommandLists);

    WaitForPreviousFrame(app);
}

void LoadScene(App& app, const AppAssets& assets)
{
    ID3D12Device* device = app.device.Get();
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
        {
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
        CD3DX12_ROOT_PARAMETER1 rootParameters[2];

        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
        rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX);
        rootParameters[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        // D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        // D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        // D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        // D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

        // FIXME: This is going to have to be dynamic...
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.MipLODBias = 0;
        sampler.MaxAnisotropy = 0;
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
        ASSERT_HRESULT(
            D3DX12SerializeVersionedRootSignature(
                &rootSignatureDesc,
                featureData.HighestVersion,
                &signature,
                &error
            )
        );
        ASSERT_HRESULT(
            device->CreateRootSignature(
                0,
                signature->GetBufferPointer(),
                signature->GetBufferSize(),
                IID_PPV_ARGS(&app.rootSignature)
            )
        );
    }

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

    float aspectRatio = (float)app.windowWidth / (float)app.windowHeight;

    {
        app.fence.Initialize(app.device.Get());
        app.copyFence.Initialize(app.device.Get());
    }

    ProcessAssets(app, assets);

    {
        const UINT constantBufferSize = sizeof(ConstantBufferData);
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
        ASSERT_HRESULT(
            device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&app.constantBuffer)
            )
        );

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = app.constantBuffer->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = constantBufferSize;
        device->CreateConstantBufferView(
            &cbvDesc,
            app.mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart()
        );

        CD3DX12_RANGE readRange(0, 0);
        ASSERT_HRESULT(
            app.constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&app.constantBufferDataPtr))
        );
    }

    UpdateConstantBuffer(app.constantBufferData, app.constantBufferDataPtr);

    WaitForPreviousFrame(app);
}

void UpdateScene(App& app)
{
    float timeSeconds = (float)high_resolution_clock::now().time_since_epoch().count() / (float)1e9;

    glm::mat4 translation = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, sin(timeSeconds) * 5.0f, 0.0f));
    glm::mat4 scale = glm::scale(translation, glm::vec3(10.0f));
    glm::mat4 rotation = glm::rotate(scale, glm::radians(0.0f), glm::vec3(1, 0, 0));
    app.model.transform = rotation;

    glm::mat4 projection = glm::perspective(glm::pi<float>() * 0.2f, (float)app.windowWidth / (float)app.windowHeight, 0.01f, 5000.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, -100.0f), glm::vec3(0, 0.0f, 0), glm::vec3(0, 1, 0));
    glm::mat4 model = app.model.transform;
    app.constantBufferData.MVP = projection * view * model;

    UpdateConstantBuffer(app.constantBufferData, app.constantBufferDataPtr);
}

void DrawModelCommandList(const Model& model, ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* mainDescriptorHeap)
{
    ID3D12DescriptorHeap* ppHeaps[] = { mainDescriptorHeap };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    commandList->SetGraphicsRootDescriptorTable(0, mainDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    for (const auto& mesh : model.meshes) {
        for (const auto& primitive : mesh.primitives) {
            commandList->SetGraphicsRootDescriptorTable(1, primitive.srvHandle);
            commandList->IASetPrimitiveTopology(primitive.primitiveTopology);
            commandList->SetPipelineState(primitive.PSO.Get());
            commandList->IASetVertexBuffers(0, primitive.vertexBufferViews.size(), primitive.vertexBufferViews.data());
            commandList->IASetIndexBuffer(&primitive.indexBufferView);
            commandList->DrawIndexedInstanced(primitive.indexCount, 1, 0, 0, 0);
        }
    }
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

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.renderTargets[app.frameIdx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(app.rtvHeap->GetCPUDescriptorHandleForHeapStart(), app.frameIdx, app.rtvDescriptorSize);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Record commands.
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    DrawModelCommandList(app.model, commandList, app.mainDescriptorHeap.Get());

    // Indicate that the back buffer will now be used to present.
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.renderTargets[app.frameIdx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    commandList->ResourceBarrier(1, &barrier);

    ASSERT_HRESULT(
        commandList->Close()
    );
}

void RenderFrame(App& app)
{
    BuildCommandList(app);

    ID3D12CommandList* ppCommandLists[] = { app.commandList.Get() };
    app.commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    ASSERT_HRESULT(
        app.swapChain->Present(1, 0)
    );

    WaitForPreviousFrame(app);
}

int RunApp(int argc, char** argv)
{
    App app;

    InitApp(app, argc, argv);
    InitWindow(app);
    InitD3D(app);

    {
        AppAssets assets = LoadAssets(app);
        LoadScene(app, assets);
    }

    SDL_Event e;

    bool running = true;
    while (running) {
        while (SDL_PollEvent(&e) > 0) {
            if (e.type == SDL_QUIT) {
                running = false;
            }
        }

        UpdateScene(app);
        RenderFrame(app);
    }

    WaitForPreviousFrame(app);

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
