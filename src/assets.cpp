#include "assets.h"

#include "app.h"
#include "d3dutils.h"
#include "uploadbatch.h"

#include <pix3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <fstream>
#include <future>

std::mutex g_assetMutex;
std::mutex g_punctualLightLock;

struct alignas(16) GenerateMipsConstantData
{
    UINT texIdx;
    UINT srcMipLevel;
    UINT numMipLevels;
    UINT srcDimension;
    UINT isSRGB;
    glm::vec2 texelSize;
    float padding[57];
};
static_assert((sizeof(GenerateMipsConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");


struct alignas(16) ComputeSkyboxMapsConstantData
{
    glm::vec2 texelSize;
    UINT faceIndex;
    float padding[61];
};
static_assert((sizeof(ComputeSkyboxMapsConstantData) % 256) == 0, "Constant buffer must be 256-byte aligned");


std::vector<unsigned char> LoadBinaryFile(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    std::vector<unsigned char> data;

    if (!file.good()) {
        return data;
    }

    file.unsetf(std::ios::skipws);

    std::streampos fileSize;
    file.seekg(0, std::ios::end);
    fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    data.reserve(fileSize);

    data.insert(data.begin(), std::istream_iterator<unsigned char>(file), std::istream_iterator<unsigned char>());

    return data;
}


std::optional<tinygltf::Image> LoadImageFromMemory(const unsigned char* bytes, int size)
{
    tinygltf::Image image;

    unsigned char* imageData = stbi_load_from_memory(bytes, size, &image.width, &image.height, nullptr, STBI_rgb_alpha);

    if (!imageData) {
        return {};
    }

    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.component = STBI_rgb_alpha;
    image.as_is = false;

    // Copy image data over
    image.image.assign(imageData, imageData + (image.width * image.height * STBI_rgb_alpha));

    stbi_image_free(imageData);

    return image;
}


std::optional<tinygltf::Image> LoadImageFile(const std::string& imagePath)
{
    auto fileData = LoadBinaryFile(imagePath);

    if (fileData.empty()) {
        DebugLog() << "Failed to load " << imagePath << ": " << stbi_failure_reason() << "\n";
        assert(false);
    }

    return LoadImageFromMemory(fileData.data(), fileData.size());
}


std::optional<HDRImage> LoadHDRImage(const std::string& filePath)
{
    HDRImage result;

    float* data = stbi_loadf(filePath.c_str(), &result.width, &result.height, nullptr, STBI_rgb_alpha);

    if (!data) {
        return {};
    }

    result.data.assign(data, data + (result.width * result.height * STBI_rgb_alpha));

    return result;
}


void CreateModelDescriptors(
    App& app,
    const tinygltf::Model& inputModel,
    Model& outputModel,
    const std::span<ComPtr<ID3D12Resource>> textureResources
)
{
    // Allocate 1 descriptor for the constant buffer and the rest for the textures.
    UINT numConstantBuffers = 0;
    for (const auto& mesh : inputModel.meshes) {
        numConstantBuffers += (UINT)mesh.primitives.size();
    }
    UINT numDescriptors = numConstantBuffers + (UINT)inputModel.textures.size();

    UINT incrementSize = G_IncrementSizes.CbvSrvUav;

    // Create per-primitive constant buffer
    ComPtr<ID3D12Resource> perPrimitiveConstantBuffer;
    {
        outputModel.primitiveDataDescriptors = AllocateDescriptorsUnique(app.descriptorPool, numConstantBuffers, "PerPrimitiveConstantBuffer");
        auto cpuHandle = outputModel.primitiveDataDescriptors.CPUHandle();
        CreateConstantBufferAndViews(
            app.device.Get(),
            perPrimitiveConstantBuffer,
            sizeof(PrimitiveInstanceConstantData),
            numConstantBuffers,
            cpuHandle
        );
    }

    // Create SRVs
    if (textureResources.size() > 0) {
        auto descriptorRef = AllocateDescriptorsUnique(app.descriptorPool, (UINT)textureResources.size(), "MeshTextures");
        auto cpuHandle = descriptorRef.CPUHandle();
        for (const auto& textureResource : textureResources) {
            auto textureDesc = textureResource->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = textureDesc.Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = textureDesc.MipLevels;
            app.device->CreateShaderResourceView(textureResource.Get(), &srvDesc, cpuHandle);
            cpuHandle.Offset(1, incrementSize);
        }
        outputModel.baseTextureDescriptor = std::move(descriptorRef);
    }

    outputModel.perPrimitiveConstantBuffer = perPrimitiveConstantBuffer;
    CD3DX12_RANGE readRange(0, 0);
    ASSERT_HRESULT(perPrimitiveConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&outputModel.perPrimitiveBufferPtr)));
}


void CreateModelMaterials(
    App& app,
    const tinygltf::Model& inputModel,
    Model& outputModel,
    std::vector<SharedPoolItem<Material>>& modelMaterials
)
{
    // Allocate material constant buffers and create views
    int materialCount = (int)inputModel.materials.size();

    if (materialCount == 0) {
        return;
    }

    auto descriptorReference = AllocateDescriptorsUnique(app.descriptorPool, materialCount, "model materials");
    auto constantBufferSlice = app.materialConstantBuffer.Allocate(materialCount);
    //app.materialConstantBuffer.CreateViews(app.device.Get(), constantBufferSlice, descriptorReference.CPUHandle());
    outputModel.baseMaterialDescriptor = descriptorReference.Ref();

    auto baseTextureDescriptor = outputModel.baseTextureDescriptor.Ref();

    for (int i = 0; i < materialCount; i++) {
        auto& inputMaterial = inputModel.materials[i];

        DescriptorRef baseColorTextureDescriptor;
        DescriptorRef normalTextureDescriptor;
        DescriptorRef metalRoughnessTextureDescriptor;

        if (inputMaterial.pbrMetallicRoughness.baseColorTexture.index != -1) {
            auto texture = inputModel.textures[inputMaterial.pbrMetallicRoughness.baseColorTexture.index];
            auto imageIdx = texture.source;
            baseColorTextureDescriptor = baseTextureDescriptor + imageIdx;
        }
        if (inputMaterial.normalTexture.index != -1) {
            auto texture = inputModel.textures[inputMaterial.normalTexture.index];
            auto imageIdx = texture.source;
            normalTextureDescriptor = baseTextureDescriptor + imageIdx;
        }
        if (inputMaterial.pbrMetallicRoughness.metallicRoughnessTexture.index != -1) {
            auto texture = inputModel.textures[inputMaterial.pbrMetallicRoughness.metallicRoughnessTexture.index];
            auto imageIdx = texture.source;
            metalRoughnessTextureDescriptor = baseTextureDescriptor + imageIdx;
        }

        MaterialType materialType = MaterialType_PBR;
        if (inputMaterial.extensions.contains("KHR_materials_unlit")) {
            materialType = MaterialType_Unlit;
        } else if (inputMaterial.alphaMode.size() > 0 && inputMaterial.alphaMode != "OPAQUE") {
            if (inputMaterial.alphaMode == "BLEND") {
                materialType = MaterialType_AlphaBlendPBR;
            } else {
                DebugLog() << "GLTF material " << inputMaterial.name << " has unsupported alpha mode and will be treated as opaque";
                materialType = MaterialType_PBR;
            }
        }

        auto constantBufferSlice = app.materialConstantBuffer.Allocate(1);
        auto descriptor = AllocateDescriptorsUnique(app.descriptorPool, 1, inputMaterial.name.c_str());
        app.materialConstantBuffer.CreateViews(app.device.Get(), constantBufferSlice, descriptor.CPUHandle());

        auto material = app.materials.AllocateShared();
        material->constantData = constantBufferSlice.data.data();
        material->materialType = materialType;
        material->textureDescriptors.baseColor = baseColorTextureDescriptor;
        material->textureDescriptors.normal = normalTextureDescriptor;
        material->textureDescriptors.metalRoughness = metalRoughnessTextureDescriptor;
        material->baseColorFactor.r = static_cast<float>(inputMaterial.pbrMetallicRoughness.baseColorFactor[0]);
        material->baseColorFactor.g = static_cast<float>(inputMaterial.pbrMetallicRoughness.baseColorFactor[1]);
        material->baseColorFactor.b = static_cast<float>(inputMaterial.pbrMetallicRoughness.baseColorFactor[2]);
        material->baseColorFactor.a = static_cast<float>(inputMaterial.pbrMetallicRoughness.baseColorFactor[3]);
        material->metalRoughnessFactor.g = static_cast<float>(inputMaterial.pbrMetallicRoughness.roughnessFactor);
        material->metalRoughnessFactor.b = static_cast<float>(inputMaterial.pbrMetallicRoughness.metallicFactor);
        material->cbvDescriptor = std::move(descriptor);
        material->name = inputMaterial.name;
        material->castsShadow = material->receivesShadow = true;
        material->UpdateConstantData();

        modelMaterials.push_back(SharedPoolItem<Material>(material));
    }
}


std::pair<ComPtr<ID3D12GraphicsCommandList>, ComPtr<ID3D12CommandAllocator>>
EasyCreateGraphicsCommandList(App& app, D3D12_COMMAND_LIST_TYPE commandListType)
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;

    ASSERT_HRESULT(app.device->CreateCommandAllocator(
        commandListType,
        IID_PPV_ARGS(&allocator)
    ));

    ASSERT_HRESULT(app.device->CreateCommandList(
        0,
        commandListType,
        allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList)
    ));

    return std::pair(commandList, allocator);
}


ComPtr<D3D12MA::Allocation> CopyResourceWithDifferentFlags(
    App& app,
    ID3D12Resource* srcResource,
    D3D12_RESOURCE_FLAGS newFlags,
    D3D12_RESOURCE_STATES resourceStates,
    ID3D12GraphicsCommandList* commandList
)
{
    D3D12_RESOURCE_DESC resourceDesc = srcResource->GetDesc();
    resourceDesc.Flags = newFlags;

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<D3D12MA::Allocation> destResource;
    ASSERT_HRESULT(app.mainAllocator->CreateResource(
        &allocDesc,
        &resourceDesc,
        resourceStates,
        nullptr,
        &destResource,
        IID_NULL, nullptr
    ));

    commandList->CopyResource(destResource->GetResource(), srcResource);

    return destResource;
}


// Generates mip maps for a range of textures. The `textures` must have their
// MipLevels already set.
void GenerateMipMaps(App& app, const std::span<ComPtr<ID3D12Resource>>& textures, const std::vector<bool>& imageIsSRGB, FenceEvent& initialUploadEvent)
{
    // This function is a damn mess...

    if (textures.size() == 0) {
        return;
    }

    ScopedPerformanceTracker perf("GenerateMipMaps", PerformancePrecision::Milliseconds);

    UINT descriptorCount = 0;
    UINT numTextures = static_cast<UINT>(textures.size());

    ComPtr<D3D12MA::Allocation> constantBuffer;

    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(GenerateMipsConstantData));
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    ASSERT_HRESULT(
        app.mainAllocator->CreateResource(
            &allocDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            &constantBuffer,
            IID_NULL, nullptr
        )
    );

    auto [commandList, commandAllocator] = EasyCreateGraphicsCommandList(
        app,
        D3D12_COMMAND_LIST_TYPE_COMPUTE
    );

    auto [copyToCommandList, copyCommandAllocator] = EasyCreateGraphicsCommandList(
        app,
        D3D12_COMMAND_LIST_TYPE_COPY
    );

    auto [copyFromCommandList, copyCommandAllocator2] = EasyCreateGraphicsCommandList(
        app,
        D3D12_COMMAND_LIST_TYPE_COPY
    );

    commandList->SetPipelineState(app.MipMapGenerator.PSO->Get());
    commandList->SetComputeRootSignature(app.MipMapGenerator.rootSignature.Get());

    ID3D12DescriptorHeap* ppHeaps[] = { app.descriptorPool.Heap() };
    commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    std::vector<ComPtr<D3D12MA::Allocation>> allocations;
    allocations.reserve(textures.size());

    bool needsCopy = false;
    std::vector<ID3D12Resource*> destResources;
    destResources.resize(textures.size());

    UINT uavCount = 0;
    UINT cbvCount = 0;
    // Dry run to create destination resources and compute constant buffer count
    for (size_t textureIdx = 0; textureIdx < textures.size(); textureIdx++) {
        ID3D12Resource* destResource = textures[textureIdx].Get();
        auto resourceDesc = destResource->GetDesc();
        auto sliceCount = resourceDesc.DepthOrArraySize;

        // IF source does not have UAV flag, we need to create a copy of the resource with the flag.
        if (!(resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
            auto alloc = CopyResourceWithDifferentFlags(
                app,
                textures[textureIdx].Get(),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON,
                copyToCommandList.Get()
            );
            destResource = alloc->GetResource();

            // After mip generation finishes, we will use this command list to copy back the UAVs to the non-UAVS
            copyFromCommandList->CopyResource(textures[textureIdx].Get(), destResource);

            allocations.push_back(alloc);
            needsCopy = true;
        }

        destResources[textureIdx] = destResource;

        for (UINT16 slice = 0; slice < sliceCount; slice++) {
            for (UINT16 srcMip = 0; srcMip < resourceDesc.MipLevels - 1u; ) {
                UINT64 srcWidth = resourceDesc.Width >> srcMip;
                UINT64 srcHeight = resourceDesc.Height >> srcMip;
                UINT64 dstWidth = (UINT)(srcWidth >> 1);
                UINT64 dstHeight = srcHeight >> 1;

                DWORD mipCount;
                _BitScanForward64(&mipCount, (dstWidth == 1 ? dstHeight : dstWidth) | (dstHeight == 1 ? dstWidth : dstHeight));

                mipCount = std::min<DWORD>(4, mipCount + 1);
                mipCount = (srcMip + mipCount) >= resourceDesc.MipLevels ? resourceDesc.MipLevels - srcMip - 1 : mipCount;

                uavCount += mipCount;

                srcMip += assert_cast<UINT16>(mipCount);
                cbvCount++;
            }
        }
    }

    // Create SRVs for the base mip
    auto baseTextureDescriptor = AllocateDescriptorsUnique(app.descriptorPool, numTextures, "MipGenerationSourceSRVs");
    for (UINT i = 0; i < numTextures; i++) {
        auto textureDesc = destResources[i]->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MipLevels = textureDesc.MipLevels;
        srvDesc.Texture2DArray.ArraySize = textureDesc.DepthOrArraySize;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.PlaneSlice = 0;
        app.device->CreateShaderResourceView(destResources[i], &srvDesc, baseTextureDescriptor.CPUHandle(i));
    }

    if (needsCopy) {
        copyToCommandList->Close();
        copyFromCommandList->Close();
        app.copyQueue.ExecuteCommandListsBlocking({ copyToCommandList.Get() }, initialUploadEvent);
    }

    ConstantBufferArena<GenerateMipsConstantData> constantBufferArena;
    constantBufferArena.InitializeWithCapacity(app.mainAllocator.Get(), cbvCount);
    auto constantBuffers = constantBufferArena.Allocate(cbvCount);

    auto cbvs = AllocateDescriptorsUnique(app.descriptorPool, cbvCount, "MipMapGenerator constant buffers");
    auto uavDescriptors = AllocateDescriptorsUnique(app.descriptorPool, uavCount, "MipMapGenerator UAVs");

    constantBufferArena.CreateViews(app.device.Get(), constantBuffers, cbvs.CPUHandle());

    UINT totalUAVs = 0;
    UINT cbvIndex = 0;
    UINT uavIndex = 0;

    for (size_t textureIdx = 0; textureIdx < textures.size(); textureIdx++) {
        ID3D12Resource* destResource = destResources[textureIdx];
        auto resourceDesc = destResource->GetDesc();

        auto sliceCount = resourceDesc.DepthOrArraySize;
        for (UINT16 slice = 0; slice < sliceCount; slice++) {
            for (UINT16 srcMip = 0; srcMip < resourceDesc.MipLevels - 1u; ) {
                UINT64 srcWidth = resourceDesc.Width >> srcMip;
                UINT64 srcHeight = resourceDesc.Height >> srcMip;
                UINT64 dstWidth = (UINT)(srcWidth >> 1);
                UINT64 dstHeight = srcHeight >> 1;

                DWORD mipCount;
                _BitScanForward64(&mipCount, (dstWidth == 1 ? dstHeight : dstWidth) | (dstHeight == 1 ? dstWidth : dstHeight));

                mipCount = std::min<DWORD>(4, mipCount + 1);
                mipCount = (srcMip + mipCount) >= resourceDesc.MipLevels ? resourceDesc.MipLevels - srcMip - 1 : mipCount;

                dstWidth = std::max<UINT64>(dstWidth, 1);
                dstHeight = std::max<UINT64>(dstHeight, 1);

                auto uavs = uavDescriptors.Ref(uavIndex);
                for (UINT mip = 0; mip < mipCount; mip++) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                    uavDesc.Format = resourceDesc.Format;
                    if (resourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                        uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    }
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    uavDesc.Texture2DArray.MipSlice = srcMip + mip + 1;
                    uavDesc.Texture2DArray.FirstArraySlice = slice;
                    uavDesc.Texture2DArray.ArraySize = 1;

                    app.device->CreateUnorderedAccessView(destResource, nullptr, &uavDesc, (uavs + mip).CPUHandle());
                }

                constantBuffers.data[cbvIndex].texIdx = slice;
                constantBuffers.data[cbvIndex].srcMipLevel = srcMip;
                constantBuffers.data[cbvIndex].srcDimension = (srcHeight & 1) << 1 | (srcWidth & 1);
                constantBuffers.data[cbvIndex].isSRGB = imageIsSRGB[textureIdx]; // SRGB does not seem to work right
                constantBuffers.data[cbvIndex].numMipLevels = mipCount;
                constantBuffers.data[cbvIndex].texelSize.x = 1.0f / (float)dstWidth;
                constantBuffers.data[cbvIndex].texelSize.y = 1.0f / (float)dstHeight;
                constantBuffers.data[cbvIndex].texelSize.y = 1.0f / (float)dstHeight;

                UINT constantValues[6] = {
                    uavs.index,
                    uavs.index + 1,
                    uavs.index + 2,
                    uavs.index + 3,
                    cbvs.Index() + cbvIndex,
                    baseTextureDescriptor.Ref((int)textureIdx).index
                };

                commandList->SetComputeRoot32BitConstants(0, _countof(constantValues), constantValues, 0);

                UINT threadsX = static_cast<UINT>(std::ceil((float)dstWidth / 8.0f));
                UINT threadsY = static_cast<UINT>(std::ceil((float)dstHeight / 8.0f));
                commandList->Dispatch(threadsX, threadsY, 1);

                CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(destResource);
                commandList->ResourceBarrier(1, &barrier);

                uavIndex += mipCount;
                cbvIndex++;
                srcMip += assert_cast<UINT16>(mipCount);
            }
        }
    }

    ASSERT_HRESULT(commandList->Close());

    app.computeQueue.ExecuteCommandListsBlocking({ commandList.Get() }, initialUploadEvent);

    if (needsCopy) {
        app.copyQueue.ExecuteCommandListsBlocking({ copyFromCommandList.Get() });
    }
}


// Note: if I truely want to do things as efficiently as possible I will
// Generate mips on demand as images load asynchronously.
void LoadModelTextures(
    App& app,
    Model& outputModel,
    tinygltf::Model& inputModel,
    std::vector<CD3DX12_RESOURCE_BARRIER>& resourceBarriers,
    const std::vector<bool>& imageIsSRGB,
    ID3D12GraphicsCommandList* commandList,
    ID3D12CommandAllocator* commandAllocator,
    FenceEvent& fenceEvent
)
{
    // We generate mips on unordered access view textures, but these textures
    // are slow for rendering, so we have to copy them to normal textures
    // aftwards.
    std::vector<ComPtr<ID3D12Resource>> stagingTexturesForMipMaps;
    stagingTexturesForMipMaps.reserve(inputModel.images.size());

    UploadBatch uploadBatch;
    uploadBatch.Begin(app.mainAllocator.Get(), &app.copyQueue);

    // Upload images to buffers
    for (int i = 0; i < inputModel.images.size(); i++) {
        const auto& gltfImage = inputModel.images[i];
        ComPtr<ID3D12Resource> buffer;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = GetImageResourceDesc(gltfImage, imageIsSRGB[i]);
        auto resourceState = D3D12_RESOURCE_STATE_COMMON;
        ASSERT_HRESULT(
            app.device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                resourceState,
                nullptr,
                IID_PPV_ARGS(&buffer)
            )
        );

        D3D12_SUBRESOURCE_DATA subresourceData;
        subresourceData.pData = gltfImage.image.data();
        subresourceData.RowPitch = gltfImage.width * gltfImage.component;
        subresourceData.SlicePitch = gltfImage.height * subresourceData.RowPitch;
        uploadBatch.AddTexture(buffer.Get(), &subresourceData, 0, 1);

        stagingTexturesForMipMaps.push_back(buffer);
    }

    FenceEvent uploadEvent = uploadBatch.Finish();

    app.copyQueue.WaitForEventCPU(uploadEvent);

    // Now that the images are uploaded these can be free'd
    for (auto& image : inputModel.images) {
        image.image.clear();
        image.image.shrink_to_fit();
    }

    GenerateMipMaps(app, stagingTexturesForMipMaps, imageIsSRGB, uploadEvent);

    D3D12MA::Budget localBudget;
    app.mainAllocator->GetBudget(&localBudget, nullptr);

    // Only use up to 75% remaining memory on these uploads
    size_t maxUploadBytes = localBudget.BudgetBytes / 2;
    size_t pendingUploadBytes = 0;

    // Copy mip map textures to non-UAV textures for the model.
    for (int textureIdx = 0; textureIdx < inputModel.images.size(); textureIdx++) {
        // FIXME: This would be much better with aliased resources, but that will 
        // require switching the model to use the D3D12MA system for placed resources.
        ComPtr<ID3D12Resource> destResource;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto& inputImage = inputModel.images[textureIdx];
        auto resourceDesc = GetImageResourceDesc(inputImage, imageIsSRGB[textureIdx]);

        auto allocInfo = app.device->GetResourceAllocationInfo(0, 1, &resourceDesc);
        if (pendingUploadBytes > 0 && allocInfo.SizeInBytes + pendingUploadBytes > maxUploadBytes) {
            // Flush the upload
            ASSERT_HRESULT(commandList->Close());
            app.copyQueue.ExecuteCommandListsBlocking({ commandList });

            // Clear the previous staging textures to free memory
            for (int stageIdx = 0; stageIdx < textureIdx; stageIdx++) {
                stagingTexturesForMipMaps[stageIdx] = nullptr;
            }

            ASSERT_HRESULT(commandAllocator->Reset());
            ASSERT_HRESULT(
                commandList->Reset(
                    commandAllocator,
                    nullptr
                )
            );

            pendingUploadBytes = 0;
        }

        ASSERT_HRESULT(
            app.device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&destResource)
            )
        );

        pendingUploadBytes += allocInfo.SizeInBytes;

#ifdef MDXR_DEBUG
        {
            std::wstring bufName = convert_to_wstring(
                "Texture#" + std::to_string(textureIdx) + " " + inputImage.name + ":" + inputImage.uri
            );
            destResource->SetName(bufName.c_str());
        }
#endif

        commandList->CopyResource(destResource.Get(), stagingTexturesForMipMaps[textureIdx].Get());
        outputModel.resources.push_back(destResource);

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            outputModel.resources.back().Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        ));
    }

    commandList->Close();
    app.copyQueue.ExecuteCommandLists({ commandList }, fenceEvent);
    for (auto& texture : stagingTexturesForMipMaps) {
        if (texture != nullptr) {
            fenceEvent.TrackObject(texture.Get());
        }
    }
}


// Gets a vector of booleans parallel to inputModel.images to determine which
// images are SRGB. This information is needed to generate the mipmaps properly.
std::vector<bool> DetermineSRGBTextures(const tinygltf::Model& inputModel)
{
    std::vector<bool> imageIsSRGB(inputModel.images.size(), false);

    // The only textures in a GLTF model that are SRGB are the base color textures.
    for (const auto& material : inputModel.materials) {
        auto textureIndex = material.pbrMetallicRoughness.baseColorTexture.index;
        if (textureIndex != -1) {
            auto imageIndex = inputModel.textures[textureIndex].source;
            imageIsSRGB[imageIndex] = true;
        }
    }

    return imageIsSRGB;
}


typedef std::vector<std::thread> ImageLoadContext;


void WaitForModelImages(ImageLoadContext& threads)
{
    for (auto& thread : threads) {
        thread.join();
    }
}


std::vector<ComPtr<ID3D12Resource>> UploadModelBuffers(
    Model& outputModel,
    App& app,
    tinygltf::Model& inputModel,
    ID3D12GraphicsCommandList* copyCommandList,
    ID3D12CommandAllocator* copyCommandAllocator,
    const std::vector<UINT64>& uploadOffsets,
    std::span<ComPtr<ID3D12Resource>>& outGeometryResources,
    std::span<ComPtr<ID3D12Resource>>& outTextureResources,
    FenceEvent& fenceEvent,
    ImageLoadContext& imageLoadContext,
    AssetLoadContext* context
)
{
    context->currentTask = "Uploading model buffers";
    context->overallPercent = 0.15f;

    std::vector<bool> imageIsSRGB = DetermineSRGBTextures(inputModel);

    std::vector<ComPtr<ID3D12Resource>>& resourceBuffers = outputModel.resources;
    resourceBuffers.reserve(inputModel.buffers.size() + inputModel.images.size());

    UploadBatch uploadBatch;
    uploadBatch.Begin(app.mainAllocator.Get(), &app.copyQueue);

    std::vector<CD3DX12_RESOURCE_BARRIER> resourceBarriers;

    // Copy all the gltf buffer data to a dedicated geometry buffer
    for (size_t bufferIdx = 0; bufferIdx < inputModel.buffers.size(); bufferIdx++) {
        const auto& gltfBuffer = inputModel.buffers[bufferIdx];
        ComPtr<ID3D12Resource> geometryBuffer;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(gltfBuffer.data.size());
        auto resourceState = D3D12_RESOURCE_STATE_COMMON;
        ASSERT_HRESULT(
            app.device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                resourceState,
                nullptr,
                IID_PPV_ARGS(&geometryBuffer)
            )
        );

        uploadBatch.AddBuffer(geometryBuffer.Get(), 0, (void*)gltfBuffer.data.data(), gltfBuffer.data.size());

#ifdef MDXR_DEBUG
        {
            std::wstring bufName = convert_to_wstring(context->assetPath + " Buffer#" + std::to_string(bufferIdx));
            geometryBuffer->SetName(bufName.c_str());
        }
#endif

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            geometryBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER
        ));

        resourceBuffers.push_back(geometryBuffer);
    }

    uploadBatch.Finish();

    context->currentTask = "Loading model textures";
    context->overallPercent = 0.30f;

    WaitForModelImages(imageLoadContext);

    LoadModelTextures(
        app,
        outputModel,
        inputModel,
        resourceBarriers,
        imageIsSRGB,
        copyCommandList,
        copyCommandAllocator,
        fenceEvent
    );

    auto endGeometryBuffer = resourceBuffers.begin() + inputModel.buffers.size();
    outGeometryResources = std::span(resourceBuffers.begin(), endGeometryBuffer);
    outTextureResources = std::span(endGeometryBuffer, resourceBuffers.end());

    context->overallPercent = 0.6f;

    return resourceBuffers;
}


glm::mat4 GetNodeTransfomMatrix(const tinygltf::Node& node, glm::vec3& translate, glm::quat& rotation, glm::vec3& scale, bool& hasTRS)
{
    translate = glm::vec3(0.0f);
    rotation = glm::quat_identity<float, glm::defaultp>();
    scale = glm::vec3(1.0f);

    if (node.matrix.size() > 0) {
        hasTRS = false;
        CHECK(node.matrix.size() == 16);
        return glm::make_mat4(node.matrix.data());;
    } else {
        hasTRS = true;
        if (node.translation.size() > 0) {
            auto translationData = node.translation.data();
            translate = glm::make_vec3(translationData);
        }
        if (node.rotation.size() > 0) {
            auto rotationData = node.rotation.data();
            rotation = glm::make_quat(rotationData);
        }
        if (node.scale.size() > 0) {
            auto scaleData = node.scale.data();
            scale = glm::make_vec3(scaleData);
        }

        glm::mat4 modelMatrix(1.0f);
        glm::mat4 T = glm::translate(glm::mat4(1.0f), translate);
        glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
        glm::mat4 R = glm::toMat4(rotation);
        return T * R * S;
    }
}


struct GLTFLightTransform {
    int lightIndex;
    glm::vec3 position;
    glm::quat rotation;
};


void TraverseNode(const tinygltf::Model& model, const tinygltf::Node& node, std::vector<PoolItem<Mesh>>& meshes, std::vector<GLTFLightTransform>& lights, const glm::mat4& accumulator, const glm::vec3& translateAccum, const glm::quat& rotAccum, const glm::vec3& scaleAccum)
{
    glm::vec3 translate;
    glm::quat rotate;
    glm::vec3 scale;
    bool hasTRS;
    glm::mat4 transform = accumulator * GetNodeTransfomMatrix(node, translate, rotate, scale, hasTRS);
    translate = translate + translateAccum;
    rotate = rotAccum * rotate;
    scale = scaleAccum * scale;

    if (node.mesh != -1) {
        meshes[node.mesh]->baseModelTransform = transform;
    } else if (node.extensions.contains("KHR_lights_punctual")) {
        if (hasTRS) {
            GLTFLightTransform transform;
            transform.lightIndex = node.extensions.at("KHR_lights_punctual").Get("light").GetNumberAsInt();
            transform.position = translate;
            transform.rotation = rotate;
            lights.push_back(transform);
        } else {
            // I'm not going to bother with trying to decompose transform matrices for this
            DebugLog() << "Punctual light with matrix transform will be ignored";
        }
    }

    for (const auto& child : node.children) {
        TraverseNode(model, model.nodes[child], meshes, lights, transform, translate, rotate, scale);
    }
}


// Traverse the GLTF scene to get the correct model matrix for each mesh.
void ResolveModelTransforms(
    const tinygltf::Model& model,
    std::vector<PoolItem<Mesh>>& meshes,
    std::vector<GLTFLightTransform>& lightTransforms
)
{
    if (model.scenes.size() == 0) {
        return;
    }

    int scene = model.defaultScene != 0 ? model.defaultScene : 0;
    for (const auto& node : model.scenes[scene].nodes) {
        TraverseNode(model, model.nodes[node], meshes, lightTransforms, glm::mat4(1.0f), glm::vec3(0.0f), glm::quat_identity<float, glm::defaultp>(), glm::vec3(1.0f));
    }
}


void AssignPSOToPrimitive(
    App& app,
    const tinygltf::Model& inputModel,
    const tinygltf::Primitive& inputPrimitive,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout,
    Primitive* primitive,
    bool hasUVs
)
{
    Material* material = primitive->material.get();
    if (inputPrimitive.material != -1) {
        // FIXME: NO! I should not be creating a PSO for every single Primitive
        if (material->materialType == MaterialType_PBR) {
            primitive->PSO = CreateMeshPBRPSO(
                app.psoManager,
                app.device.Get(),
                app.dataDir,
                app.rootSignature.Get(),
                inputLayout
            );
        } else if (material->materialType == MaterialType_AlphaBlendPBR) {
            primitive->PSO = CreateMeshAlphaBlendedPBRPSO(
                app.psoManager,
                app.device.Get(),
                app.dataDir,
                app.rootSignature.Get(),
                inputLayout
            );
        } else if (material->materialType == MaterialType_Unlit) {
            if (hasUVs) {
                primitive->PSO = CreateMeshUnlitTexturedPSO(
                    app.psoManager,
                    app.device.Get(),
                    app.dataDir,
                    app.rootSignature.Get(),
                    inputLayout
                );
            } else {
                primitive->PSO = CreateMeshUnlitPSO(
                    app.psoManager,
                    app.device.Get(),
                    app.dataDir,
                    app.rootSignature.Get(),
                    inputLayout
                );
            }
        } else {
            // Unimplemented MaterialType
            abort();
        }
    } else {
        // Just pray this will work
        primitive->materialIndex = -1;
        primitive->PSO = CreateMeshUnlitPSO(
            app.psoManager,
            app.device.Get(),
            app.dataDir,
            app.rootSignature.Get(),
            inputLayout
        );
    }

    primitive->directionalShadowPSO = CreateDirectionalLightShadowMapPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        inputLayout
    );
}


PoolItem<Primitive> CreateModelPrimitive(
    App& app,
    Model& outputModel,
    const tinygltf::Model& inputModel,
    const tinygltf::Mesh& inputMesh,
    const tinygltf::Primitive& inputPrimitive,
    const std::vector<SharedPoolItem<Material>>& modelMaterials,
    int perPrimitiveDescriptorIdx
)
{
    // Just storing these strings so that we don't have to keep the Model object around.
    static std::array SEMANTIC_NAMES{
        std::string("POSITION"),
        std::string("NORMAL"),
        std::string("TEXCOORD"),
        std::string("TANGENT"),
    };

    // GLTF stores attribute names like "TEXCOORD_0", "TEXCOORD_1", etc.
    // But DirectX expects SemanticName "TEXCOORD" and SemanticIndex 0
    // So parse it out here
    auto ParseAttribToSemantic = [](const std::string& attribName) -> std::pair<std::string, int>
    {
        auto underscorePos = attribName.find('_');
        if (underscorePos == std::string::npos) {
            return std::pair(attribName, 0);
        } else {
            std::string semantic = attribName.substr(0, underscorePos);
            int index = std::stoi(attribName.substr(underscorePos + 1));
            return std::pair(semantic, index);
        }
    };

    const std::vector<ComPtr<ID3D12Resource>>& resourceBuffers = outputModel.resources;

    auto primitive = app.primitivePool.AllocateUnique();

    primitive->perPrimitiveDescriptor = outputModel.primitiveDataDescriptors.Ref(perPrimitiveDescriptorIdx);
    primitive->constantData = &outputModel.perPrimitiveBufferPtr[perPrimitiveDescriptorIdx];
    perPrimitiveDescriptorIdx++;

    std::vector<D3D12_VERTEX_BUFFER_VIEW>& vertexBufferViews = primitive->vertexBufferViews;

    // Track what addresses are mapped to a vertex buffer view.
    // 
    // The key is the address in the buffer of the first vertex,
    // the value is an index into the vertexBufferViews array.
    // 
    // This needs to be done because certain GLTF models are designed in a way that 
    // doesn't allow us to have a one-to-one relationship between gltf buffer views 
    // and d3d buffer views.
    std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT> vertexStartOffsetToBufferView;

    // Build per drawcall data 
    // input layout and vertex buffer views
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
    inputLayout.reserve(inputPrimitive.attributes.size());

    bool hasUVs = false;

    for (const auto& attrib : inputPrimitive.attributes) {
        auto [targetSemantic, semanticIndex] = ParseAttribToSemantic(attrib.first);
        auto semanticName = std::find(SEMANTIC_NAMES.begin(), SEMANTIC_NAMES.end(), targetSemantic);

        if (semanticName == SEMANTIC_NAMES.end()) {
            DebugLog() << "Unsupported semantic in " << inputMesh.name << " " << targetSemantic << "\n";
            continue;
        }

        if (*semanticName == "TEXCOORD") {
            hasUVs = true;
        }

        D3D12_INPUT_ELEMENT_DESC desc;
        int accessorIdx = attrib.second;
        auto& accessor = inputModel.accessors[accessorIdx];
        desc.SemanticName = semanticName->c_str();
        desc.SemanticIndex = semanticIndex;
        desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        desc.InstanceDataStepRate = 0;
        UINT64 byteStride;

        switch (accessor.type) {
        case TINYGLTF_TYPE_VEC2:
            desc.Format = DXGI_FORMAT_R32G32_FLOAT;
            byteStride = 4 * 2;
            break;
        case TINYGLTF_TYPE_VEC3:
            desc.Format = DXGI_FORMAT_R32G32B32_FLOAT;
            byteStride = 4 * 3;
            break;
        case TINYGLTF_TYPE_VEC4:
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            byteStride = 4 * 4;
            break;
        };

        // Extract bounding box
        if (*semanticName == "POSITION") {
            CHECK(accessor.maxValues.size() >= 3);
            double maxX = accessor.maxValues[0];
            double maxY = accessor.maxValues[1];
            double maxZ = accessor.maxValues[2];
            double minX = accessor.minValues[0];
            double minY = accessor.minValues[1];
            double minZ = accessor.minValues[2];
            primitive->localBoundingBox.max = glm::vec3(maxX, maxY, maxZ);
            primitive->localBoundingBox.min = glm::vec3(minX, minY, minZ);
        }

        // Accessors can be linked to the same bufferview, so here we keep
        // track of what input slot is linked to a bufferview.
        int bufferViewIdx = accessor.bufferView;
        auto bufferView = inputModel.bufferViews[bufferViewIdx];

        byteStride = bufferView.byteStride > 0 ? (UINT)bufferView.byteStride : byteStride;

        auto buffer = resourceBuffers[bufferView.buffer];
        UINT64 vertexStartOffset = bufferView.byteOffset + accessor.byteOffset - (accessor.byteOffset % byteStride);
        D3D12_GPU_VIRTUAL_ADDRESS vertexStartAddress = buffer->GetGPUVirtualAddress() + vertexStartOffset;

        desc.AlignedByteOffset = assert_cast<UINT>(accessor.byteOffset - vertexStartOffset + bufferView.byteOffset);
        // No d3d buffer view attached to this range of vertices yet, add one
        if (!vertexStartOffsetToBufferView.contains(vertexStartAddress)) {
            D3D12_VERTEX_BUFFER_VIEW view;
            view.BufferLocation = vertexStartAddress;
            view.SizeInBytes = assert_cast<UINT>(accessor.count * byteStride);
            view.StrideInBytes = assert_cast<UINT>(byteStride);

            if (view.BufferLocation + view.SizeInBytes > buffer->GetGPUVirtualAddress() + buffer->GetDesc().Width) {
                // The Sponza scene seems to have an oddity mesh that goes out of bounds.
                DebugLog() << "NO!!\n";
                DebugLog() << "Mesh " << inputMesh.name << "\n";
                DebugLog() << "Input element desc.AlignedByteOffset: " << desc.AlignedByteOffset << "\n";
                DebugLog() << "START ADDRESS: " << buffer->GetGPUVirtualAddress() << "\n";
                DebugLog() << "END ADDRESS: " << buffer->GetGPUVirtualAddress() + buffer->GetDesc().Width << "\n";
                DEBUG_VAR(byteStride);
                DEBUG_VAR(desc.AlignedByteOffset);
                DEBUG_VAR(accessor.byteOffset);
                DEBUG_VAR(accessor.count);
                DEBUG_VAR(view.BufferLocation);
                DEBUG_VAR(buffer->GetDesc().Width);
                DEBUG_VAR(vertexStartOffset);
                DEBUG_VAR(*semanticName);
                primitive = nullptr;
                return nullptr;
            }

            vertexBufferViews.push_back(view);
            vertexStartOffsetToBufferView[vertexStartAddress] = (UINT)vertexBufferViews.size() - 1;
        }
        desc.InputSlot = vertexStartOffsetToBufferView[vertexStartAddress];

        inputLayout.push_back(desc);

        D3D12_PRIMITIVE_TOPOLOGY& topology = primitive->primitiveTopology;
        switch (inputPrimitive.mode)
        {
        case TINYGLTF_MODE_POINTS:
            topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            break;
        case TINYGLTF_MODE_LINE:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case TINYGLTF_MODE_LINE_LOOP:
            DebugLog() << "Error: line loops are not supported";
            return nullptr;
        case TINYGLTF_MODE_LINE_STRIP:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case TINYGLTF_MODE_TRIANGLES:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        case TINYGLTF_MODE_TRIANGLE_STRIP:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case TINYGLTF_MODE_TRIANGLE_FAN:
            DebugLog() << "Error: triangle fans are not supported";
            return nullptr;
        };

    }

    primitive->instanceCount = 1;

    if (inputPrimitive.material != -1) {
        auto& material = modelMaterials[inputPrimitive.material];
        primitive->material = material;
    }

    AssignPSOToPrimitive(
        app,
        inputModel,
        inputPrimitive,
        inputLayout,
        primitive.get(),
        hasUVs
    );

    D3D12_INDEX_BUFFER_VIEW& ibv = primitive->indexBufferView;
    int accessorIdx = inputPrimitive.indices;
    auto& accessor = inputModel.accessors[accessorIdx];
    int indexBufferViewIdx = accessor.bufferView;
    auto bufferView = inputModel.bufferViews[indexBufferViewIdx];
    ibv.BufferLocation = resourceBuffers[bufferView.buffer]->GetGPUVirtualAddress() + bufferView.byteOffset + accessor.byteOffset;
    ibv.SizeInBytes = (UINT)(bufferView.byteLength - accessor.byteOffset);

    switch (accessor.componentType) {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        ibv.Format = DXGI_FORMAT_R8_UINT;
        DebugLog() << "GLTF mesh uses byte indices which aren't supported " << inputMesh.name;
        abort();
        break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        ibv.Format = DXGI_FORMAT_R16_UINT;
        break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        ibv.Format = DXGI_FORMAT_R32_UINT;
        break;
    };

    primitive->indexCount = (UINT)accessor.count;
    app.Stats.triangleCount += primitive->indexCount;

    return primitive;
}


void AddPunctualLights(App& app, const tinygltf::Model& inputModel, std::vector<GLTFLightTransform> lightTransforms)
{
    std::scoped_lock<std::mutex> lock(g_punctualLightLock);

    for (size_t i = 0; i < inputModel.lights.size(); i++) {
        const auto& inputLight = inputModel.lights[i];
        LightType lightType;
        if (inputLight.type == "directional") {
            lightType = LightType_Directional;
        } else if (inputLight.type == "point") {
            lightType = LightType_Point;
        } else {
            DebugLog() << "Light '" << inputLight.name << "' has unsupported type '" << inputLight.type << "' will be ignored";
            continue;
        }

        glm::vec3 color(1.0f);
        if (inputLight.color.size() >= 3) {
            color.r = inputLight.color[0];
            color.g = inputLight.color[1];
            color.b = inputLight.color[2];
        }

        for (const auto& lightTransform : lightTransforms) {
            if (lightTransform.lightIndex == i) {
                glm::vec3 position = lightTransform.position;
                glm::vec3 direction = lightTransform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);

                app.lights[app.LightBuffer.count].color = color;
                app.lights[app.LightBuffer.count].direction = direction;
                app.lights[app.LightBuffer.count].position = position;
                app.lights[app.LightBuffer.count].range = inputLight.range;
                app.lights[app.LightBuffer.count].intensity = inputLight.intensity;
                app.lights[app.LightBuffer.count].lightType = lightType;
                app.LightBuffer.count++;
            }
        }
    }
}


void FinalizeModel(
    Model& outputModel,
    App& app,
    const tinygltf::Model& inputModel,
    const std::vector<SharedPoolItem<Material>>& modelMaterials
)
{
    const std::vector<ComPtr<ID3D12Resource>>& resourceBuffers = outputModel.resources;

    int perPrimitiveDescriptorIdx = 0;

    for (const auto& inputMesh : inputModel.meshes) {
        outputModel.meshes.emplace_back(std::move(app.meshPool.AllocateUnique()));

        PoolItem<Mesh>& mesh = outputModel.meshes.back();
        mesh->name = inputMesh.name;
        std::vector<PoolItem<Primitive>>& primitives = mesh->primitives;

        for (int primitiveIdx = 0; primitiveIdx < inputMesh.primitives.size(); primitiveIdx++) {
            const auto& inputPrimitive = inputMesh.primitives[primitiveIdx];

            auto primitive = CreateModelPrimitive(
                app,
                outputModel,
                inputModel,
                inputMesh,
                inputPrimitive,
                modelMaterials,
                perPrimitiveDescriptorIdx
            );

            if (primitive != nullptr) {
                mesh->primitives.emplace_back(std::move(primitive));
                perPrimitiveDescriptorIdx++;
            }
        }
    }

    std::vector<GLTFLightTransform> lightTransforms;
    ResolveModelTransforms(inputModel, outputModel.meshes, lightTransforms);
    AddPunctualLights(app, inputModel, lightTransforms);

    for (auto& mesh : outputModel.meshes) {
        mesh->isReadyForRender = true;
    }
}


// For the time being we cannot handle GLTF meshes without normals, tangents and UVs.
bool ValidateGLTFModel(tinygltf::Model& model)
{
    for (auto& mesh : model.meshes) {
        for (auto& primitive : mesh.primitives) {
            auto& attributes = primitive.attributes;
            bool hasNormals = attributes.contains("NORMAL");
            bool hasTangents = attributes.contains("TANGENT");
            bool hasTexcoords = attributes.contains("TEXCOORD") || attributes.contains("TEXCOORD_0");
            if (!hasNormals || !hasTangents || !hasTexcoords) {
                DebugLog() << "Model with mesh " << mesh.name << " is missing required vertex attributes and will default to being unlit\n";
                attributes.erase(std::string("NORMAL"));
                attributes.erase(std::string("TANGENT"));
            }
        }
    }

    return true;
}


bool ValidateSkyboxAssets(const SkyboxAssets& assets)
{
    auto resourceDesc = GetHDRImageDesc(assets.images[0].width, assets.images[0].height);
    for (int i = 1; i < assets.images.size(); i++) {
        if (GetHDRImageDesc(assets.images[0].width, assets.images[0].height) != resourceDesc) {
            DebugLog() << "Error: all skybox images must have the same image format and dimensions\n";
            return false;
        }
    }

    return true;
}


// Renders all the lighting maps for the skybox, including diffuse irradiance,
// prefilter map, and the BRDF lut.
void RenderSkyboxEnvironmentLightMaps(App& app, const SkyboxAssets& assets, FenceEvent& cubemapUploadEvent, AssetLoadContext* context)
{
    // IMPORTANT: If this gets changed, PREFILTER_MAP_MIPCOUNT in common.hlsli must also be changed
    const UINT PrefilterMipCount = 5;

    ScopedPerformanceTracker perf(__func__, PerformancePrecision::Milliseconds);

    context->currentTask = "Rendering diffuse irradiance map";

    if (app.graphicsAnalysis) {
        app.graphicsAnalysis->BeginCapture();
    }

    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ASSERT_HRESULT(
        app.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            IID_PPV_ARGS(&commandAllocator)
        )
    );

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ASSERT_HRESULT(
        app.device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            commandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&commandList)
        )
    );

    // Create a new cubemap matching the skybox's cubemap resource.
    auto cubemapDesc = app.Skybox.cubemap->GetResource()->GetDesc();
    cubemapDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    {
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &cubemapDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                &app.Skybox.irradianceCubeMap,
                IID_NULL, nullptr
            )
        );


        auto prefilterMapDesc = cubemapDesc;
        prefilterMapDesc.MipLevels = PrefilterMipCount;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &prefilterMapDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                &app.Skybox.prefilterMap,
                IID_NULL, nullptr
            )
        );
    }


    auto diffuseIrradianceUAV = AllocateDescriptorsUnique(app.descriptorPool, 1, "Diffuse radiance UAV");
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uavDesc.Texture2DArray.FirstArraySlice = 0;
    uavDesc.Texture2DArray.ArraySize = CubeImage_Count;
    app.device->CreateUnorderedAccessView(app.Skybox.irradianceCubeMap->GetResource(), nullptr, &uavDesc, diffuseIrradianceUAV.CPUHandle());

    // Create UAVs for each mip on the prefilter map
    auto prefilterMapUAVs = AllocateDescriptorsUnique(app.descriptorPool, PrefilterMipCount, "Prefilter map UAVs");
    for (UINT i = 0; i < PrefilterMipCount; i++) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavDesc.Texture2DArray.FirstArraySlice = 0;
        uavDesc.Texture2DArray.MipSlice = i;
        uavDesc.Texture2DArray.ArraySize = CubeImage_Count;
        app.device->CreateUnorderedAccessView(app.Skybox.prefilterMap->GetResource(), nullptr, &uavDesc, prefilterMapUAVs.CPUHandle(i));
    }

    ManagedPSORef PSO = CreateSkyboxComputeLightMapsPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.MipMapGenerator.rootSignature.Get(),
        app.Skybox.inputLayout
    );

    glm::vec2 texelSize = glm::vec2(1.0f / (float)cubemapDesc.Width, 1.0f / (float)cubemapDesc.Height);

    Primitive* primitive = app.Skybox.mesh->primitives[0].get();

    CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(cubemapDesc.Width), static_cast<float>(cubemapDesc.Height));
    CD3DX12_RECT scissorRect(0, 0, static_cast<LONG>(cubemapDesc.Width), static_cast<LONG>(cubemapDesc.Height));

    ID3D12DescriptorHeap* ppHeaps[] = { app.descriptorPool.Heap() };
    commandList->SetDescriptorHeaps(1, ppHeaps);
    commandList->SetComputeRootSignature(app.MipMapGenerator.rootSignature.Get());
    commandList->SetPipelineState(PSO->Get());

    for (UINT i = 0; i < CubeImage_Count; i++)
    {
        context->currentTask = "Diffuse Irradiance Image " + std::to_string(i);

        PIXScopedEvent(commandList.Get(), 0, ("CubeImage#" + std::to_string(i)).c_str());

        float roughness = 1.0f;

        UINT constantValues[6] = {
            app.Skybox.texcubeSRV.Index(),
            diffuseIrradianceUAV.Index(),
            i,
            *reinterpret_cast<UINT*>(&roughness),
            *reinterpret_cast<UINT*>(&texelSize[0]),
            *reinterpret_cast<UINT*>(&texelSize[1])
        };
        commandList->SetComputeRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
        commandList->Dispatch(static_cast<UINT>(cubemapDesc.Width) / 8, static_cast<UINT>(cubemapDesc.Height) / 8, 1);

        UINT mipWidth = static_cast<UINT>(cubemapDesc.Width);
        UINT mipHeight = static_cast<UINT>(cubemapDesc.Height);
        for (UINT mip = 0; mip < PrefilterMipCount; mip++)
        {
            glm::vec2 texelSize = glm::vec2(1.0f / mipWidth, 1.0f / mipHeight);
            float roughness = static_cast<float>(mip) / static_cast<float>(PrefilterMipCount - 1);
            UINT constantValues[6] = {
                app.Skybox.texcubeSRV.Index(),
                prefilterMapUAVs.Index() + mip,
                i,
                *reinterpret_cast<UINT*>(&roughness),
                *reinterpret_cast<UINT*>(&texelSize[0]),
                *reinterpret_cast<UINT*>(&texelSize[1])
            };
            commandList->SetComputeRoot32BitConstants(0, _countof(constantValues), constantValues, 0);
            commandList->Dispatch(mipWidth / 8, mipHeight / 8, 1);

            mipWidth /= 2;
            mipHeight /= 2;
        }
    }

    // FIXME: this barrier would need to be done on the graphics queue.
    // Also need to research if the UAVs should be copied to a new resource without the UAV flag...
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        app.Skybox.irradianceCubeMap->GetResource(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );

    commandList->Close();
    app.computeQueue.ExecuteCommandListsBlocking({ commandList.Get() });

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = cubemapDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;

    app.Skybox.irradianceCubeSRV = AllocateDescriptorsUnique(app.descriptorPool, 1, "Diffuse Irradiance Cubemap SRV");
    app.device->CreateShaderResourceView(
        app.Skybox.irradianceCubeMap->GetResource(),
        &srvDesc,
        app.Skybox.irradianceCubeSRV.CPUHandle()
    );

    app.Skybox.prefilterMapSRV = AllocateDescriptorsUnique(app.descriptorPool, 1, "Prefilter Map SRV");
    srvDesc.TextureCube.MipLevels = PrefilterMipCount;
    app.device->CreateShaderResourceView(
        app.Skybox.prefilterMap->GetResource(),
        &srvDesc,
        app.Skybox.prefilterMapSRV.CPUHandle()
    );

    if (app.graphicsAnalysis) {
        app.graphicsAnalysis->EndCapture();
    }
}


void LoadBRDFLUT(App& app, UploadBatch& uploadBatch)
{
    if (app.Skybox.brdfLUT != nullptr) {
        // LUT is same for all skyboxes, no need to load twice
        return;
    }

    auto image = *LoadImageFile(app.dataDir + "/brdfLUT.png");
    auto resourceDesc = GetImageResourceDesc(image, false);
    resourceDesc.MipLevels = 1;
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ASSERT_HRESULT(
        app.mainAllocator->CreateResource(
            &allocDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            &app.Skybox.brdfLUT,
            IID_NULL, nullptr
        )
    );

    D3D12_SUBRESOURCE_DATA subresourceData{};
    subresourceData.pData = image.image.data();
    subresourceData.RowPitch = image.width * image.component;
    subresourceData.SlicePitch = image.height * subresourceData.RowPitch;

    uploadBatch.AddTexture(
        app.Skybox.brdfLUT->GetResource(),
        &subresourceData,
        0,
        1
    );

    app.Skybox.brdfLUTDescriptor = AllocateDescriptorsUnique(app.descriptorPool, 1, "Skybox BRDF LUT");

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = resourceDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = resourceDesc.MipLevels;

    app.device->CreateShaderResourceView(
        app.Skybox.brdfLUT->GetResource(),
        &srvDesc,
        app.Skybox.brdfLUTDescriptor.CPUHandle()
    );
}


void CreateSkybox(App& app, const SkyboxAssets& asset, AssetLoadContext* context)
{
    if (!ValidateSkyboxAssets(asset)) {
        return;
    }

    context->currentTask = "Uploading cubemap";
    context->overallPercent = 0.15f;

    ComPtr<D3D12MA::Allocation> cubemap;
    ComPtr<D3D12MA::Allocation> vertexBuffer;
    ComPtr<D3D12MA::Allocation> indexBuffer;
    ComPtr<D3D12MA::Allocation> perPrimitiveBuffer;

    auto cubemapDesc = GetHDRImageDesc(asset.images[0].width, asset.images[0].height);
    cubemapDesc.DepthOrArraySize = CubeImage_Count;
    cubemapDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    {
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &cubemapDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &cubemap,
                IID_NULL, nullptr
            )
        );
    }

    {
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(PrimitiveInstanceConstantData));

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                &perPrimitiveBuffer,
                IID_NULL, nullptr
            )
        );
    }

    UniqueDescriptors perPrimitiveCBV = AllocateDescriptorsUnique(app.descriptorPool, 1, "Skybox PerPrimitive CBV");
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = perPrimitiveBuffer->GetResource()->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(PrimitiveInstanceConstantData);
    app.device->CreateConstantBufferView(&cbvDesc, perPrimitiveCBV.CPUHandle());

    UploadBatch uploadBatch;
    uploadBatch.Begin(app.mainAllocator.Get(), &app.copyQueue);

    for (int i = 0; i < CubeImage_Count; i++) {
        D3D12_SUBRESOURCE_DATA subresourceData = {};
        subresourceData.pData = asset.images[i].data.data();
        subresourceData.RowPitch = asset.images[i].width * 4 * sizeof(float);
        subresourceData.SlicePitch = asset.images[i].height * subresourceData.RowPitch;
        uploadBatch.AddTexture(cubemap->GetResource(), &subresourceData, i * cubemapDesc.MipLevels, 1);
    }

    app.Skybox.cubemap = cubemap;

    LoadBRDFLUT(app, uploadBatch);

    {
        UniqueDescriptors texcubeSRV = AllocateDescriptorsUnique(app.descriptorPool, 1, "Skybox SRV");

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = cubemapDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = cubemapDesc.MipLevels;
        app.device->CreateShaderResourceView(
            cubemap->GetResource(),
            &srvDesc,
            texcubeSRV.CPUHandle()
        );

        app.Skybox.texcubeSRV = std::move(texcubeSRV);
    }

    app.Skybox.inputLayout = {
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

    float vertexData[] =
    {
        // front
        -1.0, -1.0,  1.0,
         1.0, -1.0,  1.0,
         1.0,  1.0,  1.0,
        -1.0,  1.0,  1.0,
        // back
        -1.0, -1.0, -1.0,
         1.0, -1.0, -1.0,
         1.0,  1.0, -1.0,
        -1.0,  1.0, -1.0
    };

    unsigned short indices[] =
    {
        // front
        0, 1, 2,
        2, 3, 0,
        // right
        1, 5, 6,
        6, 2, 1,
        // back
        7, 6, 5,
        5, 4, 7,
        // left
        4, 0, 3,
        3, 7, 4,
        // bottom
        4, 5, 1,
        1, 0, 4,
        // top
        3, 2, 6,
        6, 7, 3
    };

    {
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(vertexData));
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &vertexBuffer,
                IID_NULL, nullptr
            )
        );
    }

    {
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(indices));
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        ASSERT_HRESULT(
            app.mainAllocator->CreateResource(
                &allocDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &indexBuffer,
                IID_NULL, nullptr
            )
        );
    }

    uploadBatch.AddBuffer(vertexBuffer->GetResource(), 0, vertexData, sizeof(vertexData));
    uploadBatch.AddBuffer(indexBuffer->GetResource(), 0, reinterpret_cast<void*>(indices), sizeof(indices));

    auto cubemapUpload = uploadBatch.Finish();
    app.copyQueue.WaitForEventCPU(cubemapUpload);

    // Generate mips for the cubemap. This is needed by the prefilter map computations.
    {
        FenceEvent mipsFenceEvent;
        std::vector<bool> imageIsSRGB(1, false);
        ComPtr<ID3D12Resource> resource = cubemap->GetResource();
        std::vector<ComPtr<ID3D12Resource>> resources = { resource };
        GenerateMipMaps(app, resources, imageIsSRGB, cubemapUpload);
    }

    SharedPoolItem<Material> material = app.materials.AllocateShared();
    material->castsShadow = false;
    material->receivesShadow = false;
    material->materialType = MaterialType_Unlit;
    material->name = "Internal Skybox";

    PoolItem<Primitive> primitive = app.primitivePool.AllocateUnique();
    primitive->indexBufferView.BufferLocation = indexBuffer->GetResource()->GetGPUVirtualAddress();
    primitive->indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    primitive->indexBufferView.SizeInBytes = sizeof(indices);
    primitive->material = material;
    primitive->instanceCount = 1;

    D3D12_VERTEX_BUFFER_VIEW vertexView;
    vertexView.BufferLocation = vertexBuffer->GetResource()->GetGPUVirtualAddress();
    vertexView.SizeInBytes = sizeof(vertexData);
    vertexView.StrideInBytes = sizeof(glm::vec3);
    primitive->vertexBufferViews.push_back(vertexView);

    primitive->PSO = CreateSkyboxPSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        app.Skybox.inputLayout
    );

    primitive->perPrimitiveDescriptor = perPrimitiveCBV.Ref();

    primitive->primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    primitive->indexCount = _countof(indices);

    primitive->miscDescriptorParameter = app.Skybox.texcubeSRV.Ref();

    // Exclude skybox from frustum culling.
    primitive->localBoundingBox = AABB{ glm::vec4(-FLT_MAX), glm::vec4(FLT_MAX) };

    perPrimitiveBuffer->GetResource()->Map(0, nullptr, reinterpret_cast<void**>(&primitive->constantData));

    app.Skybox.mesh = app.meshPool.AllocateUnique();
    app.Skybox.mesh->primitives.emplace_back(std::move(primitive));
    app.Skybox.mesh->baseModelTransform = glm::scale(glm::mat4(1.0f), glm::vec3(50.0f));
    app.Skybox.mesh->name = "Skybox";

    app.Skybox.cubemap = cubemap;
    app.Skybox.indexBuffer = indexBuffer;
    app.Skybox.vertexBuffer = vertexBuffer;
    app.Skybox.perPrimitiveConstantBuffer = perPrimitiveBuffer;
    app.Skybox.perPrimitiveCBV = std::move(perPrimitiveCBV);

    app.Skybox.mesh->isReadyForRender = true;

    RenderSkyboxEnvironmentLightMaps(app, asset, cubemapUpload, context);
}


void GLTFImageLoaderThread(
    tinygltf::Image* out
)
{
    auto maybeImage = LoadImageFromMemory(out->image.data(), out->image.size());

    if (maybeImage) {
        *out = *maybeImage;
    } else {
        // Indicate that the image load has failed
        out->image.clear();
        out->width = 0;
        out->height = 0;
    }
}


ImageLoadContext BeginModelImageLoad(tinygltf::Model& model)
{
    std::vector<std::thread> imageLoadThreads;
    imageLoadThreads.reserve(model.images.size());
    for (auto& image : model.images) {
        imageLoadThreads.push_back(std::thread(GLTFImageLoaderThread, &image));
    }

    return imageLoadThreads;
}


bool TinyGLTFImageLoader(
    tinygltf::Image* image,
    const int image_idx,
    std::string* err,
    std::string* warn,
    int req_width,
    int req_height,
    const unsigned char* bytes,
    int size,
    void* user_pointer
)
{
    image->image.assign(bytes, bytes + size);

    image->width = req_width;
    image->height = req_height;
    image->component = 0;

    image->as_is = true;

    return true;
}


void LoadGLTFThread(App& app, const GLTFLoadEntry& loadEntry, AssetLoadContext* context)
{
    const auto& gltfFile = loadEntry.assetPath;

    auto perfName = "Loading " + gltfFile;
    ScopedPerformanceTracker perf(perfName.c_str(), PerformancePrecision::Milliseconds);

    tinygltf::TinyGLTF loader;
    tinygltf::Model gltfModel;
    std::string err;
    std::string warn;

    loader.SetImageLoader(
        TinyGLTFImageLoader,
        nullptr
    );

    context->assetPath = gltfFile;

    context->currentTask = "Loading GLTF file";
    context->overallPercent = 0.0f;

    if (!loader.LoadASCIIFromFile(&gltfModel, &err, &warn, gltfFile)) {
        DebugLog() << "Failed to load GLTF file " << gltfFile << ":";
        DebugLog() << err;
        return;
    }
    DebugLog() << warn;

    if (!ValidateGLTFModel(gltfModel)) {
        context->isFinished = true;
        return;
    }

    auto imageLoadContext = BeginModelImageLoad(gltfModel);

    std::vector<UINT64> uploadOffsets;
    std::span<ComPtr<ID3D12Resource>> geometryBuffers;
    std::span<ComPtr<ID3D12Resource>> textureBuffers;
    Model model;

    auto [copyCommandList, copyCommandAllocator] = EasyCreateGraphicsCommandList(
        app,
        D3D12_COMMAND_LIST_TYPE_COPY
    );

    FenceEvent fenceEvent;

    // Can only call this ONCE before command list executed
    // This will need to be adapted to handle N models.
    auto resourceBuffers = UploadModelBuffers(
        model,
        app,
        gltfModel,
        copyCommandList.Get(),
        copyCommandAllocator.Get(),
        uploadOffsets,
        geometryBuffers,
        textureBuffers,
        fenceEvent,
        imageLoadContext,
        context
    );

    std::vector<SharedPoolItem<Material>> modelMaterials;

    context->currentTask = "Finalizing";
    CreateModelDescriptors(app, gltfModel, model, textureBuffers);
    CreateModelMaterials(app, gltfModel, model, modelMaterials);
    FinalizeModel(model, app, gltfModel, modelMaterials);

    context->overallPercent = 1.0f;

    app.models.push_back(std::move(model));

    app.copyQueue.WaitForEventCPU(fenceEvent);

    context->isFinished = true;
    loadEntry.finishCB(app, app.models.back());
}


void StartAssetThread(App& app)
{
    app.AssetThread.thread = std::thread(AssetLoadThread, std::ref(app));
}


void NotifyAssetThread(App& app)
{
    std::lock_guard<std::mutex> lock(g_assetMutex);
    app.AssetThread.workEvent.notify_one();
}


void EnqueueGLTF(App& app, const std::string& filePath, ModelFinishCallback finishCB)
{
    GLTFLoadEntry loadEntry;
    loadEntry.assetPath = filePath;
    loadEntry.finishCB = finishCB;

    {
        std::lock_guard<std::mutex> lock(g_assetMutex);
        app.AssetThread.gltfLoadEntries.push_front(loadEntry);
    }

    NotifyAssetThread(app);
}


void EnqueueSkybox(App& app, const SkyboxImagePaths& assetPaths)
{
    {
        std::lock_guard<std::mutex> lock(g_assetMutex);
        app.AssetThread.skyboxToLoad = assetPaths;
    }
    NotifyAssetThread(app);
}


void LoadSkyboxThread(App& app, const SkyboxImagePaths& paths, AssetLoadContext* context)
{
    context->assetPath = paths.paths[0];
    context->currentTask = "Loading skybox images";
    context->overallPercent = 0.0f;

    SkyboxAssets assets;

    std::array<std::future<std::optional<HDRImage>>, CubeImage_Count> images;

    for (int i = 0; i < CubeImage_Count; i++) {
        images[i] = std::async(std::launch::async, LoadHDRImage, paths.paths[i]);
    }

    bool fail = false;
    for (int i = 0; i < CubeImage_Count; i++) {
        images[i].wait();
        auto maybeImage = images[i].get();
        if (!maybeImage) {
            DebugLog() << "Failed to load image " << paths.paths[i];
            fail = true;
            break;
        }

        assets.images[i] = *maybeImage;
    }

    if (fail) {
        context->isFinished = true;
        return;
    }

    CreateSkybox(app, assets, context);

    context->isFinished = true;
}


bool AreAssetsPendingLoad(const App& app)
{
    return !app.AssetThread.gltfLoadEntries.empty() || app.AssetThread.skyboxToLoad.has_value();
}


template<class... Args>
void StartLoadThread(App& app, std::vector<std::thread>& loadThreads, Args... args)
{
    // If one of the previous load threads finished, use it
    for (int i = 0; i < app.AssetThread.assetLoadInfo.size(); i++) {
        if (app.AssetThread.assetLoadInfo[i]->isFinished) {
            app.AssetThread.assetLoadInfo[i] = std::unique_ptr<AssetLoadContext>(new AssetLoadContext);
            loadThreads[i].join();
            loadThreads[i] = std::thread(
                args...,
                app.AssetThread.assetLoadInfo[i].get()
            );
            return;
        }
    }

    // otherwise allocate a new thread
    app.AssetThread.assetLoadInfo.emplace_back(new AssetLoadContext);
    AssetLoadContext* context = app.AssetThread.assetLoadInfo.back().get();
    loadThreads.emplace_back(
        args...,
        context
    );
}


void AssetLoadThread(App& app)
{
    std::vector<std::thread> loadThreads;

    while (app.running)
    {
        std::unique_lock<std::mutex> lock(g_assetMutex);
        while (!AreAssetsPendingLoad(app) && app.running) { app.AssetThread.workEvent.wait(lock); };

        // When the program quits the app will notify the asset thread
        if (!app.running) {
            for (auto& thread : loadThreads) {
                thread.join();
            }

            break;
        }

        if (app.AssetThread.skyboxToLoad.has_value()) {
            StartLoadThread(
                app,
                loadThreads,
                // I have absolutely no idea why this God forsaken cast is necessary
                static_cast<void(*)(App&, const SkyboxImagePaths&, AssetLoadContext*)>(LoadSkyboxThread),
                std::ref(app),
                app.AssetThread.skyboxToLoad.value()
            );
        }
        app.AssetThread.skyboxToLoad = {};

        while (!app.AssetThread.gltfLoadEntries.empty()) {
            auto gltfFile = app.AssetThread.gltfLoadEntries.front();
            app.AssetThread.gltfLoadEntries.pop_front();

            StartLoadThread(
                app,
                loadThreads,
                LoadGLTFThread,
                std::ref(app),
                gltfFile
            );
        }

        lock.unlock();
    }
}
