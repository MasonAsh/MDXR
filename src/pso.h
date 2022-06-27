#pragma once

#include <directx/d3dx12.h>
#include <wrl.h>

using namespace Microsoft::WRL;

#include "gbuffer.h"

struct IManagedPSO
{
public:
    virtual ID3D12PipelineState* Get() = 0;
    virtual void Reload(ID3D12Device* device) = 0;
};

struct PSOGraphicsShaderPaths
{
    std::wstring vertex;
    std::wstring pixel;
};

struct ManagedGraphicsPSO : public IManagedPSO
{
    ComPtr<ID3D12PipelineState> PSO;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    PSOGraphicsShaderPaths shaderPaths;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;

    ID3D12PipelineState* Get() override;
    void Reload(ID3D12Device* device) override;
};

struct ManagedComputePSO : public IManagedPSO
{
    ComPtr<ID3D12PipelineState> PSO;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
    std::wstring computeShaderPath;

    ID3D12PipelineState* Get() override;
    void Reload(ID3D12Device* device) override;
};

typedef std::shared_ptr<IManagedPSO> ManagedPSORef;

struct PSOManager
{
    std::vector<std::weak_ptr<IManagedPSO>> PSOs;

    void Reload(ID3D12Device* device);
};

ManagedPSORef CreateGraphicsPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const PSOGraphicsShaderPaths& paths,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc
);

ManagedPSORef SimpleCreateGraphicsPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& baseShaderPath,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc
);

ManagedPSORef CreateComputePSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& baseShaderPath,
    ID3D12RootSignature* rootSignature,
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc
);

ManagedPSORef CreateMipMapGeneratorPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature
);

ManagedPSORef CreateMeshPBRPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateMeshAlphaBlendedPBRPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateMeshUnlitPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateDirectionalLightPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateEnvironmentCubemapLightPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreatePointLightPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateSkyboxPSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateSkyboxDiffuseIrradiancePSO(
    PSOManager& manager,
    ID3D12Device* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);
