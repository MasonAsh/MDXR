#pragma once

#include "util.h"
#include "incrementalfence.h"

#include <directx/d3dx12.h>
#include <dxgi.h>

#include <mutex>
#include <span>

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

#if 0
    ID3D12GraphicsCommandList* GetCommandList(ID3D12Device* device, ID3D12PipelineState* initialState)
    {
        std::scoped_lock<std::mutex> lock(mutex);

        CommandListEntry* entry = nullptr;

        for (auto& commandListEntry : commandLists) {
            if (!commandListEntry.isRecording) {
                entry = &commandListEntry;
            }
        }

        if (entry) {
            ASSERT_HRESULT(entry->commandAllocator->Reset());
            ASSERT_HRESULT(entry->commandList->Reset(
                entry->commandAllocator.Get(),
                initialState
            ));

        } else {
            AppendCommandList(device, initialState);
            entry = &commandLists.back();
        }

        entry->isRecording = true;

        return commandLists.back().commandList.Get();
    }
#endif

    void ExecuteCommandLists(std::span<ID3D12CommandList* const> commandLists, FenceEvent& fenceEvent, std::initializer_list<FenceEvent> waitEvents = {})
    {
#ifdef MDXR_DEBUG
        for (auto& commandList : commandLists) {
            CHECK(commandList->GetType() == commandListType);
        }
#endif

        std::scoped_lock lock{ mutex };

#if 0
        // Clear the recording flag for command lists that were allocated from this pool.
        for (auto& commandList : commandLists) {
            for (auto& entry : this->commandLists) {
                if (entry.commandList.Get() == commandList) {
                    entry.commandAllocator->Reset();
                    entry.isRecording = false;
                }
            }
        }
#endif

        for (const FenceEvent& waitEvent : waitEvents) {
            waitEvent.sourceFence->WaitQueue(commandQueue.Get(), waitEvent);
        }
        commandQueue->ExecuteCommandLists(assert_cast<UINT>(commandLists.size()), &commandLists[0]);
        fence.SignalQueue(commandQueue.Get(), fenceEvent);
    }

    void ExecuteCommandLists(std::initializer_list<ID3D12CommandList* const> commandLists, FenceEvent& fenceEvent, std::initializer_list<FenceEvent> waitEvents = {})
    {
        ExecuteCommandLists(std::span<ID3D12CommandList* const>(commandLists), fenceEvent, waitEvents);
    }

    void ExecuteCommandListsBlocking(std::span<ID3D12CommandList* const> commandLists, std::initializer_list<FenceEvent> waitEvents = {})
    {
#ifdef MDXR_DEBUG
        for (auto& commandList : commandLists) {
            CHECK(commandList->GetType() == commandListType);
        }
#endif

        FenceEvent fenceEvent;
        ExecuteCommandLists(commandLists, fenceEvent, waitEvents);
        WaitForEventCPU(fenceEvent);
    }

    // This is necessary because Present must happen before signaling the queue.
    HRESULT ExecuteCommandListsAndPresent(
        std::span<ID3D12CommandList* const> commandLists,
        IDXGISwapChain* swapChain,
        UINT syncInterval,
        UINT presentFlags,
        FenceEvent& fenceEvent,
        FenceEvent waitEvent = FenceEvent()
    )
    {
#ifdef MDXR_DEBUG
        for (auto& commandList : commandLists) {
            CHECK(commandList->GetType() == commandListType);
        }
#endif

        HRESULT hr;
        {
            // We must synchronize the calls to the queue, but don't want to keep the lock for
            // the CPU wait.
            std::scoped_lock lock{ mutex };

            waitEvent.sourceFence->WaitQueue(commandQueue.Get(), waitEvent);
            commandQueue->ExecuteCommandLists(assert_cast<UINT>(commandLists.size()), &commandLists[0]);
            hr = swapChain->Present(syncInterval, presentFlags);
            if (!SUCCEEDED(hr)) {
                return hr;
            }
            fence.SignalQueue(commandQueue.Get(), fenceEvent);
        }

        return hr;
    }

    void ExecuteCommandListsBlocking(std::initializer_list<ID3D12CommandList* const> commandLists, std::initializer_list<FenceEvent> waitEvents = {})
    {
        ExecuteCommandListsBlocking(std::span<ID3D12CommandList* const>(commandLists), waitEvents);
    }

    void WaitForEventCPU(FenceEvent& fenceEvent)
    {
        fenceEvent.sourceFence->WaitCPU(fenceEvent);
    }
private:

#if 0
    void AppendCommandList(ID3D12Device* device, ID3D12PipelineState* initialState)
    {
        CommandListEntry entry;
        entry.isRecording = false;
        ASSERT_HRESULT(device->CreateCommandAllocator(
            commandListType,
            IID_PPV_ARGS(&entry.commandAllocator)
        ));
        ASSERT_HRESULT(device->CreateCommandList(
            0,
            commandListType,
            entry.commandAllocator.Get(),
            initialState,
            IID_PPV_ARGS(&entry.commandList)
        ));

        commandLists.push_back(entry);
    }


    struct CommandListEntry {
        bool isRecording;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        ComPtr<ID3D12CommandAllocator> commandAllocator;
    };
    std::vector<CommandListEntry> commandLists;
#endif

    ComPtr<ID3D12CommandQueue> commandQueue;
    IncrementalFence fence;
    D3D12_COMMAND_LIST_TYPE commandListType;
    std::mutex mutex;
};