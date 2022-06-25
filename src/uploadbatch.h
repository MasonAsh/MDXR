#pragma once

#include "util.h"
#include <D3D12MemAlloc.h>
#include <directx/d3dx12.h>

class UploadBatch
{
public:
    const UINT64 MaxUploadSize = (UINT64)1000 * 1000 * 1000;

    // Begins an upload batch.
    // The batch has full control of the command queue during this time.
    void Begin(D3D12MA::Allocator* allocator, ID3D12CommandQueue* commandQueue, IncrementalFence* fence)
    {
        this->allocator = allocator;
        this->fence = fence;
        this->commandQueue = commandQueue;

        commandQueue->GetDevice(IID_PPV_ARGS(&device));
        device->Release();

        device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COPY,
            IID_PPV_ARGS(&commandAllocator)
        );

        ASSERT_HRESULT(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_COPY,
                commandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&commandList)
            )
        );


        D3D12MA::Budget budget;
        this->allocator->GetBudget(&budget, nullptr);

        // Upload up to 1GB at a time
        uploadBufferSize = std::min(MaxUploadSize, budget.BudgetBytes / 2);
        D3D12MA::VIRTUAL_BLOCK_DESC blockDesc{};
        blockDesc.Size = uploadBufferSize;
        D3D12MA::CreateVirtualBlock(&blockDesc, &virtualBlock);

        auto uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        ASSERT_HRESULT(
            allocator->CreateResource(&allocDesc,
                &uploadBufferDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &this->uploadBuffer,
                IID_NULL,
                nullptr
            )
        );

        uploadBuffer->GetResource()->Map(0, nullptr, reinterpret_cast<void**>(&uploadDataPtr));
    }

    void AddTexture(ID3D12Resource* destResource, D3D12_SUBRESOURCE_DATA* subresourceData, int subresource, int numSubresources)
    {
        // Suballocate one resource at a time. It's just easier this way and the end effect should be the same.
        for (int i = 0; i < numSubresources; i++) {
            UINT64 requiredBytes = GetRequiredIntermediateSize(destResource, i, 1);

            // FIXME: This case should be possible to handle
            assert(requiredBytes <= uploadBufferSize);

            UINT64 offset;
            D3D12MA::VirtualAllocation allocation;
            D3D12MA::VIRTUAL_ALLOCATION_DESC allocDesc = {};
            allocDesc.Alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
            allocDesc.Size = requiredBytes;
            if (!SUCCEEDED(virtualBlock->Allocate(
                &allocDesc,
                &allocation,
                &offset))) {
                Flush();

                // This *should* work 100%, as we've made sure we're not uploading >
                // uploadBufferSize at once at this point.
                ASSERT_HRESULT(
                    virtualBlock->Allocate(
                        &allocDesc,
                        &allocation,
                        &offset
                    )
                );
            }

            auto resourceDesc = destResource->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            device->GetCopyableFootprints(
                &resourceDesc,
                i,
                1,
                offset,
                &footprint,
                nullptr,
                nullptr,
                &requiredBytes
            );

            for (UINT y = 0; y < footprint.Footprint.Height; y++) {
                UINT8* pSrcPtr = (UINT8*)subresourceData[i].pData + y * footprint.Footprint.RowPitch;
                UINT8* pDestPtr = uploadDataPtr + footprint.Offset + y * footprint.Footprint.RowPitch;
                memcpy(pDestPtr, pSrcPtr, subresourceData[i].RowPitch);
            }

            const CD3DX12_TEXTURE_COPY_LOCATION Dst(destResource, i);
            const CD3DX12_TEXTURE_COPY_LOCATION Src(uploadBuffer->GetResource(), footprint);
            commandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        }
    }

    void AddBuffer(ID3D12Resource* destinationResource, size_t destOffset, void* srcData, size_t numBytes)
    {
        // If a big resource can't fit into one upload, split into two. This
        // will recurse, so if a resource is somehow uploadBufferSize * 2, then
        // it will get split in recurse calls.
        if (numBytes > uploadBufferSize) {
            Flush();
            int leftoverBytes = numBytes - uploadBufferSize;
            AddBuffer(destinationResource, destOffset, srcData, uploadBufferSize);
            AddBuffer(destinationResource, destOffset + uploadBufferSize, srcData, leftoverBytes);
            return;
        }

        UINT64 offset;
        D3D12MA::VirtualAllocation allocation;
        D3D12MA::VIRTUAL_ALLOCATION_DESC allocDesc = {};
        allocDesc.Alignment = sizeof(float);
        allocDesc.Size = numBytes;
        if (!SUCCEEDED(virtualBlock->Allocate(
            &allocDesc,
            &allocation,
            &offset))) {
            Flush();

            // This *should* work 100%, as we've made sure we're not uploading >
            // uploadBufferSize at once at this point.
            ASSERT_HRESULT(
                virtualBlock->Allocate(
                    &allocDesc,
                    &allocation,
                    &offset
                )
            );
        }

        memcpy(uploadDataPtr + offset, srcData, numBytes);

        commandList->CopyBufferRegion(destinationResource,
            destOffset,
            uploadBuffer->GetResource(),
            offset,
            numBytes
        );
    }

    void Finish()
    {
        Flush();

        ASSERT_HRESULT(commandList->Close());
    }
private:
    void Flush()
    {
        commandList->Close();
        ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, ppCommandLists);
        this->fence->SignalAndWait(commandQueue);
        virtualBlock->Clear();

        ASSERT_HRESULT(commandList->Reset(commandAllocator.Get(), nullptr));
    }

    ID3D12CommandQueue* commandQueue;
    ComPtr<ID3D12CommandAllocator> commandAllocator;

    ComPtr<ID3D12GraphicsCommandList> commandList;
    IncrementalFence* fence;

    ComPtr<D3D12MA::Allocation> uploadBuffer;
    UINT64 uploadBufferSize;
    UINT8* uploadDataPtr;

    D3D12MA::Allocator* allocator;
    ID3D12Device* device;
    D3D12MA::VirtualBlock* virtualBlock;
};