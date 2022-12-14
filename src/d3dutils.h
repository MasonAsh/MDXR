#pragma once

#include "util.h"

#include <directx/d3dx12.h>
#include <dxgi.h>

#include <iostream>
#include <algorithm>

#include <tiny_gltf.h>

// Store descriptor sizes globally for convenience
struct IncrementSizes
{
    int CbvSrvUav;
    int Rtv;
};
extern IncrementSizes G_IncrementSizes;

typedef ID3D12GraphicsCommandList4 GraphicsCommandList;

inline void PrintCapabilities(ID3D12Device* device, IDXGIAdapter1* adapter)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS featureSupport;
    ASSERT_HRESULT(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureSupport, sizeof(featureSupport)));
    switch (featureSupport.ResourceBindingTier)
    {
    case D3D12_RESOURCE_BINDING_TIER_1:
        DebugLog() << "Hardware is tier 1\n";
        break;

    case D3D12_RESOURCE_BINDING_TIER_2:
        // Tiers 1 and 2 are supported.
        DebugLog() << "Hardware is tier 2\n";
        break;

    case D3D12_RESOURCE_BINDING_TIER_3:
        // Tiers 1, 2, and 3 are supported.
        DebugLog() << "Hardware is tier 3\n";
        break;
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{};
    shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_7;
    ASSERT_HRESULT(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)));

    switch (shaderModel.HighestShaderModel) {
    case D3D_SHADER_MODEL_5_1:
        DebugLog() << "Shader model 5_1 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_0:
        DebugLog() << "Shader model 6_0 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_1:
        DebugLog() << "Shader model 6_1 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_2:
        DebugLog() << "Shader model 6_2 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_3:
        DebugLog() << "Shader model 6_3 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_4:
        DebugLog() << "Shader model 6_4 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_5:
        DebugLog() << "Shader model 6_5 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_6:
        DebugLog() << "Shader model 6_6 is supported\n";
        break;
    case D3D_SHADER_MODEL_6_7:
        DebugLog() << "Shader model 6_7 is supported\n";
        break;
    }

    {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info;
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &info
            ))) {
                DebugLog() << "\nVideo memory information:\n";
                DebugLog() << "\tBudget: " << info.Budget << " bytes\n";
                DebugLog() << "\tAvailable for reservation: " << info.AvailableForReservation << " bytes\n";
                DebugLog() << "\tCurrent usage: " << info.CurrentUsage << " bytes\n";
                DebugLog() << "\tCurrent reservation: " << info.CurrentReservation << " bytes\n\n";
            }
        }
    }
}

inline void CreateConstantBufferAndViews(
    ID3D12Device* device,
    ComPtr<ID3D12Resource>& buffer,
    size_t elementSize,
    UINT count,
    D3D12_CPU_DESCRIPTOR_HANDLE baseDescriptorHandle
)
{
    const UINT constantBufferSize = (UINT)elementSize * count;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
    ASSERT_HRESULT(
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&buffer)
        )
    );

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(baseDescriptorHandle);
    for (UINT i = 0; i < count; i++) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = buffer->GetGPUVirtualAddress() + (i * (UINT)elementSize);
        cbvDesc.SizeInBytes = (UINT)elementSize;
        device->CreateConstantBufferView(
            &cbvDesc,
            cpuHandle
        );
        cpuHandle.Offset(1, G_IncrementSizes.CbvSrvUav);
    }
}

inline D3D12_RESOURCE_DESC GetHDRImageDesc(int width, int height)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.DepthOrArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    UINT highestDimension = std::max((UINT)desc.Width, desc.Height);
    desc.MipLevels = (UINT16)std::floor(std::log2(highestDimension)) + 1;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    return desc;
}

inline D3D12_RESOURCE_DESC GetImageResourceDesc(const tinygltf::Image& image, bool isSRGB)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.DepthOrArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    // Generate as many mips as we can
    UINT highestDimension = std::max((UINT)desc.Width, desc.Height);
    desc.MipLevels = (UINT16)std::floor(std::log2(highestDimension)) + 1;

    CHECK(image.component == 4);

    switch (image.pixel_type) {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        desc.Format = !isSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        if (isSRGB) {
            DebugLog() << "16Bit image will not be treated as SRGB";
        }
        break;
    default:
        abort();
    };

    return desc;
}

inline void DeviceRemovedHandler(ID3D12Device* device)
{
    ComPtr<ID3D12DeviceRemovedExtendedData> pDred;
    ASSERT_HRESULT(device->QueryInterface(IID_PPV_ARGS(&pDred)));

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT DredAutoBreadcrumbsOutput;
    D3D12_DRED_PAGE_FAULT_OUTPUT DredPageFaultOutput;
    ASSERT_HRESULT(pDred->GetPageFaultAllocationOutput(&DredPageFaultOutput));
    ASSERT_HRESULT(pDred->GetAutoBreadcrumbsOutput(&DredAutoBreadcrumbsOutput));

    std::cout << "PAGE FAULT INFORMATION:\n"
        << "\tVirtualAddress: " << DredPageFaultOutput.PageFaultVA << "\n";

    std::cout << "DRED Breadcrumbs:\n";
    auto breadcrumb = DredAutoBreadcrumbsOutput.pHeadAutoBreadcrumbNode;
    while (breadcrumb) {
        std::cout << "\tCommandList: " << breadcrumb->pCommandListDebugNameA << "\n";
        std::cout << "\tBreadcrumbCount: " << breadcrumb->BreadcrumbCount << "\n";
        breadcrumb = breadcrumb->pNext;
    }
}

inline ComPtr<D3D12MA::Allocation> CreateUploadBufferWithData(D3D12MA::Allocator* allocator, void* srcData, size_t dataSize, UINT64 bufferSize = SIZE_MAX, void** mappedPtr = nullptr)
{
    if (bufferSize == SIZE_MAX) {
        bufferSize = dataSize;
    }

    if (bufferSize == 0) {
        return nullptr;
    }

    ComPtr<D3D12MA::Allocation> uploadBuffer;
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    ASSERT_HRESULT(
        allocator->CreateResource(&allocDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            &uploadBuffer,
            IID_NULL, nullptr
        )
    );

    void* dest;
    void** mapped = mappedPtr ? mappedPtr : &dest;
    uploadBuffer->GetResource()->Map(0, nullptr, mapped);

    memcpy(*mapped, srcData, dataSize);

    return uploadBuffer;
}

// Creates an upload buffer with the specified data, or reallocates an existing one if not big enough.
// The data will be updated if a suitable allocation already exists.
inline void CreateOrReallocateUploadBufferWithData(D3D12MA::Allocator* allocator, ComPtr<D3D12MA::Allocation>& allocation, void* srcData, size_t dataSize, UINT64 bufferSize = SIZE_MAX, void** mappedPtr = nullptr)
{
    if (bufferSize == SIZE_MAX) {
        bufferSize = dataSize;
    }

    if (bufferSize == 0) {
        return;
    }

    bool needsAlloc = !allocation || allocation->GetResource()->GetDesc().Width < bufferSize;
    if (needsAlloc) {
        allocation = CreateUploadBufferWithData(allocator, srcData, dataSize, bufferSize, mappedPtr);
    } else {
        void* dest;
        void** mapped = mappedPtr ? mappedPtr : &dest;
        allocation->GetResource()->Map(0, nullptr, mapped);

        memcpy(*mapped, srcData, dataSize);
    }
}

inline void CreateAccelerationStructureBuffers(
    D3D12MA::Allocator* allocator,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& prebuildInfo,
    ComPtr<D3D12MA::Allocation>& scratch,
    ComPtr<D3D12MA::Allocation>& result
)
{
    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    {
        CD3DX12_RESOURCE_DESC resDesc = CD3DX12_RESOURCE_DESC::Buffer(prebuildInfo.ScratchDataSizeInBytes);
        resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        bool needsAlloc = !scratch || scratch->GetResource()->GetDesc().Width < prebuildInfo.ScratchDataSizeInBytes;

        if (needsAlloc) {
            allocator->CreateResource(
                &allocDesc,
                &resDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                &scratch,
                IID_NULL, nullptr
            );
        }
    }

    {
        CD3DX12_RESOURCE_DESC resDesc = CD3DX12_RESOURCE_DESC::Buffer(prebuildInfo.ResultDataMaxSizeInBytes);
        resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        bool needsAlloc = !result || result->GetResource()->GetDesc().Width < prebuildInfo.ResultDataMaxSizeInBytes;

        if (needsAlloc) {
            allocator->CreateResource(
                &allocDesc,
                &resDesc,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                nullptr,
                &result,
                IID_NULL, nullptr
            );
        }
    }
}

template<class T>
inline T Align(T size, T alignment)
{
    return (size + (alignment - 1)) & ~(alignment - 1);
}