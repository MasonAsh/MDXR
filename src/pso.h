#pragma once

#include <directx/d3dx12.h>
#include <wrl.h>

using namespace Microsoft::WRL;

#include "gbuffer.h"
#include "crc32.h"
#include "util.h"

#include "d3dcompiler.h"
#include <D3D12MemAlloc.h>

#include <mutex>
#include <map>

struct ShaderByteCodeCache
{
    std::mutex mutex;
    std::vector<ComPtr<ID3DBlob>> blobs;
    std::map<std::string, D3D12_SHADER_BYTECODE> cache;

    D3D12_SHADER_BYTECODE Fetch(const std::string& filepath)
    {
        std::scoped_lock lock(mutex);

        auto cachedBytecode = cache.find(filepath);

        if (cachedBytecode != cache.end()) {
            return cachedBytecode->second;
        }

        ComPtr<ID3DBlob> blob;
        auto wfilepath = convert_to_wstring(filepath);

        if (!SUCCEEDED(D3DReadFileToBlob(wfilepath.c_str(), &blob))) {
            return D3D12_SHADER_BYTECODE{ nullptr, 0 };
        }

        blobs.push_back(blob);

        D3D12_SHADER_BYTECODE bytecode = { reinterpret_cast<UINT8*>(blob->GetBufferPointer()), blob->GetBufferSize() };
        auto item = cache.insert_or_assign(filepath, bytecode);

        return bytecode;
    }

    void Invalidate()
    {
        std::scoped_lock lock(mutex);

        blobs.clear();
        cache.clear();
    }
};

struct ShaderPaths
{
    std::string vertex;
    std::string pixel;
    std::string compute;
};

struct ManagedPSO
{
    ShaderPaths shaderPaths;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    CD3DX12_PIPELINE_STATE_STREAM desc;
    UINT hash;
    ComPtr<ID3D12PipelineState> PSO;

    bool Load(ShaderByteCodeCache& cache)
    {
        D3D12_SHADER_BYTECODE VS{ nullptr, 0 };
        D3D12_SHADER_BYTECODE PS{ nullptr, 0 };
        D3D12_SHADER_BYTECODE CS{ nullptr, 0 };

        if (!shaderPaths.vertex.empty()) {
            VS = cache.Fetch(shaderPaths.vertex);
            if (!VS.pShaderBytecode) {
                return false;
            }
        }

        if (!shaderPaths.pixel.empty()) {
            PS = cache.Fetch(shaderPaths.pixel);
            if (!PS.pShaderBytecode) {
                return false;
            }
        }

        if (!shaderPaths.compute.empty()) {
            CS = cache.Fetch(shaderPaths.compute);
            if (!CS.pShaderBytecode) {
                return false;
            }
        }

        desc.VS = VS;
        desc.PS = PS;
        desc.CS = CS;

        ComputeHash();

        return true;
    }

    void Reload(ID3D12Device5* device, ShaderByteCodeCache& cache)
    {
        auto oldVS = desc.VS;
        auto oldPS = desc.PS;
        auto oldCS = desc.CS;

        if (!Load(cache)) {
            return;
        }

        if (!SUCCEEDED(Compile(device))) {
            desc.VS = oldVS;
            desc.PS = oldPS;
            desc.CS = oldCS;
        }
    }

    HRESULT Compile(ID3D12Device5* device)
    {
        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{};
        streamDesc.SizeInBytes = sizeof(desc);
        streamDesc.pPipelineStateSubobjectStream = reinterpret_cast<void*>(&desc);
        return device->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&PSO));
    }

    void ComputeHash()
    {
        hash =
            crc32b(reinterpret_cast<unsigned char*>(&desc.Flags), sizeof(desc.Flags)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.NodeMask), sizeof(desc.NodeMask)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.pRootSignature), sizeof(desc.pRootSignature)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.IBStripCutValue), sizeof(desc.IBStripCutValue)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.PrimitiveTopologyType), sizeof(desc.PrimitiveTopologyType)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.VS), sizeof(desc.VS)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.GS), sizeof(desc.GS)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.PS), sizeof(desc.PS)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.StreamOutput), sizeof(desc.StreamOutput)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.HS), sizeof(desc.HS)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.DS), sizeof(desc.DS)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.PS), sizeof(desc.PS)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.CS), sizeof(desc.CS)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.BlendState), sizeof(desc.BlendState)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.DepthStencilState), sizeof(desc.DepthStencilState)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.DSVFormat), sizeof(desc.DSVFormat)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.RasterizerState), sizeof(desc.RasterizerState)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.RTVFormats), sizeof(desc.RTVFormats)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.SampleDesc), sizeof(desc.SampleDesc)) +
            crc32b(reinterpret_cast<unsigned char*>(&desc.SampleMask), sizeof(desc.SampleMask));

        for (const auto& inputLayoutElement : inputLayout) {
            hash += crc32b(
                reinterpret_cast<const unsigned char* const>(&inputLayoutElement.SemanticName),
                strlen(inputLayoutElement.SemanticName)
            );

            hash += crc32b(
                reinterpret_cast<const unsigned char* const>(&inputLayoutElement.SemanticIndex),
                sizeof(inputLayoutElement.SemanticIndex)
            );

            hash += crc32b(
                reinterpret_cast<const unsigned char* const>(&inputLayoutElement.Format),
                sizeof(inputLayoutElement.Format)
            );

            hash += crc32b(
                reinterpret_cast<const unsigned char* const>(&inputLayoutElement.InputSlot),
                sizeof(inputLayoutElement.InputSlot)
            );

            hash += crc32b(
                reinterpret_cast<const unsigned char* const>(&inputLayoutElement.AlignedByteOffset),
                sizeof(inputLayoutElement.AlignedByteOffset)
            );

            hash += crc32b(
                reinterpret_cast<const unsigned char* const>(&inputLayoutElement.InputSlotClass),
                sizeof(inputLayoutElement.InputSlotClass)
            );

            hash += crc32b(
                reinterpret_cast<const unsigned char* const>(&inputLayoutElement.InstanceDataStepRate),
                sizeof(inputLayoutElement.InstanceDataStepRate)
            );
        }
    }

    ID3D12PipelineState* Get() const
    {
        return PSO.Get();
    }
};

// Managed ID3D12StateObject for DXR.
//
// At this point, DX12 does not support using state objects for graphics and compute
// pipelines, so we need this completely different system for ray tracing pipelines :/
struct RayTraceStateObject
{
    ComPtr<ID3D12StateObject> SO;
    std::string dxilLibPath;
    CD3DX12_STATE_OBJECT_DESC desc{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

    bool Load(ShaderByteCodeCache& cache)
    {
        auto DXIL = cache.Fetch(dxilLibPath);
        if (!DXIL.pShaderBytecode) {
            return false;
        }

        auto DXILLib = desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
        DXILLib->SetDXILLibrary(&DXIL);

        return true;
    }

    HRESULT Compile(
        ID3D12Device5* device
    )
    {
        return device->CreateStateObject(
            desc,
            IID_PPV_ARGS(&SO)
        );
    }
};

typedef std::shared_ptr<ManagedPSO> ManagedPSORef;
typedef std::shared_ptr<RayTraceStateObject> RayTraceStateObjectRef;

struct PSOManager
{
    std::vector<std::weak_ptr<ManagedPSO>> PSOs;
    ShaderByteCodeCache shaderByteCodeCache;

    void Reload(ID3D12Device5* device);
    ManagedPSORef FindPSO(UINT hash);
};

ManagedPSORef CreatePSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const ShaderPaths& paths,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    CD3DX12_PIPELINE_STATE_STREAM psoDesc
);

ManagedPSORef SimpleCreateGraphicsPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& baseShaderPath,
    ID3D12RootSignature* rootSignature,
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout,
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc
);

ManagedPSORef CreateComputePSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& baseShaderPath,
    ID3D12RootSignature* rootSignature,
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc
);

ManagedPSORef CreateMipMapGeneratorPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature
);

ManagedPSORef CreateMeshPBRPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateMeshAlphaBlendedPBRPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateMeshUnlitPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateMeshUnlitTexturedPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateDirectionalLightPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateDirectionalLightShadowMapPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateEnvironmentCubemapLightPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreatePointLightPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateSkyboxPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateSkyboxDiffuseIrradiancePSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateSkyboxLightMapsPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateSkyboxComputeLightMapsPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateToneMapPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

ManagedPSORef CreateDebugVisualizerPSO(
    PSOManager& manager,
    ID3D12Device5* device,
    const std::string& dataDir,
    ID3D12RootSignature* rootSignature,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout
);

// struct RTShaderTable
// {
//     ComPtr<D3D12MA::Allocation> allocation;

//     D3D12_GPU_VIRTUAL_ADDRESS_RANGE RayGenerationShaderRecord;
//     D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE MissShaderTable;
//     D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE HitGroupTable;
//     D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE CallableShaderTable;
// };

// RayTraceStateObjectRef CreateRTShadowSO(
//     PSOManager& manager,
//     ID3D12Device5* device,
//     D3D12MA::Allocator* allocator,
//     const std::string& dataDir,
//     ID3D12RootSignature* rootSignature,
//     RTShaderTable* shaderTable
// );