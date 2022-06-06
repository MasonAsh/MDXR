#pragma once

#include <string>
#include <vector>
#include <future>
#include <directx/d3dx12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <glm/glm.hpp>

using namespace DirectX;
using namespace Microsoft::WRL;

struct Primitive {
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    std::vector<D3D12_VERTEX_BUFFER_VIEW> vertexBufferViews;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    D3D12_PRIMITIVE_TOPOLOGY primitiveTopology;
    ComPtr<ID3D12PipelineState> PSO;
    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle;
    int indexCount;
};

struct Mesh {
    std::vector<Primitive> primitives;

    // TODO:
    // XMFLOAT3 translation;
};

struct Model {
    std::vector<ComPtr<ID3D12Resource>> buffers;
    std::vector<Mesh> meshes;

    glm::mat4 transform;
};

Model LoadGLTF(ID3D12Device* device, const std::string& path);