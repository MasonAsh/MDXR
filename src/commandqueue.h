#pragma once

#include "util.h"
#include "incrementalfence.h"

#include <directx/d3dx12.h>
#include <dxgi.h>

#include <mutex>

// Wrapper for a command queue, a fence, and a mutex to synchronize calls to ExecuteCommandLists
class CommandQueue
{
public:
    void Initialize(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE commandListType)
    {
        this->commandListType = commandListType;

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = commandListType;
        device->CreateCommandQueue(
            &queueDesc,
            IID_PPV_ARGS(&commandQueue)
        );

        fence.Initialize(device);
    }

    ID3D12CommandQueue* GetInternal()
    {
        return commandQueue.Get();
    }

    void ExecuteCommandLists(std::initializer_list<ID3D12CommandList* const> commandLists, FenceEvent& fenceEvent, FenceEvent waitEvent = FenceEvent())
    {
#ifdef MDXR_DEBUG
        for (auto& commandList : commandLists) {
            CHECK(commandList->GetType() == commandListType);
        }
#endif

        std::scoped_lock lock{ mutex };

        waitEvent.sourceFence->WaitQueue(commandQueue.Get(), waitEvent);
        commandQueue->ExecuteCommandLists(assert_cast<UINT>(commandLists.size()), commandLists.begin());
        fence.SignalQueue(commandQueue.Get(), fenceEvent);
    }

    void ExecuteCommandListsBlocking(std::initializer_list<ID3D12CommandList* const> commandLists, FenceEvent waitEvent = FenceEvent())
    {
#ifdef MDXR_DEBUG
        for (auto& commandList : commandLists) {
            CHECK(commandList->GetType() == commandListType);
        }
#endif

        FenceEvent fenceEvent;

        {
            // We must synchronize the calls to the queue, but don't want to keep the lock for
            // the CPU wait.
            std::scoped_lock lock{ mutex };

            waitEvent.sourceFence->WaitQueue(commandQueue.Get(), waitEvent);
            commandQueue->ExecuteCommandLists(assert_cast<UINT>(commandLists.size()), commandLists.begin());
            fence.SignalQueue(commandQueue.Get(), fenceEvent);
        }

        WaitForEventCPU(fenceEvent);
    }

    // This is necessary because Present must happen before signaling the queue.
    void ExecuteCommandListsAndPresentBlocking(
        std::initializer_list<ID3D12CommandList* const> commandLists,
        IDXGISwapChain* swapChain,
        UINT syncInterval,
        UINT presentFlags,
        FenceEvent waitEvent = FenceEvent()
    )
    {
#ifdef MDXR_DEBUG
        for (auto& commandList : commandLists) {
            CHECK(commandList->GetType() == commandListType);
        }
#endif

        FenceEvent fenceEvent;

        {
            // We must synchronize the calls to the queue, but don't want to keep the lock for
            // the CPU wait.
            std::scoped_lock lock{ mutex };

            waitEvent.sourceFence->WaitQueue(commandQueue.Get(), waitEvent);
            commandQueue->ExecuteCommandLists(assert_cast<UINT>(commandLists.size()), commandLists.begin());
            swapChain->Present(syncInterval, presentFlags);
            fence.SignalQueue(commandQueue.Get(), fenceEvent);
        }

        WaitForEventCPU(fenceEvent);
    }

    void WaitForEventCPU(FenceEvent& fenceEvent)
    {
        fenceEvent.sourceFence->WaitCPU(fenceEvent);
    }
private:
    ComPtr<ID3D12CommandQueue> commandQueue;
    IncrementalFence fence;
    D3D12_COMMAND_LIST_TYPE commandListType;
    std::mutex mutex;
};