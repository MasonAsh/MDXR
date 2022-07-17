#include "pso.h"
#include "util.h"

#include <iostream>
#include <mutex>
#include <map>

#include <d3dcompiler.h>

void PSOManager::Reload(ID3D12Device2* device)
{
    shaderByteCodeCache.Invalidate();

    for (auto PSO = PSOs.begin(); PSO != PSOs.end(); ) {
        if (ManagedPSORef pso = PSO->lock()) {
            pso->Reload(device, shaderByteCodeCache);
            PSO++;
        } else {
            // Erase any unused PSOs.
            PSO = PSOs.erase(PSO);
        }
    }
}

ManagedPSORef PSOManager::FindPSO(UINT hash)
{
    for (auto PSO = PSOs.begin(); PSO != PSOs.end(); ) {
        if (ManagedPSORef pso = PSO->lock()) {
            if (pso->hash == hash) {
                return pso;
            }
            PSO++;
        } else {
            // Erase any unused PSOs.
            PSO = PSOs.erase(PSO);
        }
    }

    return nullptr;
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

std::mutex g_PSOMutex;

ManagedPSORef CreatePSO(
    PSOManager& manager,
    ID3D12Device2* device,
    const ShaderPaths& paths,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    CD3DX12_PIPELINE_STATE_STREAM psoDesc
)
{
    std::scoped_lock<std::mutex> lock(g_PSOMutex);

    auto mPso = std::make_shared<ManagedPSO>();
    mPso->desc = psoDesc;
    mPso->inputLayoutMemory = inputLayout;
    mPso->desc.pRootSignature = rootSignature;
    mPso->desc.InputLayout = { mPso->inputLayoutMemory.data(), (UINT)mPso->inputLayoutMemory.size() };
    mPso->shaderPaths = paths;

    CHECK(mPso->Load(manager.shaderByteCodeCache));

    // Check if we've already created this PSO
    auto PSO = manager.FindPSO(mPso->hash);
    if (PSO) {
        return PSO;
    }

    ASSERT_HRESULT(mPso->Compile(device));

    manager.PSOs.emplace_back(mPso);

    return mPso;
}

ManagedPSORef SimpleCreateGraphicsPSO(
    PSOManager& manager,
    ID3D12Device2* device,
    const std::string& baseShaderPath,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc
)
{
    const std::string vertexShaderPath = (baseShaderPath + ".cvert");
    const std::string pixelShaderPath = (baseShaderPath + ".cpixel");

    ShaderPaths paths;
    paths.vertex = vertexShaderPath;
    paths.pixel = pixelShaderPath;

    return CreatePSO(
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
    ID3D12Device2* device,
    const std::string& baseShaderPath,
    ID3D12RootSignature* rootSignature,
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc
)
{
    ShaderPaths shaderPaths;
    shaderPaths.compute = baseShaderPath + ".ccomp";

    return CreatePSO(
        manager,
        device,
        shaderPaths,
        rootSignature,
        {},
        CD3DX12_PIPELINE_STATE_STREAM(psoDesc)
    );
}

ManagedPSORef CreateMipMapGeneratorPSO(
    PSOManager& manager,
    ID3D12Device2* device,
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
    ID3D12Device2* device,
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
    ID3D12Device2* device,
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
    ID3D12Device2* device,
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
    ID3D12Device2* device,
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

ManagedPSORef CreateDirectionalLightShadowMapPSO(
    PSOManager& manager,
    ID3D12Device2* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    float depthBias = -0.0005f;
    psoDesc.RasterizerState.DepthBias = -(depthBias / (1.0f / pow(2.0, 23.0)));
    psoDesc.RasterizerState.SlopeScaledDepthBias = -0.005;
    psoDesc.RasterizerState.DepthBiasClamp = -0.05f;

    // No RTVs
    psoDesc.NumRenderTargets = 0;
    std::fill_n(psoDesc.RTVFormats, _countof(psoDesc.RTVFormats), DXGI_FORMAT_UNKNOWN);

    // This is a depth only render pass, so no fragment shader.
    std::string baseShaderPath = dataDir + "shadow_directional";
    const std::string vertexShaderPath = baseShaderPath + ".cvert";

    ShaderPaths paths;
    paths.vertex = vertexShaderPath;

    return CreatePSO(
        manager,
        device,
        paths,
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateEnvironmentCubemapLightPSO(
    PSOManager& manager,
    ID3D12Device2* device,
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
    ID3D12Device2* device,
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
    ID3D12Device2* device,
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
    ID3D12Device2* device,
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

ManagedPSORef CreateSkyboxLightMapsPSO(
    PSOManager& manager,
    ID3D12Device2* device,
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
        dataDir + "skybox_light_maps",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateSkyboxComputeLightMapsPSO(
    PSOManager& manager,
    ID3D12Device2* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    return CreateComputePSO(
        manager,
        device,
        dataDir + "skybox_compute_maps",
        rootSignature,
        desc
    );
}
