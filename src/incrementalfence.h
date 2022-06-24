#pragma once

#include <directx/d3dx12.h>

using namespace Microsoft::WRL;

struct IncrementalFence {
    ComPtr<ID3D12Fence> fence;
    UINT64 targetFenceValue;
    UINT64 nextFenceValue;
    HANDLE event;

    void Initialize(ID3D12Device* device)
    {
        targetFenceValue = 0;
        ASSERT_HRESULT(
            device->CreateFence(
                targetFenceValue,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&fence)
            )
        );
        nextFenceValue = targetFenceValue + 1;

        event = CreateEvent(nullptr, false, false, nullptr);
        if (event == nullptr) {
            ASSERT_HRESULT(
                HRESULT_FROM_WIN32(
                    GetLastError()
                )
            );
        }
    }

    void Signal(ID3D12CommandQueue* commandQueue)
    {
        CHECK(IsFinished());
        // Signal and increment the fence value.
        targetFenceValue = nextFenceValue;
        ASSERT_HRESULT(
            commandQueue->Signal(fence.Get(), targetFenceValue)
        );
        nextFenceValue++;
    }

    void Wait(ID3D12CommandQueue* commandQueue)
    {
        // Wait until the previous frame is finished.
        if (fence->GetCompletedValue() < targetFenceValue)
        {
            ASSERT_HRESULT(
                fence->SetEventOnCompletion(targetFenceValue, event)
            );

            WaitForSingleObject(event, INFINITE);
        }
    }

    bool IsFinished()
    {
        return fence->GetCompletedValue() >= targetFenceValue;
    }

    void SignalAndWait(ID3D12CommandQueue* commandQueue)
    {
        Signal(commandQueue);
        Wait(commandQueue);
    }
};