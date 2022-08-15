#include "internalmeshes.h"

const static std::array<D3D12_INPUT_ELEMENT_DESC, 1> PositionOnlyInputLayout = {
    {
        "POSITION",                                 // SemanticName
        0,                                          // SemanticIndex
        DXGI_FORMAT_R32G32B32_FLOAT,                // Format
        0,                                          // InputSlot
        D3D12_APPEND_ALIGNED_ELEMENT,               // AlignedByteOffset
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, // InputSlotClass
        0                                           // InstanceDataStepRate
    },
};
