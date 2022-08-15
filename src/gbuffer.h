#pragma once

#include <directx/d3dx12.h>

enum GBufferTarget {
    GBuffer_Radiance,
    GBuffer_BaseColor,
    GBuffer_Normal,
    GBuffer_MetalRoughness,
    GBuffer_Depth,
    GBuffer_Count,
};

const int GBuffer_RTVCount = GBuffer_Depth;

inline D3D12_RESOURCE_DESC GBufferResourceDesc(GBufferTarget target, int windowWidth, int windowHeight)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.MipLevels = 1;
    desc.Width = windowWidth;
    desc.Height = windowHeight;
    desc.DepthOrArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // https://www.3dgep.com/forward-plus/#:~:text=G%2Dbuffer%20pass.-,Layout%20Summary,G%2Dbuffer%20layout%20looks%20similar%20to%20the%20table%20shown%20below.,-R
    switch (target)
    {
    case GBuffer_Radiance:
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        break;
    case GBuffer_BaseColor:
    case GBuffer_MetalRoughness:
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case GBuffer_Normal:
        // 32 bit textures can have a significant performance impact in scenes with lots of lights.
        //desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        break;
    case GBuffer_Depth:
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        break;
    };

    return desc;
}
