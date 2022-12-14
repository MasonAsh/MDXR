#include "renderer.h"
#include "app.h"
#include "assets.h"
#include "gui.h"
#include "d3dutils.h"

#include <directx/d3dx12.h>
#include <pix3.h>

std::scoped_lock<std::mutex> LockRenderThread(App& app)
{
    return std::scoped_lock<std::mutex>(app.renderFrameMutex);
}

void SetupDepthStencil(App& app, bool isResize)
{
    if (!isResize) {
        app.depthStencilDescriptor = AllocateDescriptorsUnique(app.dsvDescriptorPool, 1, "Main DepthStencilView");
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsDesc = {};
    dsDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsDesc.Flags = D3D12_DSV_FLAG_NONE;

    D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
    depthOptimizedClearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;

    ComPtr<ID3D12Resource> depthStencilBuffer;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R24G8_TYPELESS, app.windowWidth, app.windowHeight, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
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
        app.depthStencilDescriptor.CPUHandle()
    );
}

void SetupBloomPass(App& app)
{
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    app.Bloom.threshold = 1.0f;

    app.Bloom.PingPong[0].texture = nullptr;
    app.Bloom.PingPong[1].texture = nullptr;

    auto resourceDesc = GBufferResourceDesc(GBuffer_Radiance, 1024, 1024);

    ASSERT_HRESULT(app.mainAllocator->CreateResource(
        &allocDesc,
        &resourceDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr,
        &app.Bloom.PingPong[0].texture,
        IID_NULL, nullptr
    ));
    app.Bloom.PingPong[0].srv = AllocateDescriptorsUnique(app.descriptorPool, 1, "Bloom.PingPong[0].srv");
    app.Bloom.PingPong[0].rtv = AllocateDescriptorsUnique(app.rtvDescriptorPool, 1, "Bloom.PingPong[0].rtv");
    app.device->CreateShaderResourceView(
        app.Bloom.PingPong[0].texture->GetResource(),
        nullptr,
        app.Bloom.PingPong[0].srv.CPUHandle()
    );
    app.device->CreateRenderTargetView(
        app.Bloom.PingPong[0].texture->GetResource(),
        nullptr,
        app.Bloom.PingPong[0].rtv.CPUHandle()
    );

    ASSERT_HRESULT(app.mainAllocator->CreateResource(
        &allocDesc,
        &resourceDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr,
        &app.Bloom.PingPong[1].texture,
        IID_NULL, nullptr
    ));
    app.Bloom.PingPong[1].srv = AllocateDescriptorsUnique(app.descriptorPool, 1, "Bloom.PingPong[1].srv");
    app.Bloom.PingPong[1].rtv = AllocateDescriptorsUnique(app.rtvDescriptorPool, 1, "Bloom.PingPong[1].rtv");
    app.device->CreateShaderResourceView(
        app.Bloom.PingPong[1].texture->GetResource(),
        nullptr,
        app.Bloom.PingPong[1].srv.CPUHandle()
    );
    app.device->CreateRenderTargetView(
        app.Bloom.PingPong[1].texture->GetResource(),
        nullptr,
        app.Bloom.PingPong[1].rtv.CPUHandle()
    );

    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    app.Bloom.filterPSO = CreateBloomFilterPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );
    app.Bloom.blurPSO = CreateBloomBlurPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );
    app.Bloom.applyPSO = CreateBloomApplyPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );
}

void SetupRenderTargets(App& app, bool isResize)
{
    // Allocate descriptors. These remain for the lifetime of the app so no need to manage lifetimes.
    DescriptorAlloc frameBufferDescriptor;
    DescriptorAlloc nonSRGBFrameBufferDescriptor;
    if (!isResize) {
        frameBufferDescriptor = app.rtvDescriptorPool.AllocateDescriptors(FrameBufferCount, "FrameBuffer RTVs");
        nonSRGBFrameBufferDescriptor = app.rtvDescriptorPool.AllocateDescriptors(FrameBufferCount, "FrameBuffer RTVs (non-SRGB)");
    }

    for (int i = 0; i < FrameBufferCount; i++) {
        ASSERT_HRESULT(
            app.swapChain->GetBuffer(i, IID_PPV_ARGS(&app.renderTargets[i]))
        );

        if (!isResize) {
            app.frameBufferRTVs[i] = frameBufferDescriptor.Ref(i);
            app.nonSRGBFrameBufferRTVs[i] = nonSRGBFrameBufferDescriptor.Ref(i);
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        app.device->CreateRenderTargetView(app.renderTargets[i].Get(), &rtvDesc, app.frameBufferRTVs[i].CPUHandle());

        // ImGui doesn't know how to handle an SRGB backbuffer, so create a separate view for ImGUI
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        app.device->CreateRenderTargetView(app.renderTargets[i].Get(), &rtvDesc, app.nonSRGBFrameBufferRTVs[i].CPUHandle());
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
    depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // Can't use D32_FLOAT with SRVs...
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MipLevels = 1;
    app.device->CreateShaderResourceView(app.depthStencilBuffer.Get(), &depthSrvDesc, srvHandle);
}

void SetupCursorColorDebug(App& app)
{
    D3D12MA::ALLOCATION_DESC readbackProperties = {};
    readbackProperties.HeapType = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.Height = 1;
    resourceDesc.Width = sizeof(float) * 4;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.MipLevels = 1;
    resourceDesc.SampleDesc.Count = 1;

    ASSERT_HRESULT(
        app.mainAllocator->CreateResource(
            &readbackProperties,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            &app.CursorColorDebug.readbackBuffer,
            IID_NULL, nullptr
        )
    );

    app.CursorColorDebug.lastRGBA = glm::vec4(0);
}

void HandleResize(App& app, int newWidth, int newHeight)
{
    // Release references to the buffers before resizing.
    for (auto& renderTarget : app.renderTargets) {
        renderTarget = nullptr;
    }

    auto lock = LockRenderThread(app);
    app.graphicsQueue.WaitForEventCPU(app.previousFrameEvent);

    app.swapChain->ResizeBuffers(2, newWidth, newHeight, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    app.viewport.Width = (float)newWidth;
    app.viewport.Height = (float)newHeight;
    app.windowWidth = newWidth;
    app.windowHeight = newHeight;
    app.scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(newWidth), static_cast<LONG>(newHeight));

    SetupRenderTargets(app, true);
    SetupDepthStencil(app, true);
    SetupGBuffer(app, true);
    SetupBloomPass(app);

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();
}

void CreateMainRootSignature(App& app)
{
    ID3D12Device* device = app.device.Get();
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData)))) {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    CD3DX12_ROOT_PARAMETER1 rootParameters[1];

    rootParameters->InitAsConstants(ConstantIndex_Count, 0);

    D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    // FIXME: This is going to have to be dynamic...
    D3D12_STATIC_SAMPLER_DESC defaultSampler = {};
    defaultSampler.Filter = D3D12_FILTER_ANISOTROPIC;
    defaultSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    defaultSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    defaultSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    defaultSampler.MipLODBias = 0;
    defaultSampler.MaxAnisotropy = 8;
    defaultSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    defaultSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    defaultSampler.MinLOD = 0.0f;
    defaultSampler.MaxLOD = D3D12_FLOAT32_MAX;
    defaultSampler.ShaderRegister = 0;
    defaultSampler.RegisterSpace = 0;
    defaultSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowMapSampler = {};
    shadowMapSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    shadowMapSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowMapSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowMapSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowMapSampler.MipLODBias = 0;
    shadowMapSampler.MaxAnisotropy = 8;
    shadowMapSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    shadowMapSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowMapSampler.MinLOD = 0.0f;
    shadowMapSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowMapSampler.ShaderRegister = 1;
    shadowMapSampler.RegisterSpace = 0;
    shadowMapSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[] = { defaultSampler, shadowMapSampler };

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(
        _countof(rootParameters),
        rootParameters,
        _countof(samplers),
        samplers,
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
        DebugLog() << "Error: root signature compilation failed\n";
        DebugLog() << (char*)error->GetBufferPointer();
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
    ID3D12Device5* device = app.device.Get();
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
    {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    CD3DX12_ROOT_PARAMETER1 rootParameters[1];

    rootParameters->InitAsConstants(7, 0);

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
        DebugLog() << "Error: root signature compilation failed";
        DebugLog() << (char*)error->GetBufferPointer();
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

    // app.LightBuffer.pointSphereConstantData.Initialize(app.mainAllocator.Get());
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
    app.descriptorPool.Initialize(app.device.Get(), heapDesc, "Main DescriptorPool");

    SetupMaterialBuffer(app);
}

void SetupLightPass(App& app)
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

// void SetupShadowPass(App& app)
// {
//     app.ShadowPass.rtShadowSO = CreateRTShadowSO(
//         app.psoManager,
//         app.device.Get(),
//         app.mainAllocator.Get(),
//         app.dataDir,
//         app.rootSignature.Get(),
//         &app.ShadowPass.rtShadowShaderTable
//     );

//     app.ShadowPass.rtInfoAllocation = CreateUploadBufferWithData(
//         app.mainAllocator.Get(),
//         nullptr,
//         0,
//         sizeof(RayTraceInfoConstantData),
//         reinterpret_cast<void**>(&app.ShadowPass.rtInfoPtr)
//     );
//     app.ShadowPass.rtInfoAllocation->GetResource()->SetName(L"RayTraceInfoConstantData");

//     app.ShadowPass.rtInfoDescriptor = AllocateDescriptorsUnique(app.descriptorPool, 1, "ShadowPass RTInfo");

//     {
//         D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
//         cbvDesc.BufferLocation = app.ShadowPass.rtInfoAllocation->GetResource()->GetGPUVirtualAddress();
//         cbvDesc.SizeInBytes = sizeof(RayTraceInfoConstantData);

//         app.device->CreateConstantBufferView(
//             &cbvDesc,
//             app.ShadowPass.rtInfoDescriptor.CPUHandle()
//         );
//     }
// }

void SetupPostProcessPass(App& app)
{
    // No input layout
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;

    app.PostProcessPass.toneMapPSO = CreateToneMapPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );

    // Any debug visualizations also happen in post process phase
    app.DebugVisualizer.PSO = CreateDebugVisualizerPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );
}

template<auto RenderFunc, unsigned threadType>
void RenderWorker(App& app)
{
    auto& renderThread = app.renderThreads[threadType];
    ID3D12CommandAllocator* commandAllocator = renderThread.commandAllocator.Get();
    GraphicsCommandList* commandList = renderThread.commandList.Get();

    while (app.running) {
        std::unique_lock lock(renderThread.mutex);
        renderThread.beginWork.wait(lock, [&] { return renderThread.workIsAvailable || !app.running; });

        if (!app.running) {
            goto cleanup;
        }

        ASSERT_HRESULT(commandAllocator->Reset());
        ASSERT_HRESULT(commandList->Reset(
            commandAllocator,
            nullptr
        ));

        RenderFunc(app, commandList);

        commandList->Close();

    cleanup:
        renderThread.workIsAvailable = false;
        lock.unlock();

        renderThread.workFinished.notify_one();
    }
}

void GBufferPass(App&, GraphicsCommandList*);
void ShadowPass(App&, GraphicsCommandList*);
void LightPass(App&, GraphicsCommandList*);
void AlphaBlendPass(App&, GraphicsCommandList*);
void PostProcessPass(App&, GraphicsCommandList*);

template<auto WorkerFunc, unsigned threadType>
void StartRenderThread(App& app)
{
    auto& renderThread = app.renderThreads[threadType];

    auto commandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ASSERT_HRESULT(app.device->CreateCommandAllocator(
        commandListType,
        IID_PPV_ARGS(&renderThread.commandAllocator)
    ));

    ASSERT_HRESULT(app.device->CreateCommandList(
        0,
        commandListType,
        renderThread.commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&renderThread.commandList)
    ));
    renderThread.commandList->Close();

    std::wstring commandListName;
    switch (threadType) {
    case RenderThread_GBufferPass:
        commandListName = L"GBufferPass";
        break;
    case RenderThread_LightPass:
        commandListName = L"LightPass";
        break;
    case RenderThread_AlphaBlendPass:
        commandListName = L"AlphaBlendPass";
        break;
    };

    renderThread.commandList->SetName(commandListName.c_str());

    renderThread.thread = std::thread(
        &RenderWorker<WorkerFunc, threadType>,
        std::ref(app)
    );
}

void StartRenderThreads(App& app)
{
    StartAssetThread(app);
    StartRenderThread<GBufferPass, RenderThread_GBufferPass>(app);
    StartRenderThread<LightPass, RenderThread_LightPass>(app);
    StartRenderThread<AlphaBlendPass, RenderThread_AlphaBlendPass>(app);
}


void NotifyRenderThreads(App& app)
{
    for (auto& renderThread : app.renderThreads) {
        renderThread.workIsAvailable = true;
        renderThread.beginWork.notify_one();
    }
}

void WaitRenderThreads(App& app)
{
    for (auto& renderThread : app.renderThreads) {
        std::unique_lock lock(renderThread.mutex);
        renderThread.workFinished.wait(lock, [&] { return !renderThread.workIsAvailable; });
        lock.unlock();
    }
}

void LoadInternalModels(App& app)
{
    // EnqueueGLTF(
    //     app,
    //     app.dataDir + "/InternalModels/sphere.gltf",
    //     [](App& app, Model& model) {
    //         app.LightBuffer.pointLightSphere = model.meshes[0].get();
    //     }
    // );
}

void D3DMessageCallback(
    D3D12_MESSAGE_CATEGORY category,
    D3D12_MESSAGE_SEVERITY severity,
    D3D12_MESSAGE_ID ID,
    LPCSTR pDescription,
    void* pContext)
{

    std::string catString;
    std::string sevString;

    switch (category) {
    case D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED:
        catString = "[APPLICATION_DEFINED]";
        break;
    case D3D12_MESSAGE_CATEGORY_MISCELLANEOUS:
        catString = "[MISCELLANEOUS]";
        break;
    case D3D12_MESSAGE_CATEGORY_INITIALIZATION:
        catString = "[INITIALIZATION]";
        break;
    case D3D12_MESSAGE_CATEGORY_CLEANUP:
        catString = "[CLEANUP]";
        break;
    case D3D12_MESSAGE_CATEGORY_COMPILATION:
        catString = "[COMPILATION]";
        break;
    case D3D12_MESSAGE_CATEGORY_STATE_CREATION:
        catString = "[STATE_CREATION]";
        break;
    case D3D12_MESSAGE_CATEGORY_STATE_SETTING:
        catString = "[STATE_SETTING]";
        break;
    case D3D12_MESSAGE_CATEGORY_STATE_GETTING:
        catString = "[STATE_GETTING]";
        break;
    case D3D12_MESSAGE_CATEGORY_RESOURCE_MANIPULATION:
        catString = "[RESOURCE_MANIPULATION]";
        break;
    case D3D12_MESSAGE_CATEGORY_EXECUTION:
        catString = "[EXECUTION]";
        break;
    case D3D12_MESSAGE_CATEGORY_SHADER:
        catString = "[SHADER]";
        break;
    };

    switch (severity) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        sevString = "[CORRUPTION]";
        break;
    case D3D12_MESSAGE_SEVERITY_ERROR:
        sevString = "[ERROR]";
        break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        sevString = "[WARNING]";
        break;
    case D3D12_MESSAGE_SEVERITY_INFO:
        sevString = "[INFO]";
        break;
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
        sevString = "[MESSAGE]";
        break;
    };

    DebugLog() << catString << sevString << ": " << pDescription;
}

void InitRenderer(App& app)
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
            DebugLog() << "Failed to enable D3D12 debug layer";
        }

        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> pDredSettings;
        D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings));
        if (pDredSettings) {
            pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        } else {
            DebugLog() << "Failed to load DRED\n";
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
        ComPtr<ID3D12InfoQueue1> infoQueue;
        app.device->QueryInterface(IID_PPV_ARGS(&infoQueue));
        if (infoQueue) {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
            infoQueue->RegisterMessageCallback(
                D3DMessageCallback,
                D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                nullptr,
                nullptr
            );
        } else {
            DebugLog() << "Failed to set info queue breakpoints\n";
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

    {
        D3D12_DESCRIPTOR_HEAP_DESC dsHeapDesc = {};
        dsHeapDesc.NumDescriptors = MaxLightCount + 1;
        dsHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        app.dsvDescriptorPool.Initialize(app.device.Get(), dsHeapDesc, "DSV DescriptorPool");
    }

    G_IncrementSizes.CbvSrvUav = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    G_IncrementSizes.Rtv = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    SetupRenderTargets(app, false);
    SetupDepthStencil(app, false);

    SetupGBufferPass(app);
    SetupGBuffer(app, false);

    CreateMainRootSignature(app);

    SetupBloomPass(app);

    SetupMipMapGenerator(app);

    SetupLightPass(app);
    // SetupShadowPass(app);
    SetupPostProcessPass(app);

    SetupCursorColorDebug(app);

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

    StartRenderThreads(app);
    LoadInternalModels(app);
}

void DestroyRenderer(App& app)
{
    CHECK(!app.running);

    // Stop all the render threads
    for (auto& renderThread : app.renderThreads) {
        renderThread.beginWork.notify_one();
        renderThread.thread.join();
    }
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

void UpdatePerPrimitiveData(App& app, const glm::mat4& projection, const glm::mat4& view)
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
            primitive->constantData->M = modelMatrix;
            // primitive->worldBoundingBox.max = modelMatrix * glm::vec4(primitive->localBoundingBox.max, 1.0f);
            // primitive->worldBoundingBox.min = modelMatrix * glm::vec4(primitive->localBoundingBox.min, 1.0f);
        }

        meshIter = app.meshPool.Next(meshIter);
    }
}

void SetupLightShadowMap(App& app, Light& light, int lightIdx)
{
    // We can use the same resource description as the GBuffer depth render target
    // for directional shadow map lights.
    auto resourceDesc = GBufferResourceDesc(GBufferTarget::GBuffer_Depth, app.windowWidth, app.windowHeight);
    resourceDesc.Format = DXGI_FORMAT_R32_FLOAT;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Create the resource
    {
        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = resourceDesc.Format;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                nullptr,
                &light.RayTracedShadow.texture,
                IID_NULL, nullptr
            )
        );

        auto lightName = convert_to_wstring(
            "LightRTShadow#" + std::to_string(lightIdx)
        );
        light.RayTracedShadow.texture->GetResource()->SetName(lightName.c_str());
    }

    // Create SRV
    {
        light.RayTracedShadow.SRV = AllocateDescriptorsUnique(app.descriptorPool, 1, "RTShadowMap SRV");

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT; // Can't use D32_FLOAT with SRVs...
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        app.device->CreateShaderResourceView(
            light.RayTracedShadow.texture->GetResource(),
            &srvDesc,
            light.RayTracedShadow.SRV.CPUHandle()
        );
    }

    // Create UAV
    {
        light.RayTracedShadow.UAV = AllocateDescriptorsUnique(app.descriptorPool, 1, "RTShadowMap UAV");

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        app.device->CreateUnorderedAccessView(
            light.RayTracedShadow.texture->GetResource(),
            nullptr,
            &uavDesc,
            light.RayTracedShadow.UAV.CPUHandle()
        );
    }
}

void UpdateLightConstantBuffers(App& app, const glm::mat4& projection, const glm::mat4& view, const glm::vec3& eyePosWorld)
{
    app.LightBuffer.passData->inverseProjectionMatrix = glm::inverse(projection);
    app.LightBuffer.passData->inverseViewMatrix = glm::inverse(view);
    app.LightBuffer.passData->eyePosWorld = glm::vec4(eyePosWorld, 1.0f);

    for (UINT i = 0; i < app.LightBuffer.count; i++) {
        Light& light = app.lights[i];
        if (light.castsShadow && !light.RayTracedShadow.texture) {
            SetupLightShadowMap(app, light, i);
        }

        app.lights[i].UpdateConstantData(view);

        if (app.RenderSettings.disableShadows) {
            app.lights[i].constantData->castsShadow = false;
        }
    }
}

struct Plane
{
    glm::vec4 normal;
};

enum FrustumPlane
{
    FrustumPlane_Right,
    FrustumPlane_Left,
    FrustumPlane_Top,
    FrustumPlane_Bottom,
    FrustumPlane_Near,
    FrustumPlane_Far,
};

struct Frustum
{
    glm::vec4 planes[6];
};

Frustum ComputeFrustum(const glm::mat4& viewProjection)
{
    // Thanks reddit <3
    //https://www.reddit.com/r/gamedev/comments/xj47t/does_glm_support_frustum_plane_extraction/

    glm::mat4 m = glm::transpose(viewProjection);
    //glm::mat4 m = viewProjection;

    Frustum result;

    result.planes[FrustumPlane_Right] = glm::row(viewProjection, 3) - glm::row(viewProjection, 0);
    result.planes[FrustumPlane_Left] = glm::row(viewProjection, 3) + glm::row(viewProjection, 0);
    result.planes[FrustumPlane_Top] = glm::row(viewProjection, 3) - glm::row(viewProjection, 1);
    result.planes[FrustumPlane_Bottom] = glm::row(viewProjection, 3) + glm::row(viewProjection, 1);
    result.planes[FrustumPlane_Far] = glm::row(viewProjection, 3) - glm::row(viewProjection, 2);
    result.planes[FrustumPlane_Near] = glm::row(viewProjection, 2);

    for (auto& plane : result.planes) {
        // normalize the planes
        glm::vec3 xyz(plane);
        float length = glm::length(xyz);
        plane /= length;
    }

    return result;
}

bool IsAABBCulled(const Frustum& f, const AABB& box)
{
    // https://bruop.github.io/frustum_culling/
    glm::vec4 corners[8] = {
        {box.min.x, box.min.y, box.min.z, 1.0},
        {box.max.x, box.min.y, box.min.z, 1.0},
        {box.min.x, box.max.y, box.min.z, 1.0},
        {box.max.x, box.max.y, box.min.z, 1.0},

        {box.min.x, box.min.y, box.max.z, 1.0},
        {box.max.x, box.min.y, box.max.z, 1.0},
        {box.min.x, box.max.y, box.max.z, 1.0},
        {box.max.x, box.max.y, box.max.z, 1.0},
    };

    for (const auto& plane : f.planes) {
        int out = 0;
        for (const auto& corner : corners) {
            float d = glm::dot(plane, corner);
            out += d < 0 ? 1 : 0;
        }
        if (out == 8) return true;
    }

    return false;
}

void DoFrustumCulling(PrimitivePool& primitivePool, const glm::mat4& viewProjection)
{
    PIXScopedEvent(0x93E9BE, __func__);

    Frustum f = ComputeFrustum(viewProjection);

    auto primitiveIter = primitivePool.Begin();
    while (primitiveIter) {
        if (!primitiveIter->constantData) {
            continue;
        }

        AABB worldBB = primitiveIter->localBoundingBox;
        worldBB.min = primitiveIter->constantData->M * glm::vec4(worldBB.min, 1.0f);
        worldBB.max = primitiveIter->constantData->M * glm::vec4(worldBB.max, 1.0f);


        primitiveIter->cull = IsAABBCulled(f, worldBB);
        primitiveIter = primitivePool.Next(primitiveIter);
    }
}

// void UpdateRayTraceInfo(App& app, const glm::mat4& viewProjection, const glm::vec3& camPos)
// {
//     app.ShadowPass.rtInfoPtr->camPosWorld = camPos;
//     app.ShadowPass.rtInfoPtr->tMin = 0.001f;
//     app.ShadowPass.rtInfoPtr->tMax = 1000.0f;

//     app.ShadowPass.rtInfoPtr->projectionToWorld = glm::inverse(viewProjection);
// }

void UpdateRenderData(App& app, const glm::mat4& projection, const glm::mat4& view, const glm::vec3& camPos)
{
    UpdateLightConstantBuffers(app, projection, view, camPos);
    UpdatePerPrimitiveData(app, projection, view);
    DoFrustumCulling(app.primitivePool, projection * view);
    //UpdateRayTraceInfo(app, projection * view, camPos);
}

std::vector<Mesh*> PickSceneMeshes(Scene& scene)
{
    std::vector<Mesh*> meshes;

    for (auto& node : scene.nodes) {
        if (node.nodeType == NodeType_Mesh) {
            meshes.push_back(node.mesh);
        }
    }

    return meshes;
}

void BuildTLAS(App& app, GraphicsCommandList* commandList)
{
    auto meshes = PickSceneMeshes(app.scene);

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
    int instanceId = 0;
    for (auto& mesh : meshes)
    {
        for (auto& primitive : mesh->primitives)
        {
            if (!primitive->blasResult) {
                continue;
            }

            glm::mat3x4 truncatedModelMat = glm::transpose(primitive->constantData->M);

            D3D12_RAYTRACING_INSTANCE_DESC instances = {};
            instances.InstanceID = instanceId++;
            instances.InstanceContributionToHitGroupIndex = 0;
            instances.InstanceMask = 0xFF;
            memcpy(instances.Transform, reinterpret_cast<void*>(&truncatedModelMat[0]), sizeof(instances.Transform));
            instances.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            instances.AccelerationStructure = primitive->blasResult->GetResource()->GetGPUVirtualAddress();

            instanceDescs.push_back(instances);
        }
    }

    // DXR is quite strange...
    auto instanceBufferSizeBytes = instanceDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    // If we don't have an instance buffer OR it's not big enough create one
    // if (!app.TLAS.instancesUploadBuffer ||
    //     app.TLAS.instancesUploadBuffer->GetResource()->GetDesc().Width < instanceBufferSizeBytes) {

    if (instanceBufferSizeBytes == 0) {
        return;
    }

    // Per frame pain?
    CreateOrReallocateUploadBufferWithData(
        app.mainAllocator.Get(),
        app.TLAS.instancesUploadBuffer,
        instanceDescs.data(),
        instanceBufferSizeBytes
    );

    // }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS asInputs = {};
    asInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    asInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    asInputs.NumDescs = instanceDescs.size();
    asInputs.InstanceDescs = app.TLAS.instancesUploadBuffer->GetResource()->GetGPUVirtualAddress();
    asInputs.Flags = buildFlags;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    app.device->GetRaytracingAccelerationStructurePrebuildInfo(
        &asInputs,
        &prebuildInfo
    );

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
    desc.Inputs = asInputs;

    CreateAccelerationStructureBuffers(
        app.mainAllocator.Get(),
        prebuildInfo,
        app.TLAS.scratch,
        app.TLAS.result
    );
    app.TLAS.result->GetResource()->SetName(L"tlasResult");
    app.TLAS.scratch->GetResource()->SetName(L"tlasScratch");

    desc.ScratchAccelerationStructureData = app.TLAS.scratch->GetResource()->GetGPUVirtualAddress();
    desc.DestAccelerationStructureData = app.TLAS.result->GetResource()->GetGPUVirtualAddress();

    // Create SRV to the TLAS
    {
        if (!app.TLAS.descriptor.IsValid()) {
            app.TLAS.descriptor = AllocateDescriptorsUnique(app.descriptorPool, 1, "TLAS SRV");
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.RaytracingAccelerationStructure.Location = app.TLAS.result->GetResource()->GetGPUVirtualAddress();
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        app.device->CreateShaderResourceView(
            nullptr,
            &srvDesc,
            app.TLAS.descriptor.CPUHandle()
        );
    }

    commandList->BuildRaytracingAccelerationStructure(
        &desc,
        0,
        nullptr
    );
}

void DrawMeshesGBuffer(App& app, GraphicsCommandList* commandList)
{
    commandList->OMSetStencilRef(0xFFFFFFFF);

    ManagedPSORef lastUsedPSO = nullptr;

    auto meshes = PickSceneMeshes(app.scene);

    for (auto& mesh : meshes) {
        if (!mesh->isReadyForRender) {
            continue;
        }

        for (const auto& primitive : mesh->primitives) {
            if (primitive->cull) {
                continue;
            }

            const auto& material = primitive->material.get();
            // FIXME: if I am not lazy I will SORT by material type
            // Transparent materials drawn in different pass
            DescriptorRef materialDescriptor;

            if (material) {
                if (material->materialType == MaterialType_AlphaBlendPBR || material->materialType == MaterialType_Unlit) {
                    continue;
                }
                materialDescriptor = material->cbvDescriptor.Ref();
            }

            // Set the per-primitive constant buffer
            UINT constantValues[5] = { primitive->perPrimitiveDescriptor.index, materialDescriptor.index, 0, 0, primitive->miscDescriptorParameter.index };
            commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
            commandList->IASetPrimitiveTopology(primitive->primitiveTopology);

            if (primitive->PSO != lastUsedPSO) {
                commandList->SetPipelineState(primitive->PSO->Get());
                lastUsedPSO = primitive->PSO;
            }

            commandList->IASetVertexBuffers(0, (UINT)primitive->vertexBufferViews.size(), primitive->vertexBufferViews.data());
            commandList->IASetIndexBuffer(&primitive->indexBufferView);
            commandList->DrawIndexedInstanced(primitive->indexCount, primitive->instanceCount, 0, 0, 0);

            app.Stats.drawCalls++;
        }
    }
}

void DrawAlphaBlendedMeshes(App& app, GraphicsCommandList* commandList)
{
    const UINT MaxLightsPerDraw = 8;

    ManagedPSORef lastUsedPSO = nullptr;

    auto meshes = PickSceneMeshes(app.scene);

    for (auto& mesh : meshes) {
        if (!mesh->isReadyForRender) {
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

            for (UINT lightIdx = 0; lightIdx < app.LightBuffer.count; lightIdx += MaxLightsPerDraw) {
                UINT lightCount = glm::max(app.LightBuffer.count - lightIdx, MaxLightsPerDraw);

                UINT lightDescriptorIndex = app.LightBuffer.cbvHandle.Index() + lightIdx + 1u;
                // Set the per-primitive constant buffer
                UINT constantValues[5] = {
                    primitive->perPrimitiveDescriptor.index,
                    materialDescriptor.Index(),
                    lightDescriptorIndex,
                    0,
                    primitive->miscDescriptorParameter.index
                };
                commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
                commandList->IASetPrimitiveTopology(primitive->primitiveTopology);

                if (primitive->PSO != lastUsedPSO) {
                    commandList->SetPipelineState(primitive->PSO->Get());
                    lastUsedPSO = primitive->PSO;
                }

                commandList->IASetVertexBuffers(0, (UINT)primitive->vertexBufferViews.size(), primitive->vertexBufferViews.data());
                commandList->IASetIndexBuffer(&primitive->indexBufferView);
                commandList->DrawIndexedInstanced(primitive->indexCount, primitive->instanceCount, 0, 0, 0);

                app.Stats.drawCalls++;
            }
        }
    }
}

void DrawUnlitMeshes(App& app, GraphicsCommandList* commandList)
{
    commandList->RSSetViewports(1, &app.viewport);
    commandList->RSSetScissorRects(1, &app.scissorRect);

    auto rtvHandle = app.nonSRGBFrameBufferRTVs[app.frameIdx].CPUHandle();
    auto dsvHandle = app.depthStencilDescriptor.CPUHandle();
    commandList->OMSetRenderTargets(1, &rtvHandle, false, &dsvHandle);

    ManagedPSORef lastUsedPSO = nullptr;

    auto meshes = PickSceneMeshes(app.scene);

    for (auto& mesh : meshes) {
        if (!mesh->isReadyForRender) {
            continue;
        }

        for (const auto& primitive : mesh->primitives) {
            if (primitive->cull) {
                continue;
            }

            const auto& material = primitive->material.get();
            // FIXME: if I am not lazy I will SORT by material type
            // Transparent materials drawn in different pass
            DescriptorRef materialDescriptor;

            if (!material || material->materialType != MaterialType_Unlit) {
                continue;
            }

            materialDescriptor = material->cbvDescriptor.Ref();

            // Set the per-primitive constant buffer
            UINT constantValues[5] = { primitive->perPrimitiveDescriptor.index, materialDescriptor.index, 0, 0, primitive->miscDescriptorParameter.index };
            commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
            commandList->IASetPrimitiveTopology(primitive->primitiveTopology);

            if (primitive->PSO != lastUsedPSO) {
                commandList->SetPipelineState(primitive->PSO->Get());
                lastUsedPSO = primitive->PSO;
            }

            commandList->IASetVertexBuffers(0, (UINT)primitive->vertexBufferViews.size(), primitive->vertexBufferViews.data());
            commandList->IASetIndexBuffer(&primitive->indexBufferView);
            commandList->DrawIndexedInstanced(primitive->indexCount, primitive->instanceCount, 0, 0, 0);

            app.Stats.drawCalls++;
        }
    }
}

void DrawFullscreenQuad(App& app, GraphicsCommandList* commandList)
{
    // NOTE: if this only draws one triangle, its because the primitive topology is not triangle strip
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->DrawInstanced(4, 1, 0, 0);

    app.Stats.drawCalls++;
}

void DrawFullscreenQuadInstanced(App& app, GraphicsCommandList* commandList, int count)
{
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->DrawInstanced(4, count, 0, 0);

    app.Stats.drawCalls++;
}

void BindAndClearGBufferRTVs(const App& app, GraphicsCommandList* commandList)
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE renderTargetHandles[GBuffer_RTVCount] = {};
    for (int i = 0; i < GBuffer_RTVCount; i++) {
        renderTargetHandles[i] = app.GBuffer.rtvs[i].CPUHandle();
    }

    auto dsvHandle = app.depthStencilDescriptor.CPUHandle();
    commandList->OMSetRenderTargets(_countof(renderTargetHandles), renderTargetHandles, FALSE, &dsvHandle);

    // Clear render targets
    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (const auto& handle : renderTargetHandles) {
        commandList->ClearRenderTargetView(handle, clearColor, 0, nullptr);
    }
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
}

void TransitionResourcesForGBufferPass(const App& app, GraphicsCommandList* commandList)
{
    // Transition our GBuffers into being render targets.
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(GBuffer_Count + 2);
    // Back buffer
    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(app.renderTargets[app.frameIdx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
    // GBuffer RTVs
    for (int i = 0; i < GBuffer_RTVCount; i++) {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(app.GBuffer.renderTargets[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
    }

    // Transition any light shadow maps to being unordered access
    for (UINT i = 0; i < app.LightBuffer.count; i++) {
        if (app.lights[i].castsShadow && app.lights[i].RayTracedShadow.texture != nullptr) {
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                app.lights[i].RayTracedShadow.texture->GetResource(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            ));
        }
    }

    commandList->ResourceBarrier((UINT)barriers.size(), barriers.data());
}

void GBufferPass(App& app, GraphicsCommandList* commandList)
{
    PIXScopedEvent(commandList, 0xC082FF, L"GBufferPass");

    commandList->RSSetViewports(1, &app.viewport);
    commandList->RSSetScissorRects(1, &app.scissorRect);

    TransitionResourcesForGBufferPass(app, commandList);

    BuildTLAS(app, commandList);

    BindAndClearGBufferRTVs(app, commandList);

    ID3D12DescriptorHeap* mainDescriptorHeap = app.descriptorPool.Heap();
    ID3D12DescriptorHeap* ppHeaps[] = { mainDescriptorHeap };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    commandList->SetGraphicsRootSignature(app.rootSignature.Get());

    DrawMeshesGBuffer(app, commandList);
}

// void TransitionResourcesForShadowPass(const App& app, GraphicsCommandList* commandList)
// {
//     std::vector<D3D12_RESOURCE_BARRIER> barriers;

//     // Transition any light shadow maps to being unordered access
//     for (UINT i = 0; i < app.LightBuffer.count; i++) {
//         if (app.lights[i].castsShadow && app.lights[i].RayTracedShadow.texture != nullptr) {
//             barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
//                 app.lights[i].RayTracedShadow.texture->GetResource(),
//                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
//                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS
//             ));
//         }
//     }

//     commandList->ResourceBarrier((UINT)barriers.size(), barriers.data());
// }

// void ShadowPass(App& app, GraphicsCommandList* commandList)
// {
//     PIXScopedEvent(commandList, 0xC1C1C1, L"ShadowPass");

//     auto descriptorHeap = app.descriptorPool.Heap();
//     ID3D12DescriptorHeap* ppHeaps[] = { descriptorHeap };
//     commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

//     commandList->SetComputeRootSignature(app.rootSignature.Get());

//     BuildTLAS(app, commandList);

//     // if (app.TLAS.result) {
//     //     barriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(
//     //         app.TLAS.result->GetResource()
//     //     ));
//     // }

//     if (app.TLAS.result) {
//         auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(
//             app.TLAS.result->GetResource()
//         );
//         commandList->ResourceBarrier(1, &barrier);
//     }

//     commandList->SetPipelineState1(app.ShadowPass.rtShadowSO->SO.Get());

//     for (UINT i = 0; i < app.LightBuffer.count; i++) {
//         auto& light = app.lights[i];

//         if (!light.castsShadow) {
//             continue;
//         }

//         D3D12_DISPATCH_RAYS_DESC raysDesc = {};
//         raysDesc.Width = app.windowWidth;
//         raysDesc.Height = app.windowHeight;
//         raysDesc.Depth = 1;
//         raysDesc.RayGenerationShaderRecord = app.ShadowPass.rtShadowShaderTable.RayGenerationShaderRecord;
//         raysDesc.HitGroupTable = app.ShadowPass.rtShadowShaderTable.HitGroupTable;
//         raysDesc.CallableShaderTable = app.ShadowPass.rtShadowShaderTable.CallableShaderTable;
//         raysDesc.MissShaderTable = app.ShadowPass.rtShadowShaderTable.MissShaderTable;


//         UINT constantValues[4] = {
//             app.ShadowPass.rtInfoDescriptor.Index(),
//             light.RayTracedShadow.UAV.Index(),
//             app.LightBuffer.cbvHandle.Index() + i + 1,
//             app.TLAS.descriptor.Index()
//         };

//         commandList->SetComputeRoot32BitConstants(
//             0,
//             _countof(constantValues),
//             constantValues,
//             0
//         );

//         commandList->DispatchRays(
//             &raysDesc
//         );
//     }
// }

const D3D12_RESOURCE_STATES LightPassDepthResourceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;

void TransitionResourcesForLightPass(const App& app, GraphicsCommandList* commandList)
{
    // Transition GBuffer to being shader resource views so that they can be used in the lighting shaders.
    // Radiance stays as RTV for this pass.
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(GBuffer_RTVCount + 2);
    for (int i = GBuffer_BaseColor; i < GBuffer_RTVCount; i++) {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(app.GBuffer.renderTargets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    }

    // Depth buffer is special
    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(app.depthStencilBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, LightPassDepthResourceState));

    // Transition any RT shadow maps to being pixel resources
    for (UINT i = 0; i < app.LightBuffer.count; i++) {
        if (app.lights[i].castsShadow && app.lights[i].RayTracedShadow.texture != nullptr) {
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                app.lights[i].RayTracedShadow.texture->GetResource(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            ));
        }
    }

    commandList->ResourceBarrier((UINT)barriers.size(), barriers.data());
}

void LightPass(App& app, GraphicsCommandList* commandList)
{
    PIXScopedEvent(commandList, 0xFF9F82, L"LightPass");

    commandList->RSSetViewports(1, &app.viewport);
    commandList->RSSetScissorRects(1, &app.scissorRect);

    TransitionResourcesForLightPass(app, commandList);

    auto rtvHandle = app.GBuffer.rtvs[GBuffer_Radiance].CPUHandle();
    auto dsvHandle = app.depthStencilDescriptor.CPUHandle();
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
    commandList->OMSetStencilRef(0xFF);

    commandList->RSSetViewports(1, &app.viewport);
    commandList->RSSetScissorRects(1, &app.scissorRect);

    {
        auto descriptorHeap = app.descriptorPool.Heap();
        ID3D12DescriptorHeap* ppHeaps[] = { descriptorHeap };
        commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
        // Root signature must be set AFTER heaps are set when CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED is set.
        commandList->SetGraphicsRootSignature(app.rootSignature.Get());

        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        // Point lights
        PIXBeginEvent(commandList, 0xFF9F82, L"PointLights");
        commandList->SetPipelineState(app.LightPass.pointLightPSO->Get());

        for (UINT i = 0; i < app.LightBuffer.count; i++) {
            // Group together as many point lights as possible into one draw
            int lightCount = 0;
            int lightStartIdx = i;
            while (i < app.LightBuffer.count && app.lights[i].lightType == LightType_Point) {
                lightCount++;
                i++;
            }

            if (lightCount > 0) {
                UINT constantValues[4] = {
                    app.TLAS.descriptor.Index(),
                    app.LightBuffer.cbvHandle.Index() + lightStartIdx + 1,
                    app.LightBuffer.cbvHandle.Index(),
                    lightCount,
                };
                commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 1);
                DrawFullscreenQuad(app, commandList);
            }
        }

        PIXEndEvent(commandList);

        // Directional lights
        PIXBeginEvent(commandList, 0xFF9F82, L"DirectionalLights");
        bool hasSetPSO = false;
        for (UINT i = 0; i < app.LightBuffer.count; i++) {
            if (app.lights[i].lightType == LightType_Directional) {
                // avoid setting PSO if no directional lights
                if (!hasSetPSO) {
                    commandList->SetPipelineState(app.LightPass.directionalLightPso->Get());
                    hasSetPSO = true;
                }

                UINT constantValues[3] = {
                    app.TLAS.descriptor.Index(),
                    app.LightBuffer.cbvHandle.Index() + i + 1,
                    app.LightBuffer.cbvHandle.Index()
                };
                commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 1);
                DrawFullscreenQuad(app, commandList);
            }
        }
        PIXEndEvent(commandList);

        // Environment cubemap
        if (app.Skybox.prefilterMapSRV.IsValid()) {
            PIXBeginEvent(commandList, 0xFF9F82, L"EnvironmentCubemap");
            commandList->SetPipelineState(app.LightPass.environentCubemapLightPso->Get());
            UINT constantValues[5] = {
                app.TLAS.descriptor.Index(),
                app.Skybox.brdfLUTDescriptor.Index(),
                app.Skybox.irradianceCubeSRV.Index(),
                app.LightBuffer.cbvHandle.Index(),
                app.Skybox.prefilterMapSRV.Index(),
            };
            commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
            DrawFullscreenQuad(app, commandList);
            PIXEndEvent(commandList);
        }
    }
}

// Forward pass for meshes with transparency
void AlphaBlendPass(App& app, GraphicsCommandList* commandList)
{
    PIXScopedEvent(commandList, 0x93E9BE, L"AlphaBlendPass");

    commandList->RSSetViewports(1, &app.viewport);
    commandList->RSSetScissorRects(1, &app.scissorRect);

    // Transition depth buffer back to depth write for alpha blend
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.depthStencilBuffer.Get(), LightPassDepthResourceState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commandList->ResourceBarrier(1, &barrier);

    auto rtvHandle = app.GBuffer.rtvs[GBuffer_Radiance].CPUHandle();
    auto dsvHandle = app.depthStencilDescriptor.CPUHandle();
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    ID3D12DescriptorHeap* mainDescriptorHeap = app.descriptorPool.Heap();
    ID3D12DescriptorHeap* ppHeaps[] = { mainDescriptorHeap };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    commandList->SetGraphicsRootSignature(app.rootSignature.Get());

    DrawAlphaBlendedMeshes(app, commandList);
}

void TransitionResourcesForPostProcessPass(App& app, GraphicsCommandList* commandList)
{
    // Transition radiance buffer to being an SRV
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.GBuffer.renderTargets[GBuffer_Radiance].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);
}

void DebugVisualizer(App& app, GraphicsCommandList* commandList)
{
    auto rtvHandle = app.nonSRGBFrameBufferRTVs[app.frameIdx].CPUHandle();
    auto dsvHandle = app.depthStencilDescriptor.CPUHandle();
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
    commandList->SetPipelineState(app.DebugVisualizer.PSO->Get());
    UINT constantValues[2] = {
        app.LightBuffer.cbvHandle.Index(),
        (UINT)app.DebugVisualizer.mode,
    };
    commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 3);
    DrawFullscreenQuad(app, commandList);
}

void BloomPingPongStep(
    App& app,
    GraphicsCommandList* commandList,
    ManagedPSORef pso,
    int rtvPingPongIndex,
    int srvPingPongIndex,
    std::span<UINT> constantValues)
{
    D3D12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(
            app.Bloom.PingPong[rtvPingPongIndex].texture->GetResource(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(
            app.Bloom.PingPong[srvPingPongIndex].texture->GetResource(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };

    commandList->ResourceBarrier(_countof(barriers), barriers);

    auto rtvHandle = app.Bloom.PingPong[rtvPingPongIndex].rtv.CPUHandle();
    commandList->OMSetRenderTargets(
        1,
        &rtvHandle,
        false,
        nullptr
    );
    commandList->SetPipelineState(pso->Get());
    commandList->SetGraphicsRoot32BitConstants(
        0,
        constantValues.size(),
        constantValues.data(),
        0
    );
    DrawFullscreenQuad(app, commandList);
}

void ApplyBloom(App& app, GraphicsCommandList* commandList)
{
    commandList->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
    );

    {
        auto bloomResourceDesc = app.Bloom.PingPong[0].texture->GetResource()->GetDesc();
        CD3DX12_VIEWPORT viewport(app.Bloom.PingPong[0].texture->GetResource());
        commandList->RSSetViewports(1, &viewport);

        auto scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(viewport.Width), static_cast<LONG>(viewport.Height));
        commandList->RSSetScissorRects(1, &scissorRect);
    }

    // Filter
    {
        UINT constantValues[] = {
            *reinterpret_cast<UINT*>(&app.Bloom.threshold),
            app.GBuffer.baseSrvReference.Index(),
        };

        BloomPingPongStep(
            app,
            commandList,
            app.Bloom.filterPSO,
            0,
            1,
            std::span(constantValues)
        );
    }

    const int NUM_BLUR_PASSES = 10;
    static_assert(NUM_BLUR_PASSES % 2 == 0);

    int ping = 1;
    int pong = 0;

    bool horizontal = false;

    // Blur passes
    for (int i = 0; i < NUM_BLUR_PASSES; i++)
    {
        UINT constantValues[] = {
            app.Bloom.PingPong[pong].srv.Index(),
            (UINT)horizontal
        };

        BloomPingPongStep(
            app,
            commandList,
            app.Bloom.blurPSO,
            ping,
            pong,
            std::span(constantValues)
        );

        std::swap(ping, pong);

        horizontal = !horizontal;
    }

    // Final result goes into PingPong[ping]
    D3D12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(
            app.Bloom.PingPong[pong].texture->GetResource(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(
            app.Bloom.PingPong[ping].texture->GetResource(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    commandList->ResourceBarrier(_countof(barriers), barriers);

    // Apply bloom, writing bloom texture into radiance buffer
    commandList->SetPipelineState(app.Bloom.applyPSO->Get());
    commandList->RSSetViewports(1, &app.viewport);
    commandList->RSSetScissorRects(1, &app.scissorRect);
    auto rtvHandle = app.GBuffer.rtvs[GBuffer_Radiance].CPUHandle();
    commandList->OMSetRenderTargets(
        1,
        &rtvHandle,
        false,
        nullptr
    );
    UINT constantValues[] = {
        app.Bloom.PingPong[ping].srv.Index()
    };
    commandList->SetGraphicsRoot32BitConstants(
        0,
        _countof(constantValues),
        constantValues,
        0
    );
    DrawFullscreenQuad(app, commandList);
}

void PostProcessPass(App& app, GraphicsCommandList* commandList)
{
    PIXScopedEvent(commandList, 0x89E4F8, L"PostProcessPass");

    commandList->RSSetViewports(1, &app.viewport);
    commandList->RSSetScissorRects(1, &app.scissorRect);

    ID3D12DescriptorHeap* mainDescriptorHeap = app.descriptorPool.Heap();
    ID3D12DescriptorHeap* ppHeaps[] = { mainDescriptorHeap };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    commandList->SetGraphicsRootSignature(app.rootSignature.Get());

    // Apply bloom while radiance buffer is still in render target state
    ApplyBloom(app, commandList);

    TransitionResourcesForPostProcessPass(app, commandList);

    auto rtvHandle = app.nonSRGBFrameBufferRTVs[app.frameIdx].CPUHandle();

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    if (app.DebugVisualizer.mode == DebugVisualizerMode_Disabled) {
        // Tone map pass to convert radiance map to sRGB
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        commandList->SetPipelineState(app.PostProcessPass.toneMapPSO->Get());
        UINT constantValues[] = {
            app.GBuffer.baseSrvReference.Index(),
            *reinterpret_cast<UINT*>(&app.PostProcessPass.gamma),
            *reinterpret_cast<UINT*>(&app.PostProcessPass.exposure),
        };
        commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
        DrawFullscreenQuad(app, commandList);
    } else {
        DebugVisualizer(app, commandList);
    }

    rtvHandle = app.frameBufferRTVs[app.frameIdx].CPUHandle();
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    // Unlit meshes draw straight into the backbuffer without tonemapping, making this a good
    // spot to do this
    DrawUnlitMeshes(app, commandList);
}

ID3D12Resource* GetColorCusorSourceBuffer(App& app)
{
    ID3D12Resource* result = nullptr;
    switch (app.DebugVisualizer.mode)
    {
    case DebugVisualizerMode_Disabled:
    default: // TODO: support GBuffers for cursor color debug
        result = app.renderTargets[app.frameIdx].Get();
        break;
        // case DebugVisualizerMode_Radiance:
        //     result = app.GBuffer.renderTargets[GBuffer_Radiance].Get();
        //     break;
        // case DebugVisualizerMode_BaseColor:
        //     result = app.GBuffer.renderTargets[GBuffer_BaseColor].Get();
        //     break;
        // case DebugVisualizerMode_Normal:
        //     result = app.GBuffer.renderTargets[GBuffer_Normal].Get();
        //     break;
        // case DebugVisualizerMode_Depth:
        //     result = app.GBuffer.renderTargets[GBuffer_Depth].Get();
        //     break;
        // case DebugVisualizerMode_MetalRoughness:
        //     result = app.GBuffer.renderTargets[GBuffer_MetalRoughness].Get();
        //     break;
    }

    return result;
}

void ExecuteColorCusorReadback(App& app, GraphicsCommandList* commandList, D3D12_RESOURCE_STATES& renderTargetState)
{
    // Bounds check the cursor
    if (app.mouseState.cursorPos.x < 0 || app.mouseState.cursorPos.x >= app.windowWidth ||
        app.mouseState.cursorPos.y < 0 || app.mouseState.cursorPos.y >= app.windowHeight) {
        return;
    }

    auto pSrc = GetColorCusorSourceBuffer(app);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = app.CursorColorDebug.readbackBuffer->GetResource();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    auto srcDesc = pSrc->GetDesc();
    app.device->GetCopyableFootprints(
        &srcDesc,
        0,
        1,
        0,
        &dst.PlacedFootprint,
        nullptr,
        nullptr,
        nullptr
    );

    dst.PlacedFootprint.Footprint.Width = 1;
    dst.PlacedFootprint.Footprint.Height = 1;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = pSrc;
    src.SubresourceIndex = 0;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_BOX srcBox;
    srcBox.left = app.mouseState.cursorPos.x;
    srcBox.right = app.mouseState.cursorPos.x + 1;
    srcBox.top = app.mouseState.cursorPos.y;
    srcBox.bottom = app.mouseState.cursorPos.y + 1;
    srcBox.back = 1;
    srcBox.front = 0;

    auto afterState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        app.renderTargets[app.frameIdx].Get(),
        renderTargetState,
        afterState
    );
    commandList->ResourceBarrier(
        1,
        &barrier
    );
    renderTargetState = afterState;

    commandList->CopyTextureRegion(
        &dst,
        0,
        0,
        0,
        &src,
        &srcBox
    );

    app.CursorColorDebug.readbackPending = true;
}

void BuildPresentCommandList(App& app)
{
    GraphicsCommandList* commandList = app.commandList.Get();

    ASSERT_HRESULT(
        app.commandAllocator->Reset()
    );

    ASSERT_HRESULT(
        commandList->Reset(app.commandAllocator.Get(), app.pipelineState.Get())
    );

    PostProcessPass(app, commandList);

    auto linearRTV = app.nonSRGBFrameBufferRTVs[app.frameIdx].CPUHandle();
    commandList->OMSetRenderTargets(1, &linearRTV, FALSE, nullptr);

    ID3D12DescriptorHeap* ppHeaps[] = { app.ImGui.srvHeap.Heap() };
    commandList->SetDescriptorHeaps(1, ppHeaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);


    D3D12_RESOURCE_STATES backBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (app.mouseState.leftClick) {
        ExecuteColorCusorReadback(app, commandList, backBufferState);
    }

    // Indicate that the back buffer will now be used to present.
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        app.renderTargets[app.frameIdx].Get(),
        backBufferState,
        D3D12_RESOURCE_STATE_PRESENT
    );
    commandList->ResourceBarrier(1, &barrier);

    ASSERT_HRESULT(
        commandList->Close()
    );
}

void BuildCommandLists(App& app)
{
    NotifyRenderThreads(app);

    // This one can go on main thread
    BuildPresentCommandList(app);

    WaitRenderThreads(app);
}

void FetchCusorColor(App& app)
{
    if (app.CursorColorDebug.readbackPending) {
        UINT* data;
        D3D12_RANGE readbackRange{ 0, sizeof(UINT) };
        app.CursorColorDebug.readbackBuffer->GetResource()->Map(
            0,
            &readbackRange,
            reinterpret_cast<void**>(&data)
        );

        app.CursorColorDebug.lastRGBA.w = ((*data & 0xFF000000) >> 24);
        app.CursorColorDebug.lastRGBA.z = ((*data & 0x00FF0000) >> 16);
        app.CursorColorDebug.lastRGBA.y = ((*data & 0x0000FF00) >> 8);
        app.CursorColorDebug.lastRGBA.x = ((*data & 0x000000FF) >> 0);
        app.CursorColorDebug.lastRGBA /= 255.0f;

        D3D12_RANGE emptyRange{ 0, 0 };
        app.CursorColorDebug.readbackBuffer->GetResource()->Unmap(
            0,
            &emptyRange
        );

        app.CursorColorDebug.readbackPending = false;
    }
}

void RenderFrame(App& app)
{
    auto lock = LockRenderThread(app);
    app.graphicsQueue.WaitForEventCPU(app.previousFrameEvent);

    app.Stats.drawCalls = 0;

    FetchCusorColor(app);

    BuildCommandLists(app);

    std::array<ID3D12CommandList*, RenderThread_Count> commandLists;
    for (int i = 0; i < RenderThread_Count; i++) {
        commandLists[i] = app.renderThreads[i].commandList.Get();
    }

    FenceEvent shadowPassFenceEvent;
    FenceEvent gbufferFenceEvent;

    FenceEvent renderWorkloadEvent;

#define EXECUTE_MULTI_COMMANDLISTS
#ifdef EXECUTE_MULTI_COMMANDLISTS
    app.graphicsQueue.ExecuteCommandLists(commandLists, renderWorkloadEvent);
#else
    for (auto& commandList : commandLists) {
        app.graphicsQueue.ExecuteCommandListsBlocking({ commandList }, app.previousFrameEvent);
    }
#endif

    ID3D12CommandList* const presentCommandList = app.commandList.Get();

    HRESULT hr = app.graphicsQueue.ExecuteCommandListsAndPresent(
        std::span<ID3D12CommandList* const>({ presentCommandList }),
        app.swapChain.Get(),
        0,
        DXGI_PRESENT_ALLOW_TEARING,
        app.previousFrameEvent,
        renderWorkloadEvent
    );

    if (!SUCCEEDED(hr)) {
        app.running = false;
        DebugLog() << "TDR occurred\n";
    }

    app.frameIdx = app.swapChain->GetCurrentBackBufferIndex();
}
