#pragma once

#include <SDL.h>
#include <SDL_syswm.h>
#include <directx/d3dx12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include "mesh.h"
#include "glm/glm.hpp"

using namespace Microsoft::WRL;

const int FrameBufferCount = 2;

class App
{
public:
    App(int argc, char** argv);
    ~App();

    int Run();

private:

    struct ConstantBufferData {
        glm::mat4 MVP;
        float padding[48]; // 256 aligned
    };
    static_assert((sizeof(ConstantBufferData) % 256) == 0, "Constant buffer must be 256-byte aligned");

    void Initialize();
    void InitWindow();
    void InitD3D();
    void CreateUnlitMeshPSO(Primitive& primitive);
    void LoadScene();

    void WaitForPreviousFrame();

    void UpdateConstantBufferData();
    void DrawModel(const Model& model, ID3D12GraphicsCommandList* list);
    void BuildCommandList();
    void Render();

    std::string dataDir;
    std::wstring wDataDir;

    SDL_Window* window;
    HWND hwnd;

    int windowWidth = 640;
    int windowHeight = 480;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> renderTargets[FrameBufferCount];
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    CD3DX12_VIEWPORT viewport;
    CD3DX12_RECT scissorRect;

    ComPtr<ID3D12PipelineState> unlitMeshPSO;

    ComPtr<ID3D12DescriptorHeap> cbvHeap;
    ConstantBufferData constantBufferData;
    ComPtr<ID3D12Resource> constantBuffer;
    UINT8* constantBufferDataPtr = nullptr;

    Model model;

    unsigned int frameIdx;
    unsigned int rtvDescriptorSize = 0;
    unsigned int fenceValue;
    HANDLE fenceEvent;
};
