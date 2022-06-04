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

using namespace DirectX;

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT4 color;
};

App::App(int argc, char** argv) :
    viewport(0.0f, 0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight)),
    scissorRect(0, 0, static_cast<LONG>(windowHeight), static_cast<LONG>(windowHeight)),
    rtvDescriptorSize(0)
{
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--datadir")) {
            if (i + 1 < argc) {
                dataDir = argv[i + 1];
            }
        }
    }

    if (dataDir.empty()) {
        std::cout << "Error: no data directory specified\n";
        std::cout << "The program will now exit" << std::endl;
        abort();
    }

    wDataDir = convert_to_wstring(dataDir);
}

App::~App()
{
    SDL_DestroyWindow(window);
}

int App::Run()
{
    Initialize();

    SDL_Event e;

    bool running = true;
    while (running) {
        while (SDL_PollEvent(&e) > 0) {
            if (e.type == SDL_QUIT) {
                running = false;
            }
        }

        Render();
    }

    return 0;
}

void App::Initialize()
{
    InitWindow();
    InitD3D();
    LoadScene();
}

void App::InitWindow()
{
    window = SDL_CreateWindow("MDXR",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        windowWidth, windowHeight,
        0
    );


    assert(window);

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    assert(SDL_GetWindowWMInfo(window, &wmInfo));
    hwnd = wmInfo.info.win.window;
}

void App::InitD3D()
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

    IDXGIAdapter1* adapter;
    ASSERT_HRESULT(
        dxgiFactory->EnumAdapters1(0, &adapter)
    );

    ASSERT_HRESULT(
        D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))
    );


    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ASSERT_HRESULT(
        device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue))
    );

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = FrameBufferCount;
    swapChainDesc.BufferDesc.Width = windowWidth;
    swapChainDesc.BufferDesc.Height = windowHeight;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Windowed = TRUE;

    {
        ComPtr<IDXGISwapChain> swapChain;
        ASSERT_HRESULT(
            dxgiFactory->CreateSwapChain(
                commandQueue.Get(),
                &swapChainDesc,
                &swapChain
            )
        );
        ASSERT_HRESULT(swapChain.As(&this->swapChain));
    }

    frameIdx = swapChain->GetCurrentBackBufferIndex();

    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameBufferCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ASSERT_HRESULT(
            device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap))
        );

        D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = {};
        cbvHeapDesc.NumDescriptors = 1;
        cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        ASSERT_HRESULT(
            device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&cbvHeap))
        );
    }


    rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (int i = 0; i < FrameBufferCount; i++) {
        ASSERT_HRESULT(
            swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i]))
        );

        device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, rtvDescriptorSize);
    }

    ASSERT_HRESULT(
        device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&commandAllocator)
        )
    );
}

void App::CreateUnlitMeshPSO(Primitive& primitive)
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
    psoDesc.InputLayout = { primitive.inputLayout.data(), (UINT)primitive.inputLayout.size() };
    psoDesc.pRootSignature = rootSignature.Get();
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

    ASSERT_HRESULT(
        device->CreateGraphicsPipelineState(
            &psoDesc,
            IID_PPV_ARGS(&primitive.PSO)
        )
    );
}

void App::LoadScene()
{
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
                IID_PPV_ARGS(&rootSignature)
            )
        );
    }

    {
        std::wstring wDataDir = convert_to_wstring(dataDir);

        // ComPtr<ID3DBlob> vertexShader;
        // ComPtr<ID3DBlob> pixelShader;
        // ASSERT_HRESULT(
        //     D3DReadFileToBlob((wDataDir + L"/basic.cvert").c_str(), &vertexShader)
        // );
        // ASSERT_HRESULT(
        //     D3DReadFileToBlob((wDataDir + L"/basic.cpixel").c_str(), &pixelShader)
        // );

        // D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
        // {
        //     { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        //     { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        // };

        // // Describe and create the graphics pipeline state object (PSO).
        // D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        // psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        // psoDesc.pRootSignature = rootSignature.Get();
        // psoDesc.VS = { reinterpret_cast<UINT8*>(vertexShader->GetBufferPointer()), vertexShader->GetBufferSize() };
        // psoDesc.PS = { reinterpret_cast<UINT8*>(pixelShader->GetBufferPointer()), pixelShader->GetBufferSize() };
        // psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        // psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        // psoDesc.DepthStencilState.DepthEnable = FALSE;
        // psoDesc.DepthStencilState.StencilEnable = FALSE;
        // psoDesc.SampleMask = UINT_MAX;
        // psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        // psoDesc.NumRenderTargets = 1;
        // psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        // psoDesc.SampleDesc.Count = 1;

        // ASSERT_HRESULT(
        //     device->CreateGraphicsPipelineState(
        //         &psoDesc,
        //         IID_PPV_ARGS(&pipelineState)
        //     )
        // );
    }

    {
        const UINT constantBufferSize = sizeof(ConstantBufferData);

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        device->CreateConstantBufferView(&cbvDesc, cbvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    model = LoadGLTF(device.Get(), dataDir + "/Suzanne.gltf");
    for (auto& mesh : model.meshes) {
        for (auto& primitive : mesh.primitives) {
            CreateUnlitMeshPSO(primitive);
        }
    }

    ASSERT_HRESULT(
        device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            commandAllocator.Get(),
            pipelineState.Get(),
            IID_PPV_ARGS(&commandList)
        )
    );

    ASSERT_HRESULT(
        commandList->Close()
    );

    float aspectRatio = (float)windowWidth / (float)windowHeight;

    {
        // Define the geometry for a triangle.
        Vertex triangleVertices[] =
        {
            { { 0.0f, 0.25f * aspectRatio, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
            { { 0.25f, -0.25f * aspectRatio, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
            { { -0.25f, -0.25f * aspectRatio, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }
        };

        const UINT vertexBufferSize = sizeof(triangleVertices);

        // Note: using upload heaps to transfer static data like vert buffers is not 
        // recommended. Every time the GPU needs it, the upload heap will be marshalled 
        // over. Please read up on Default Heap usage. An upload heap is used here for 
        // code simplicity and because there are very few verts to actually transfer.
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
        ASSERT_HRESULT(
            device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&vertexBuffer)
            )
        );

        // Copy the triangle data to the vertex buffer.
        UINT8* pVertexDataBegin;
        CD3DX12_RANGE readRange(0, 0);
        ASSERT_HRESULT(
            vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin))
        );
        memcpy(pVertexDataBegin, triangleVertices, sizeof(triangleVertices));
        vertexBuffer->Unmap(0, nullptr);

        // Initialize the vertex buffer view.
        vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.StrideInBytes = sizeof(Vertex);
        vertexBufferView.SizeInBytes = vertexBufferSize;
    }

    UpdateConstantBufferData();

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
                IID_PPV_ARGS(&constantBuffer)
            )
        );

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = constantBufferSize;
        device->CreateConstantBufferView(
            &cbvDesc,
            cbvHeap->GetCPUDescriptorHandleForHeapStart()
        );

        CD3DX12_RANGE readRange(0, 0);
        ASSERT_HRESULT(
            constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&constantBufferDataPtr))
        );
        memcpy(constantBufferDataPtr, &constantBufferData, sizeof(constantBufferData));
    }

    {
        ASSERT_HRESULT(
            device->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&fence)
            )
        );
        fenceValue = 1;

        fenceEvent = CreateEvent(nullptr, false, false, nullptr);
        if (fenceEvent == nullptr) {
            ASSERT_HRESULT(
                HRESULT_FROM_WIN32(
                    GetLastError()
                )
            );
        }
    }

    WaitForPreviousFrame();
}

void App::WaitForPreviousFrame()
{
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity. More advanced samples 
    // illustrate how to use fences for efficient resource usage.

    // Signal and increment the fence value.
    const UINT64 currentFenceValue = fenceValue;
    ASSERT_HRESULT(
        commandQueue->Signal(fence.Get(), currentFenceValue)
    );
    fenceValue++;

    // Wait until the previous frame is finished.
    if (fence->GetCompletedValue() < currentFenceValue)
    {
        ASSERT_HRESULT(
            fence->SetEventOnCompletion(currentFenceValue, fenceEvent)
        );

        WaitForSingleObject(fenceEvent, INFINITE);
    }

    frameIdx = swapChain->GetCurrentBackBufferIndex();
}

void App::UpdateConstantBufferData()
{
    glm::mat4 projection = glm::perspective(glm::pi<float>() * 0.2f, (float)windowWidth / (float)windowHeight, 0.01f, 5000.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 100.0f, -1.0f), glm::vec3(0, 0.0f, 0), glm::vec3(0, 1, 0));
    glm::mat4 model = glm::scale(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0)), glm::vec3(10.0f));
    constantBufferData.MVP = projection * view * model;
}

void App::DrawModel(const Model& model, ID3D12GraphicsCommandList* commandList)
{
    ID3D12DescriptorHeap* ppHeaps[] = { cbvHeap.Get() };
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

void App::BuildCommandList()
{
    ASSERT_HRESULT(
        commandAllocator->Reset()
    );

    ASSERT_HRESULT(
        commandList->Reset(commandAllocator.Get(), pipelineState.Get())
    );

    commandList->SetGraphicsRootSignature(rootSignature.Get());

    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIdx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart(), frameIdx, rtvDescriptorSize);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Record commands.
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    // commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    // commandList->DrawInstanced(3, 1, 0, 0);

    DrawModel(model, commandList.Get());

    // Indicate that the back buffer will now be used to present.
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIdx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    commandList->ResourceBarrier(1, &barrier);

    ASSERT_HRESULT(
        commandList->Close()
    );
}


void App::Render()
{
    BuildCommandList();

    ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    ASSERT_HRESULT(
        swapChain->Present(1, 0)
    );

    WaitForPreviousFrame();
}