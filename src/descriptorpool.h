#pragma once

#include "util.h"

#include <directx/d3dx12.h>
#include <D3D12MemAlloc.h>
#include <wrl.h>
#include <mutex>

using namespace Microsoft::WRL;

struct DescriptorRef
{

    DescriptorRef()
        : heap(nullptr)
        , incrementSize(0)
        , index(-1)
    {
    }

    DescriptorRef(ID3D12DescriptorHeap* heap, UINT index, int incrementSize)
        : heap(heap)
        , index(index)
        , incrementSize(incrementSize)
    {
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE CPUHandle(int offset = 0) const
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(), index + offset, incrementSize);
    }

    CD3DX12_GPU_DESCRIPTOR_HANDLE GPUHandle() const
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), index, incrementSize);
    }

    DescriptorRef operator+(int offset) const
    {
        return DescriptorRef(heap, index + offset, incrementSize);
    }

    void AssignConstantBufferView(ID3D12Device* device, ID3D12Resource* constantBuffer, UINT64 byteOffset, UINT size)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress() + byteOffset;
        cbvDesc.SizeInBytes = size;
        device->CreateConstantBufferView(
            &cbvDesc,
            CPUHandle()
        );
    }

    bool IsValid() const
    {
        return index != -1;
    }

    UINT Index() const
    {
        return index;
    }

    ID3D12DescriptorHeap* heap;
    UINT incrementSize;
    UINT index;
};

struct DescriptorAlloc
{
    D3D12MA::VirtualAllocation alloc;
    UINT index;

    ID3D12DescriptorHeap* heap;
    UINT incrementSize;

    CD3DX12_CPU_DESCRIPTOR_HANDLE CPUHandle(int offset = 0) const
    {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(), index + offset, incrementSize);
    }

    CD3DX12_GPU_DESCRIPTOR_HANDLE GPUHandle(int offset = 0) const
    {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(), index + offset, incrementSize);
    }

    void AssignConstantBufferView(ID3D12Device* device, ID3D12Resource* constantBuffer, UINT64 byteOffset, UINT size)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress() + byteOffset;
        cbvDesc.SizeInBytes = size;
        device->CreateConstantBufferView(
            &cbvDesc,
            CPUHandle()
        );
    }

    bool IsValid() const
    {
        return index != -1;
    }

    DescriptorRef Ref(int offset = 0)
    {
        DescriptorRef ref;
        ref.heap = heap;
        ref.incrementSize = incrementSize;
        ref.index = index;

        return ref + offset;
    }
};

class DescriptorPool
{
public:
    DescriptorPool()
    {
    }

    ~DescriptorPool()
    {
        block->Clear();
    }

    ID3D12DescriptorHeap* Heap() const
    {
        return descriptorHeap.Get();
    }

    void Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_DESC heapDesc, const std::string& debugName)
    {
        this->debugName = debugName;
        descriptorIncrementSize = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        ASSERT_HRESULT(
            device->CreateDescriptorHeap(
                &heapDesc,
                IID_PPV_ARGS(&descriptorHeap)
            )
        );

        D3D12MA::VIRTUAL_BLOCK_DESC blockDesc{};
        blockDesc.Size = heapDesc.NumDescriptors;
        D3D12MA::CreateVirtualBlock(&blockDesc, &block);
    }

    // Allocates a group of descriptors.
    // Descriptors must be freed with FreeDescriptors.
    // For automatic freeing use UniqueDescriptors/AllocateDescriptorsUnique instead.
    DescriptorAlloc AllocateDescriptors(UINT count, const char* debugName)
    {
        DescriptorAlloc descriptorAlloc;
        D3D12MA::VirtualAllocation allocation;
        UINT64 index;

        {
            D3D12MA::VIRTUAL_ALLOCATION_DESC allocDesc{};
            allocDesc.Size = count;
            block->Allocate(&allocDesc, &allocation, &index);
        }

        descriptorAlloc.alloc = allocation;
        descriptorAlloc.index = assert_cast<UINT>(index);
        descriptorAlloc.incrementSize = descriptorIncrementSize;
        descriptorAlloc.heap = descriptorHeap.Get();

        if (debugName != nullptr) {
            DebugLog() << this->debugName << " allocation info: " <<
                "\n\tIndex: " << index <<
                "\n\tCount: " << count <<
                "\n\tReason: " << debugName << "\n";
        }

        return descriptorAlloc;
    }

    void FreeDescriptors(const DescriptorAlloc& alloc)
    {
        block->FreeAllocation(alloc.alloc);
    }
private:
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    ComPtr<D3D12MA::VirtualBlock> block;
    std::string debugName;
    UINT descriptorIncrementSize;
    std::mutex mutex;
};

// A "unique pointer" to a group of descriptors allocated from DescriptorPool
struct UniqueDescriptors
{
    DescriptorPool* pool;
    DescriptorAlloc allocation;

    UniqueDescriptors()
        : pool(nullptr)
    {
    }

    UniqueDescriptors(DescriptorPool* pool, DescriptorAlloc allocation)
        : pool(pool)
        , allocation(allocation)
    {
    }

    UniqueDescriptors(UniqueDescriptors&& rhs)
    {
        pool = std::move(rhs.pool);
        allocation = std::move(rhs.allocation);
        rhs.pool = nullptr;
        rhs.allocation = DescriptorAlloc{};
    }

    UniqueDescriptors(const UniqueDescriptors& other) = delete;

    ~UniqueDescriptors()
    {
        if (pool) {
            pool->FreeDescriptors(allocation);
        }
    }

    UniqueDescriptors& operator=(const UniqueDescriptors& rhs) = delete;

    UniqueDescriptors& operator=(UniqueDescriptors&& rhs)
    {
        pool = std::move(rhs.pool);
        allocation = std::move(rhs.allocation);
        rhs.pool = nullptr;
        rhs.allocation = DescriptorAlloc{};

        return (*this);
    }


    CD3DX12_CPU_DESCRIPTOR_HANDLE CPUHandle(int offset = 0) const
    {
        return allocation.CPUHandle(offset);
    }

    CD3DX12_GPU_DESCRIPTOR_HANDLE GPUHandle(int offset = 0) const
    {
        return allocation.GPUHandle(offset);
    }

    UINT Index() const
    {
        return allocation.index;
    }

    bool IsValid() const
    {
        return allocation.IsValid();
    }

    DescriptorRef Ref(int offset = 0)
    {
        DescriptorRef ref;
        ref.heap = allocation.heap;
        ref.incrementSize = allocation.incrementSize;
        ref.index = allocation.index;

        return ref + offset;
    }
};

static UniqueDescriptors AllocateDescriptorsUnique(DescriptorPool& pool, int count, const char* debugName)
{
    DescriptorAlloc alloc = pool.AllocateDescriptors(count, debugName);
    return UniqueDescriptors(&pool, alloc);
}