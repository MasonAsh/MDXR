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
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    CD3DX12_VIEWPORT viewport;
    CD3DX12_RECT scissorRect;

    ComPtr<ID3D12CommandQueue> copyCommandQueue;

    ComPtr<ID3D12PipelineState> unlitMeshPSO;

    ComPtr<ID3D12DescriptorHeap> cbvHeap;
    ConstantBufferData constantBufferData;
    ComPtr<ID3D12Resource> constantBuffer;
    UINT8* constantBufferDataPtr = nullptr;

    Model model;

    unsigned int frameIdx;
    unsigned int rtvDescriptorSize = 0;
    unsigned int fenceValue;
    HANDLE fenceEvent;
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
        cbvHeapDesc.NumDescriptors = 1;
        cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        ASSERT_HRESULT(
            device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&app.cbvHeap))
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
    app.scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(app.windowHeight), static_cast<LONG>(app.windowHeight));
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

ComPtr<ID3D12PipelineState> CreateUnlitMeshPSO(
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    std::wstring wDataDir = convert_to_wstring(dataDir);

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ASSERT_HRESULT(
        D3DReadFileToBlob((wDataDir + L"/unlit_mesh.cvert").c_str(), &vertexShader)
    );
    ASSERT_HRESULT(
        D3DReadFileToBlob((wDataDir + L"/unlit_mesh.cpixel").c_str(), &pixelShader)
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

void UpdateConstantBufferData(ConstantBufferData& constantBufferData, int windowWidth, int windowHeight)
{
    glm::mat4 projection = glm::perspective(glm::pi<float>() * 0.2f, (float)windowWidth / (float)windowHeight, 0.01f, 5000.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 100.0f, -1.0f), glm::vec3(0, 0.0f, 0), glm::vec3(0, 1, 0));
    glm::mat4 model = glm::scale(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0)), glm::vec3(10.0f));
    constantBufferData.MVP = projection * view * model;
}

void WaitForPreviousFrame(App& app)
{
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity. More advanced samples 
    // illustrate how to use fences for efficient resource usage.

    // Signal and increment the fence value.
    const UINT64 currentFenceValue = app.fenceValue;
    ASSERT_HRESULT(
        app.commandQueue->Signal(app.fence.Get(), currentFenceValue)
    );
    app.fenceValue++;

    // Wait until the previous frame is finished.
    if (app.fence->GetCompletedValue() < currentFenceValue)
    {
        ASSERT_HRESULT(
            app.fence->SetEventOnCompletion(currentFenceValue, app.fenceEvent)
        );

        WaitForSingleObject(app.fenceEvent, INFINITE);
    }

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();
}

void LoadScene(App& app)
{
    ID3D12Device* device = app.device.Get();
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
        {
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        }

        CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
        CD3DX12_ROOT_PARAMETER1 rootParameters[1];

        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
        rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX);

        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init_1_1(
            _countof(rootParameters),
            rootParameters,
            0,
            nullptr,
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

    {
        const UINT constantBufferSize = sizeof(ConstantBufferData);

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        device->CreateConstantBufferView(&cbvDesc, app.cbvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    app.model = LoadGLTF(device, app.dataDir + "/Suzanne.gltf");
    for (auto& mesh : app.model.meshes) {
        for (auto& primitive : mesh.primitives) {
            primitive.PSO = CreateUnlitMeshPSO(device, app.dataDir, app.rootSignature.Get(), primitive.inputLayout);
        }
    }

    ASSERT_HRESULT(
        device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            app.commandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&app.commandList)
        )
    );

    ID3D12GraphicsCommandList* commandList = app.commandList.Get();

    ASSERT_HRESULT(
        commandList->Close()
    );

    float aspectRatio = (float)app.windowWidth / (float)app.windowHeight;

    UpdateConstantBufferData(
        app.constantBufferData,
        app.windowWidth,
        app.windowHeight
    );

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
            app.cbvHeap->GetCPUDescriptorHandleForHeapStart()
        );

        CD3DX12_RANGE readRange(0, 0);
        ASSERT_HRESULT(
            app.constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&app.constantBufferDataPtr))
        );
        memcpy(app.constantBufferDataPtr, &app.constantBufferData, sizeof(app.constantBufferData));
    }

    {
        ASSERT_HRESULT(
            device->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&app.fence)
            )
        );
        app.fenceValue = 1;

        app.fenceEvent = CreateEvent(nullptr, false, false, nullptr);
        if (app.fenceEvent == nullptr) {
            ASSERT_HRESULT(
                HRESULT_FROM_WIN32(
                    GetLastError()
                )
            );
        }
    }

    WaitForPreviousFrame(app);
}

void DrawModelCommandList(const Model& model, ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* cbvHeap)
{
    ID3D12DescriptorHeap* ppHeaps[] = { cbvHeap };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    commandList->SetGraphicsRootDescriptorTable(0, cbvHeap->GetGPUDescriptorHandleForHeapStart());
    for (const auto& mesh : model.meshes) {
        for (const auto& primitive : mesh.primitives) {
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

    DrawModelCommandList(app.model, commandList, app.cbvHeap.Get());

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

    LoadScene(app);

    SDL_Event e;

    bool running = true;
    while (running) {
        while (SDL_PollEvent(&e) > 0) {
            if (e.type == SDL_QUIT) {
                running = false;
            }
        }

        RenderFrame(app);
    }

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
