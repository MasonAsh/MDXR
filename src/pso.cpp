#include "pso.h"
#include "util.h"

#include <iostream>

#include <d3dcompiler.h>

ID3D12PipelineState* ManagedGraphicsPSO::Get()
{
    return PSO.Get();
}

void ManagedGraphicsPSO::Reload(ID3D12Device* device)
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

ID3D12PipelineState* ManagedComputePSO::Get()
{
    return PSO.Get();
}

void ManagedComputePSO::Reload(ID3D12Device* device)
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

void PSOManager::Reload(ID3D12Device* device)
{
    for (auto& PSO : PSOs) {
        if (std::shared_ptr<IManagedPSO> pso = PSO.lock()) {
            pso->Reload(device);
        }
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
    mPso->Reload(device);

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
    mPso->Reload(device);

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
    for (UINT i = 1; i < psoDesc.NumRenderTargets; i++) {
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
    for (UINT i = 1; i < psoDesc.NumRenderTargets; i++) {
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
    for (UINT i = 1; i < psoDesc.NumRenderTargets; i++) {
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
