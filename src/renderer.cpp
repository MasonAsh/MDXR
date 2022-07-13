#include "renderer.h"
#include "app.h"
#include "assets.h"
#include "gui.h"
#include "d3dutils.h"

#include <directx/d3dx12.h>
#include <pix3.h>

void SetupCubemapData(App& app)
{
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(PerPrimitiveConstantData) * CubeImage_Count);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    ASSERT_HRESULT(app.mainAllocator->CreateResource(
        &allocDesc,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        &app.cubemapPerPrimitiveBuffer,
        IID_NULL, nullptr
    ));
}

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

void SetupRenderTargets(App& app, bool isResize)
{
    // Allocate descriptors. These remain for the lifetime of the app so no need to manage lifetimes.
    DescriptorAlloc frameBufferDescriptor;
    if (!isResize) {
        frameBufferDescriptor = app.rtvDescriptorPool.AllocateDescriptors(FrameBufferCount, "FrameBuffer RTVs");
    }

    for (int i = 0; i < FrameBufferCount; i++) {
        ASSERT_HRESULT(
            app.swapChain->GetBuffer(i, IID_PPV_ARGS(&app.renderTargets[i]))
        );

        if (!isResize) {
            app.frameBufferRTVs[i] = frameBufferDescriptor.Ref(i);
        }

        app.device->CreateRenderTargetView(app.renderTargets[i].Get(), nullptr, app.frameBufferRTVs[i].CPUHandle());
    }
}

void SetupGBuffer(App& app, bool isResize)
{
    DescriptorRef rtvs;
    if (!isResize) {
        // Create SRV heap for the render targets
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = (UINT)GBuffer_Count + MaxLightCount + 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        //app.lightingPassDescriptorArena.Initialize(app.device.Get(), heapDesc, "LightPassArena");
        app.GBuffer.baseSrvReference = AllocateDescriptorsUnique(app.descriptorPool, GBuffer_Count, "GBuffer SRVs");
        rtvs = app.rtvDescriptorPool.AllocateDescriptors(GBuffer_RTVCount, "GBuffer RTVs").Ref();
    } else {
        // If we're resizing then we need to release existing gbuffer
        for (auto& renderTarget : app.GBuffer.renderTargets) {
            renderTarget = nullptr;
        }

        rtvs = app.GBuffer.rtvs[0];
    }

    auto rtvHandle = rtvs.CPUHandle();
    CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle = app.GBuffer.baseSrvReference.CPUHandle();
    for (int i = 0; i < GBuffer_Depth; i++) {
        if (!isResize) {
            app.GBuffer.rtvs[i] = rtvs + i;
        }

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

    SetupRenderTargets(app, true);
    SetupDepthStencil(app, true);
    SetupGBuffer(app, true);

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();
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
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 8;
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
    auto descriptorHandle = AllocateDescriptorsUnique(app.descriptorPool, MaxLightCount + 1, "light pass and light buffer");

    CreateConstantBufferAndViews(
        app.device.Get(),
        app.LightBuffer.constantBuffer,
        sizeof(LightConstantData),
        MaxLightCount + 1,
        descriptorHandle.CPUHandle()
    );

    app.LightBuffer.constantBuffer->Map(0, nullptr, (void**)&app.LightBuffer.passData);
    // Lights are stored immediately after the pass data
    app.LightBuffer.lightConstantData = reinterpret_cast<LightConstantData*>(app.LightBuffer.passData + 1);
    app.LightBuffer.cbvHandle = std::move(descriptorHandle);

    app.LightBuffer.passData->baseGBufferIndex = app.GBuffer.baseSrvReference.Index();
    app.LightBuffer.passData->environmentIntensity = glm::vec4(1.0f);

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
    app.descriptorPool.Initialize(app.device.Get(), heapDesc, "GBufferPassArena");

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

void InitD3D(App& app)
{
    if (app.gpuDebug) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            ComPtr<ID3D12Debug1> debugController1;
            ASSERT_HRESULT(debugController->QueryInterface(IID_PPV_ARGS(&debugController1)));
            if (debugController1) {
                debugController1->SetEnableGPUBasedValidation(true);
            }
        } else {
            std::cout << "Failed to enable D3D12 debug layer\n";
        }

        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> pDredSettings;
        D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings));
        if (pDredSettings) {
            pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        } else {
            std::cout << "Failed to load DRED\n";
        }

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
        if (infoQueue) {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
        } else {
            std::cout << "Failed to set info queue breakpoints\n";
        }
    }

    D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
    allocatorDesc.pDevice = device;
    allocatorDesc.pAdapter = adapter.Get();
    ASSERT_HRESULT(
        D3D12MA::CreateAllocator(&allocatorDesc, &app.mainAllocator)
    );

    PrintCapabilities(device, adapter.Get());

    app.graphicsQueue.Initialize(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    app.copyQueue.Initialize(device, D3D12_COMMAND_LIST_TYPE_COPY);
    app.computeQueue.Initialize(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);

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
                app.graphicsQueue.GetInternal(),
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
        app.rtvDescriptorPool.Initialize(app.device.Get(), rtvHeapDesc, "RTV Heap Arena");
    }

    G_IncrementSizes.CbvSrvUav = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    G_IncrementSizes.Rtv = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    SetupRenderTargets(app, false);
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

        ASSERT_HRESULT(app.commandList->Close());
        ASSERT_HRESULT(app.copyCommandList->Close());
    }

    SetupCubemapData(app);
}

void CreateGPUBufferWithData(
    ID3D12Device* device,
    D3D12MA::Allocator* allocator,
    void* inData, size_t dataSize,
    ID3D12CommandQueue* queue,
    ComPtr<D3D12MA::Allocation>& allocation
)
{
    ComPtr<D3D12MA::Allocation> uploadBuffer;

    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    ASSERT_HRESULT(allocator->CreateResource(
        &allocDesc,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        &uploadBuffer,
        IID_NULL, nullptr
    ));

    void* dataPtr;
    uploadBuffer->GetResource()->Map(0, nullptr, &dataPtr);

    memcpy(dataPtr, inData, dataSize);
}

void UpdatePerPrimitiveConstantBuffers(App& app, const glm::mat4& projection, const glm::mat4& view)
{
    glm::mat4 viewProjection = projection * view;

    auto meshIter = app.meshPool.Begin();

    while (meshIter) {
        // Diffuse irradiance uses the primitive constant buffer before its ready to render
        if (!meshIter->isReadyForRender) {
            continue;
        }

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

    for (UINT i = 0; i < app.LightBuffer.count; i++) {
        app.lights[i].UpdateConstantData(view);
    }
}

void UpdateRenderData(App& app, const glm::mat4& projection, const glm::mat4& view)
{
    UpdateLightConstantBuffers(app, projection, view);
    UpdatePerPrimitiveConstantBuffers(app, projection, view);
}

void DrawMeshesGBuffer(App& app, ID3D12GraphicsCommandList* commandList)
{
    auto meshIter = app.meshPool.Begin();

    while (meshIter) {
        if (!meshIter->isReadyForRender) {
            meshIter = app.meshPool.Next(meshIter);
            continue;
        }

        for (const auto& primitive : meshIter->primitives) {
            const auto& material = primitive->material.get();
            // FIXME: if I am not lazy I will SORT by material type
            // Transparent materials drawn in different pass
            DescriptorRef materialDescriptor;

            if (material) {
                if (material->materialType == MaterialType_AlphaBlendPBR) {
                    continue;
                }
                materialDescriptor = material->cbvDescriptor.Ref();
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
        if (!mesh->isReadyForRender) {
            meshIter = app.meshPool.Next(meshIter);
            continue;
        }

        for (const auto& primitive : mesh->primitives) {
            auto material = primitive->material.get();
            // FIXME: if I am not lazy I will SORT by material type
            // Only draw alpha blended materials in this pass.
            if (!material || material->materialType != MaterialType_AlphaBlendPBR) {
                continue;
            }
            auto materialDescriptor = material->cbvDescriptor.Ref();

            for (UINT lightIdx = 0; lightIdx < app.LightBuffer.count; lightIdx++) {
                UINT lightDescriptorIndex = app.LightBuffer.cbvHandle.Index() + lightIdx + 1u;
                // Set the per-primitive constant buffer
                UINT constantValues[5] = { primitive->perPrimitiveDescriptor.index, materialDescriptor.Index(), lightDescriptorIndex, 0, primitive->miscDescriptorParameter.index };
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

    ID3D12DescriptorHeap* mainDescriptorHeap = app.descriptorPool.Heap();
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
        auto descriptorHeap = app.descriptorPool.Heap();
        ID3D12DescriptorHeap* ppHeaps[] = { descriptorHeap };
        commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
        // Root signature must be set AFTER heaps are set when CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED is set.
        commandList->SetGraphicsRootSignature(app.rootSignature.Get());

        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        // Point lights
        commandList->SetPipelineState(app.LightPass.pointLightPSO->Get());
        for (UINT i = 0; i < app.LightBuffer.count; i++) {
            if (app.lights[i].lightType == LightType_Point) {
                UINT constantValues[2] = {
                    app.LightBuffer.cbvHandle.Index() + i + 1,
                    app.LightBuffer.cbvHandle.Index()
                };
                commandList->SetGraphicsRoot32BitConstants(0, 2, constantValues, 2);
                DrawFullscreenQuad(app, commandList);
            }
        }

        // Directional lights
        commandList->SetPipelineState(app.LightPass.directionalLightPso->Get());
        for (UINT i = 0; i < app.LightBuffer.count; i++) {
            if (app.lights[i].lightType == LightType_Directional) {
                UINT constantValues[2] = {
                    app.LightBuffer.cbvHandle.Index() + i + 1,
                    app.LightBuffer.cbvHandle.Index()
                };
                commandList->SetGraphicsRoot32BitConstants(0, 2, constantValues, 2);
                DrawFullscreenQuad(app, commandList);
            }
        }

        // Environment cubemap
        if (app.Skybox.prefilterMapSRV.IsValid()) {
            commandList->SetPipelineState(app.LightPass.environentCubemapLightPso->Get());
            UINT constantValues[5] = {
                UINT_MAX,
                app.Skybox.brdfLUTDescriptor.Index(),
                UINT_MAX,
                app.LightBuffer.cbvHandle.Index(),
                app.Skybox.prefilterMapSRV.Index(),
            };
            commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
            DrawFullscreenQuad(app, commandList);
        }
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

    ID3D12DescriptorHeap* mainDescriptorHeap = app.descriptorPool.Heap();
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

    FenceEvent renderEvent;

    // FIXME: multithreading
    HRESULT hr = app.graphicsQueue.ExecuteCommandListsAndPresentBlocking({ app.commandList.Get() }, app.swapChain.Get(), 0, DXGI_PRESENT_ALLOW_TEARING);
    if (!SUCCEEDED(hr)) {
        tdrOccurred = true;
        app.running = false;
        DebugLog() << "TDR occurred\n";
    }

    app.graphicsQueue.WaitForEventCPU(renderEvent);

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();
}

void EnsureRenderingFinished(App& app)
{
}