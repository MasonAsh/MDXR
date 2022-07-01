#pragma once

#include <directx/d3dx12.h>

using namespace Microsoft::WRL;

class FenceEvent
{
public:
    friend class IncrementalFence;

    FenceEvent()
        : fenceValue(UINT64_MAX)
    {
    }

    FenceEvent(UINT64 fenceValue)
        : fenceValue(fenceValue)
    {
    }

    void TrackObject(IUnknown* resource)
    {
        trackedObjects.push_back(resource);
    }

    UINT64 fenceValue;
    std::vector<ComPtr<IUnknown>> trackedObjects;

#if MDXR_DEBUG
    // Used by IncrementalFence to assert that waits are done on the
    // same fence that created the event
    IncrementalFence* sourceFence;
#endif
};

class IncrementalFence
{
public:
    void Initialize(ID3D12Device* device)
    {
        ASSERT_HRESULT(
            device->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&fence)
            )
        );
        nextFenceValue = 1;
    }

    void SignalQueue(ID3D12CommandQueue* commandQueue, FenceEvent& event)
    {
        // Signal and increment the fence value.
        UINT64 targetFenceValue = nextFenceValue;
        ASSERT_HRESULT(
            commandQueue->Signal(fence.Get(), targetFenceValue)
        );
        nextFenceValue++;

        event.fenceValue = targetFenceValue;
#if MDXR_DEBUG
        event.sourceFence = this;
#endif
    }

    void WaitQueue(ID3D12CommandQueue* commandQueue, FenceEvent& event)
    {
#if MDXR_DEBUG
        assert(event.sourceFence == this);
#endif

        if (event.fenceValue == UINT64_MAX) {
            return;
        }

        if (fence->GetCompletedValue() >= event.fenceValue) {
            return;
        }

        commandQueue->Wait(fence.Get(), event.fenceValue);
    }

    void WaitCPU(FenceEvent& event)
    {
#if MDXR_DEBUG
        assert(event.sourceFence == this);
#endif
        // Wait until the previous frame is finished.
        if (fence->GetCompletedValue() < event.fenceValue)
        {
            // Not a fan of creating an event here, but probably not a big deal
            HANDLE cpuWaitEvent = CreateEvent(nullptr, false, false, nullptr);
            if (cpuWaitEvent == nullptr) {
                ASSERT_HRESULT(
                    HRESULT_FROM_WIN32(
                        GetLastError()
                    )
                );
            }

            ASSERT_HRESULT(
                fence->SetEventOnCompletion(event.fenceValue, cpuWaitEvent)
            );

            WaitForSingleObject(cpuWaitEvent, INFINITE);

            CloseHandle(cpuWaitEvent);
        }
    }

    bool IsFinished()
    {
        return fence->GetCompletedValue() >= targetFenceValue;
    }

    void SignalAndWait(ID3D12CommandQueue* commandQueue)
    {
        FenceEvent fenceEvent;
        SignalQueue(commandQueue, fenceEvent);
        WaitCPU(fenceEvent);
    }

private:
    ComPtr<ID3D12Fence> fence;
    UINT64 targetFenceValue;
    UINT64 nextFenceValue;
    HANDLE cpuWaitEvent;
};