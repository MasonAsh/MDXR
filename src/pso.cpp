#include "pso.h"
#include "util.h"

#include <iostream>
#include <mutex>
#include <map>

#include <d3dcompiler.h>

#include "d3dutils.h"

void PSOManager::Reload(ID3D12Device5* device)
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
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    return psoDesc;
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC DefaultLightPSODesc()
{
    // Unlike other PSOs, we go clockwise here.
    CD3DX12_RASTERIZER_DESC rasterizerState(D3D12_DEFAULT);

    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    // Only write lighting to areas in the stencil mask
    psoDesc.DepthStencilState.StencilEnable = TRUE;
    psoDesc.DepthStencilState.StencilReadMask = 0xff;
    psoDesc.DepthStencilState.StencilWriteMask = 0x00;
    psoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    psoDesc.DepthStencilState.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    psoDesc.RasterizerState = rasterizerState;

    psoDesc.RTVFormats[0] = GBufferResourceDesc(GBuffer_Radiance, 0, 0).Format;

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

    return psoDesc;
}

std::mutex g_PSOMutex;

ManagedPSORef CreatePSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const ShaderPaths& paths,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    CD3DX12_PIPELINE_STATE_STREAM psoDesc
)
{
    std::scoped_lock<std::mutex> lock(g_PSOMutex);

    auto mPso = std::make_shared<ManagedPSO>();
    mPso->desc = psoDesc;
    mPso->inputLayout = inputLayout;
    mPso->desc.pRootSignature = rootSignature;
    mPso->desc.InputLayout = { mPso->inputLayout.data(), (UINT)mPso->inputLayout.size() };
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
    ID3D12Device5* device,
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
    ID3D12Device5* device,
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
    ID3D12Device5* device,
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
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();

    psoDesc.NumRenderTargets = GBuffer_RTVCount + 1;
    for (UINT i = 0; i < psoDesc.NumRenderTargets; i++) {
        psoDesc.RTVFormats[i] = GBufferResourceDesc((GBufferTarget)(i), 0, 0).Format;
    }

    // Use stencil to indicate which pixels are drawn in the GBuffer pipeline.
    psoDesc.DepthStencilState.StencilEnable = TRUE;
    psoDesc.DepthStencilState.StencilWriteMask = 0xff;

    // Always write front faces
    psoDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    psoDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    // Ignore backfaces
    psoDesc.DepthStencilState.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    psoDesc.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_NEVER;

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
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.NumRenderTargets = 1;
    // for (UINT i = 1; i < psoDesc.NumRenderTargets; i++) {
    //     psoDesc.RTVFormats[i] = GBufferResourceDesc((GBufferTarget)(i - 1), 0, 0).Format;
    // }

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

    psoDesc.RTVFormats[0] = GBufferResourceDesc(GBuffer_Radiance, 0, 0).Format;

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
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();

    psoDesc.NumRenderTargets = GBuffer_RTVCount + 1;
    for (UINT i = 0; i < psoDesc.NumRenderTargets; i++) {
        psoDesc.RTVFormats[i] = GBufferResourceDesc((GBufferTarget)i, 0, 0).Format;
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

ManagedPSORef CreateMeshUnlitTexturedPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();

    psoDesc.NumRenderTargets = GBuffer_RTVCount + 1;
    for (UINT i = 0; i < psoDesc.NumRenderTargets; i++) {
        psoDesc.RTVFormats[i] = GBufferResourceDesc((GBufferTarget)i, 0, 0).Format;
    }

    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "mesh_gbuffer_unlit_textured",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

ManagedPSORef CreateDirectionalLightPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultLightPSODesc();

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
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // These shadow maps are depth only - no stencil
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    float depthBias = -0.0005f;
    psoDesc.RasterizerState.DepthBias = static_cast<int>(-(depthBias / (1.0f / pow(2.0f, 23.0f))));
    psoDesc.RasterizerState.SlopeScaledDepthBias = -0.005f;
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
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultLightPSODesc();

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
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    // Unlike other PSOs, we go clockwise here.
    auto psoDesc = DefaultLightPSODesc();

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
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
)
{
    auto psoDesc = DefaultGraphicsPSODesc();
    CD3DX12_RASTERIZER_DESC rasterizerState(D3D12_DEFAULT);
    rasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState = rasterizerState;
    psoDesc.RTVFormats[0] = GBufferResourceDesc(GBuffer_Radiance, 0, 0).Format;

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
    ID3D12Device5* device,
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
    ID3D12Device5* device,
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
    ID3D12Device5* device,
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

ManagedPSORef CreateToneMapPSO(
    PSOManager& manager,
    ID3D12Device5* device,
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
    D3D12_RENDER_TARGET_BLEND_DESC blendDesc = {};
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

    // Back buffer is SRGB format
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

    return SimpleCreateGraphicsPSO(
        manager,
        device,
        dataDir + "tonemap",
        rootSignature,
        inputLayout,
        psoDesc
    );
}

/// Wrote this before learning about DXR 1.1
/// Leaving it here for reference in case old style ray tracing is needed in the future.

// struct ShaderRecord
// {
//     void* identifier;
//     UINT size;
// };
// RayTraceStateObjectRef CreateRTShadowSO(
//     PSOManager& manager,
//     ID3D12Device5* device,
//     D3D12MA::Allocator* allocator,
//     const std::string& dataDir,
//     ID3D12RootSignature* rootSignature,
//     RTShaderTable* shaderTable
// )
// {
//     RayTraceStateObjectRef obj = RayTraceStateObjectRef(new RayTraceStateObject);

//     {
//         auto rtConfig = obj->desc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
//         UINT maxRecursionDepth = 2; // Primary and shadow rays
//         rtConfig->Config(maxRecursionDepth);
//     }

//     const auto RTRayGenName = L"RTRayGen";
//     const auto RTMissName = L"RTMiss";
//     const auto RTPrimaryRayClosestHitName = L"RTPrimaryRayClosestHit";
//     const auto RTShadowRayClosestHitName = L"RTShadowRayClosestHit";
//     const auto PrimaryHitGroupName = L"PrimaryHitGroup";
//     const auto ShadowHitGroupName = L"ShadowHitGroup";

//     // Configure DXIL library
//     {
//         obj->dxilLibPath = dataDir + "ray_shadows.clib";
//         auto DXIL = manager.shaderByteCodeCache.Fetch(obj->dxilLibPath);
//         auto dxilLib = obj->desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
//         dxilLib->SetDXILLibrary(
//             &DXIL
//         );

//         dxilLib->DefineExport(
//             RTRayGenName
//         );

//         dxilLib->DefineExport(
//             RTMissName
//         );

//         dxilLib->DefineExport(
//             RTPrimaryRayClosestHitName
//         );

//         dxilLib->DefineExport(
//             RTShadowRayClosestHitName
//         );
//     }

//     // Shader config
//     {
//         auto shaderConfig = obj->desc.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
//         UINT payloadSize = sizeof(float); // float shade;
//         UINT attributeSize = 2 * sizeof(float); // built in triangle intersection
//         shaderConfig->Config(payloadSize, attributeSize);
//     }

//     const int NUM_HIT_GROUPS = 2;

//     {
//         auto primaryHitGroup = obj->desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
//         primaryHitGroup->SetClosestHitShaderImport(
//             RTPrimaryRayClosestHitName
//         );
//         primaryHitGroup->SetHitGroupExport(
//             PrimaryHitGroupName
//         );
//         primaryHitGroup->SetHitGroupType(
//             D3D12_HIT_GROUP_TYPE_TRIANGLES
//         );
//     }

//     {
//         auto shadowHitGroup = obj->desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
//         shadowHitGroup->SetClosestHitShaderImport(
//             RTShadowRayClosestHitName
//         );
//         shadowHitGroup->SetHitGroupExport(
//             ShadowHitGroupName
//         );
//         shadowHitGroup->SetHitGroupType(
//             D3D12_HIT_GROUP_TYPE_TRIANGLES
//         );
//     }

//     // Seems we can use the same graphics root signature here.
//     {
//         auto globalRootSignature = obj->desc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
//         globalRootSignature->SetRootSignature(rootSignature);
//     }

//     ASSERT_HRESULT(obj->Compile(device));

//     ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
//     ASSERT_HRESULT(obj->SO.As(&stateObjectProperties));

//     void* rayGenIdentifier = stateObjectProperties->GetShaderIdentifier(
//         RTRayGenName
//     );

//     void* primaryHitIdentifier = stateObjectProperties->GetShaderIdentifier(
//         PrimaryHitGroupName
//     );

//     void* shadowHitIdentifier = stateObjectProperties->GetShaderIdentifier(
//         ShadowHitGroupName
//     );

//     void* missIdentifier = stateObjectProperties->GetShaderIdentifier(
//         RTMissName
//     );

//     const int NUM_MISS_SHADERS = 1;

//     const void* identifiers[] = {
//         rayGenIdentifier,
//         primaryHitIdentifier,
//         shadowHitIdentifier,
//         missIdentifier
//     };

//     UINT shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

//     UINT8* destPtr;
//     shaderTable->allocation = CreateUploadBufferWithData(
//         allocator,
//         nullptr,
//         0,
//         shaderIdentifierSize * (_countof(identifiers) + 4),
//         reinterpret_cast<void**>(&destPtr)
//     );

//     // for (int i = 0; i < _countof(identifiers); i++) {
//     //     memcpy(destPtr + i * shaderIdentifierSize, identifiers[i], shaderIdentifierSize);
//     // }

//     // Copy ray gen identifier
//     UINT8* target = destPtr;
//     memcpy(target, identifiers[0], shaderIdentifierSize);
//     target += shaderIdentifierSize;
//     target = reinterpret_cast<UINT8*>(
//         Align<UINT64>(reinterpret_cast<UINT64>(target), D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT)
//         );

//     // Hit groups
//     memcpy(target, identifiers[1], shaderIdentifierSize);
//     target += shaderIdentifierSize;
//     memcpy(target, identifiers[2], shaderIdentifierSize);
//     target += shaderIdentifierSize;
//     target = reinterpret_cast<UINT8*>(
//         Align<UINT64>(reinterpret_cast<UINT64>(target), D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT)
//         );

//     memcpy(target, identifiers[3], shaderIdentifierSize);

//     shaderTable->RayGenerationShaderRecord.StartAddress = shaderTable->allocation->GetResource()->GetGPUVirtualAddress();
//     shaderTable->RayGenerationShaderRecord.SizeInBytes = shaderIdentifierSize;

//     shaderTable->HitGroupTable.StrideInBytes = shaderIdentifierSize;
//     shaderTable->HitGroupTable.SizeInBytes = NUM_HIT_GROUPS * shaderIdentifierSize;
//     shaderTable->HitGroupTable.StartAddress =
//         Align<UINT64>(shaderTable->RayGenerationShaderRecord.StartAddress + shaderTable->RayGenerationShaderRecord.SizeInBytes, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

//     shaderTable->MissShaderTable.StartAddress =
//         Align<UINT64>(shaderTable->HitGroupTable.StartAddress + shaderTable->HitGroupTable.SizeInBytes, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
//     shaderTable->MissShaderTable.StrideInBytes = shaderIdentifierSize;
//     shaderTable->MissShaderTable.SizeInBytes = shaderIdentifierSize * NUM_MISS_SHADERS;

//     shaderTable->CallableShaderTable.SizeInBytes = 0;
//     shaderTable->CallableShaderTable.StartAddress = 0;
//     shaderTable->CallableShaderTable.StrideInBytes = 0;

//     return obj;
// }