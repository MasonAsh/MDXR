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

void LoadModelAsset(AssetBundle& assets, tinygltf::TinyGLTF& loader, const std::string& filePath)
{
    tinygltf::Model model;
    assets.models.push_back(model);
    std::string err;
    std::string warn;
    CHECK(
        loader.LoadASCIIFromFile(
            &assets.models.back(),
            &err,
            &warn,
            filePath
        )
    );
}

std::vector<unsigned char> LoadBinaryFile(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);

    file.unsetf(std::ios::skipws);

    std::streampos fileSize;
    file.seekg(0, std::ios::end);
    fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<unsigned char> data;
    data.reserve(fileSize);

    data.insert(data.begin(), std::istream_iterator<unsigned char>(file), std::istream_iterator<unsigned char>());

    return data;
}

tinygltf::Image LoadImageFile(const std::string& imagePath)
{
    tinygltf::Image image;

    auto fileData = LoadBinaryFile(imagePath);
    unsigned char* imageData = stbi_load_from_memory(fileData.data(), (int)fileData.size(), &image.width, &image.height, nullptr, STBI_rgb_alpha);
    if (!imageData) {
        std::cout << "Failed to load " << imagePath << ": " << stbi_failure_reason() << "\n";
        assert(false);
    }
    image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.component = STBI_rgb_alpha;

    // Copy image data over
    image.image.assign(imageData, imageData + (image.width * image.height * STBI_rgb_alpha));

    stbi_image_free(imageData);

    return image;
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
        auto descriptorRef = app.descriptorArena.AllocateDescriptors(numConstantBuffers, "PerPrimitiveConstantBuffer");
        auto cpuHandle = descriptorRef.CPUHandle();
        CreateConstantBufferAndViews(
            app.device.Get(),
            perPrimitiveConstantBuffer,
            sizeof(PerPrimitiveConstantData),
            numConstantBuffers,
            cpuHandle
        );
        outputModel.primitiveDataDescriptors = descriptorRef;
    }

    // Create SRVs
    {
        auto descriptorRef = app.descriptorArena.AllocateDescriptors((UINT)textureResources.size(), "MeshTextures");
        auto cpuHandle = descriptorRef.CPUHandle();
        //CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(mainDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), (int)inputModel.meshes.size(), incrementSize);
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
        outputModel.baseTextureDescriptor = descriptorRef;
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
    auto descriptorReference = app.descriptorArena.AllocateDescriptors(materialCount, "model materials");
    auto constantBufferSlice = app.materialConstantBuffer.Allocate(materialCount);
    app.materialConstantBuffer.CreateViews(app.device.Get(), constantBufferSlice, descriptorReference.CPUHandle());
    outputModel.baseMaterialDescriptor = descriptorReference;

    auto baseTextureDescriptor = outputModel.baseTextureDescriptor;

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
                std::cout << "GLTF material " << inputMaterial.name << " has unsupported alpha mode and will be treated as opaque\n";
                materialType = MaterialType_PBR;
            }
        }

        auto material = app.materials.AllocateShared();
        // Material material;
        material->constantData = &constantBufferSlice.data[i];
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
        material->cbvDescriptor = descriptorReference + i;
        material->name = inputMaterial.name;
        material->UpdateConstantData();

        modelMaterials.push_back(SharedPoolItem<Material>(material));
    }
}

// Generates mip maps for a range of textures. The `textures` must have their
// MipLevels already set. Textures must also be UAV compatible.
void GenerateMipMaps(App& app, const std::span<ComPtr<ID3D12Resource>>& textures, const std::vector<bool>& textureIsSRGB, FenceEvent& initialUploadEvent)
{
    if (textures.size() == 0) {
        return;
    }

    ScopedPerformanceTracker perf("GenerateMipMaps", PerformancePrecision::Milliseconds);

    UINT descriptorCount = 0;
    UINT numTextures = static_cast<UINT>(textures.size());

    // Create SRVs for the base mip
    auto baseTextureDescriptor = app.descriptorArena.PushDescriptorStack(numTextures);
    descriptorCount += numTextures;
    for (UINT i = 0; i < numTextures; i++) {
        auto textureDesc = textures[i]->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = textureDesc.MipLevels;
        app.device->CreateShaderResourceView(textures[i].Get(), &srvDesc, (baseTextureDescriptor + i).CPUHandle());
    }

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

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ASSERT_HRESULT(
        app.device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            app.computeCommandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&commandList)
        )
    );

    commandList->SetPipelineState(app.MipMapGenerator.PSO->Get());
    commandList->SetComputeRootSignature(app.MipMapGenerator.rootSignature.Get());

    ID3D12DescriptorHeap* ppHeaps[] = { app.descriptorArena.Heap() };
    commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    UINT cbvCount = 0;
    // Dry run to find the total number of constant buffers to allocate
    for (size_t textureIdx = 0; textureIdx < textures.size(); textureIdx++) {
        auto resourceDesc = textures[textureIdx]->GetDesc();
        for (UINT16 srcMip = 0; srcMip < resourceDesc.MipLevels - 1u; ) {
            UINT64 srcWidth = resourceDesc.Width >> srcMip;
            UINT64 srcHeight = resourceDesc.Height >> srcMip;
            UINT64 dstWidth = (UINT)(srcWidth >> 1);
            UINT64 dstHeight = srcHeight >> 1;

            DWORD mipCount;
            _BitScanForward64(&mipCount, (dstWidth == 1 ? dstHeight : dstWidth) | (dstHeight == 1 ? dstWidth : dstHeight));

            mipCount = std::min<DWORD>(4, mipCount + 1);
            mipCount = (srcMip + mipCount) >= resourceDesc.MipLevels ? resourceDesc.MipLevels - srcMip - 1 : mipCount;

            srcMip += assert_cast<UINT16>(mipCount);
            cbvCount++;
        }
    }

    ConstantBufferArena<GenerateMipsConstantData> constantBufferArena;
    constantBufferArena.InitializeWithCapacity(app.mainAllocator.Get(), cbvCount);
    auto constantBuffers = constantBufferArena.Allocate(cbvCount);

    auto cbvs = app.descriptorArena.PushDescriptorStack(cbvCount);
    descriptorCount += cbvCount;

    constantBufferArena.CreateViews(app.device.Get(), constantBuffers, cbvs.CPUHandle());

    UINT totalUAVs = 0;
    UINT cbvIndex = 0;

    for (size_t textureIdx = 0; textureIdx < textures.size(); textureIdx++) {
        auto resourceDesc = textures[textureIdx]->GetDesc();
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

            auto uavs = app.descriptorArena.PushDescriptorStack(mipCount);
            descriptorCount += mipCount;
            for (UINT mip = 0; mip < mipCount; mip++) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                uavDesc.Format = resourceDesc.Format;
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = srcMip + mip + 1;

                app.device->CreateUnorderedAccessView(textures[textureIdx].Get(), nullptr, &uavDesc, (uavs + mip).CPUHandle());
            }

            constantBuffers.data[cbvIndex].srcMipLevel = srcMip;
            constantBuffers.data[cbvIndex].srcDimension = (srcHeight & 1) << 1 | (srcWidth & 1);
            constantBuffers.data[cbvIndex].isSRGB = 0 /*textureIsSRGB[textureIdx]*/; // SRGB does not seem to work right
            constantBuffers.data[cbvIndex].numMipLevels = mipCount;
            constantBuffers.data[cbvIndex].texelSize.x = 1.0f / (float)dstWidth;
            constantBuffers.data[cbvIndex].texelSize.y = 1.0f / (float)dstHeight;

            UINT constantValues[6] = {
                uavs.index,
                uavs.index + 1,
                uavs.index + 2,
                uavs.index + 3,
                cbvs.index + cbvIndex,
                (baseTextureDescriptor + (int)textureIdx).index
            };

            commandList->SetComputeRoot32BitConstants(0, _countof(constantValues), constantValues, 0);

            UINT threadsX = static_cast<UINT>(std::ceil((float)dstWidth / 8.0f));
            UINT threadsY = static_cast<UINT>(std::ceil((float)dstHeight / 8.0f));
            commandList->Dispatch(threadsX, threadsY, 1);

            CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(textures[textureIdx].Get());
            commandList->ResourceBarrier(1, &barrier);

            cbvIndex++;
            srcMip += assert_cast<UINT16>(mipCount);
        }
    }

    commandList->Close();

    ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
    app.copyFence.WaitQueue(app.computeQueue.Get(), initialUploadEvent);
    app.computeQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
    // FIXME: Don't wait here
    app.fence.SignalAndWait(app.computeQueue.Get());

    app.descriptorArena.PopDescriptorStack(descriptorCount);
}

void LoadModelTextures(
    App& app,
    Model& outputModel,
    tinygltf::Model& inputModel,
    std::vector<CD3DX12_RESOURCE_BARRIER>& resourceBarriers,
    const std::vector<bool>& textureIsSRGB,
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
    uploadBatch.Begin(app.mainAllocator.Get(), app.copyCommandQueue.Get(), &app.copyFence);

    // Upload images to buffers
    for (int i = 0; i < inputModel.images.size(); i++) {
        const auto& gltfImage = inputModel.images[i];
        ComPtr<ID3D12Resource> buffer;
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto resourceDesc = GetImageResourceDesc(gltfImage);
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
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

    app.copyFence.WaitCPU(uploadEvent);
    // Now that the images are uploaded these can be free'd
    for (auto& image : inputModel.images) {
        image.image.clear();
        image.image.shrink_to_fit();
    }

    GenerateMipMaps(app, stagingTexturesForMipMaps, textureIsSRGB, uploadEvent);

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
        auto resourceDesc = GetImageResourceDesc(inputModel.images[textureIdx]);

        auto allocInfo = app.device->GetResourceAllocationInfo(0, 1, &resourceDesc);
        if (pendingUploadBytes > 0 && allocInfo.SizeInBytes + pendingUploadBytes > maxUploadBytes) {
            // Flush the upload
            ASSERT_HRESULT(commandList->Close());
            ID3D12CommandList* ppCommandLists[] = { commandList };
            app.copyCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
            app.copyFence.SignalAndWait(app.copyCommandQueue.Get());

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

        commandList->CopyResource(destResource.Get(), stagingTexturesForMipMaps[textureIdx].Get());
        outputModel.buffers.push_back(destResource);

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            outputModel.buffers.back().Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        ));
    }

    commandList->Close();
    ID3D12CommandList* ppCommandLists[] = { commandList };
    app.copyCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
    app.copyFence.SignalQueue(app.copyCommandQueue.Get(), fenceEvent);
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
    std::vector<bool> textureIsSRGB(inputModel.images.size(), false);

    // The only textures in a GLTF model that are SRGB are the base color textures.
    for (const auto& material : inputModel.materials) {
        auto textureIndex = material.pbrMetallicRoughness.baseColorTexture.index;
        if (textureIndex != -1) {
            auto imageIndex = inputModel.textures[textureIndex].source;
            textureIsSRGB[imageIndex] = true;
        }
    }

    return textureIsSRGB;
}

std::vector<ComPtr<ID3D12Resource>> UploadModelBuffers(
    Model& outputModel,
    App& app,
    tinygltf::Model& inputModel,
    ID3D12GraphicsCommandList* commandList,
    ID3D12GraphicsCommandList* copyCommandList,
    ID3D12CommandAllocator* copyCommandAllocator,
    const std::vector<UINT64>& uploadOffsets,
    std::span<ComPtr<ID3D12Resource>>& outGeometryResources,
    std::span<ComPtr<ID3D12Resource>>& outTextureResources,
    FenceEvent& fenceEvent,
    AssetLoadProgress* progress
)
{
    progress->currentTask = "Uploading model buffers";
    progress->overallPercent = 0.15f;

    std::vector<bool> textureIsSRGB = DetermineSRGBTextures(inputModel);

    std::vector<ComPtr<ID3D12Resource>>& resourceBuffers = outputModel.buffers;
    resourceBuffers.reserve(inputModel.buffers.size() + inputModel.images.size());

    UploadBatch uploadBatch;
    uploadBatch.Begin(app.mainAllocator.Get(), app.copyCommandQueue.Get(), &app.copyFence);

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

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            geometryBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER
        ));

        resourceBuffers.push_back(geometryBuffer);
    }

    uploadBatch.Finish();

    progress->currentTask = "Loading model textures";
    progress->overallPercent = 0.30f;

    LoadModelTextures(
        app,
        outputModel,
        inputModel,
        resourceBarriers,
        textureIsSRGB,
        copyCommandList,
        copyCommandAllocator,
        fenceEvent
    );

    auto endGeometryBuffer = resourceBuffers.begin() + inputModel.buffers.size();
    outGeometryResources = std::span(resourceBuffers.begin(), endGeometryBuffer);
    outTextureResources = std::span(endGeometryBuffer, resourceBuffers.end());

    // ASSERT_HRESULT(commandList->Close());
    // ID3D12CommandList* ppCommandLists[] = { commandList };
    // {
    //     std::lock_guard<std::mutex> lock(app.commandQueueMutex);
    //     app.copyFence.WaitQueue(app.commandQueue.Get(), fenceEvent);
    //     app.commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
    //     app.fence.SignalQueue(app.commandQueue.Get(), fenceEvent);
    // }

    progress->overallPercent = 0.6f;

    return resourceBuffers;
}

glm::mat4 GetNodeTransfomMatrix(const tinygltf::Node& node)
{
    if (node.matrix.size() > 0) {
        CHECK(node.matrix.size() == 16);
        return glm::make_mat4(node.matrix.data());;
    } else {
        glm::vec3 translate(0.0f);
        glm::quat rotation = glm::quat_identity<float, glm::defaultp>();
        glm::vec3 scale(1.0f);
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
        return S * R * T;
    }
}

void TraverseNode(const tinygltf::Model& model, const tinygltf::Node& node, std::vector<PoolItem<Mesh>>& meshes, const glm::mat4& accumulator)
{
    glm::mat4 transform = accumulator * GetNodeTransfomMatrix(node);
    if (node.mesh != -1) {
        meshes[node.mesh]->baseModelTransform = transform;
    }
    for (const auto& child : node.children) {
        TraverseNode(model, model.nodes[child], meshes, transform);
    }
}

// Traverse the GLTF scene to get the correct model matrix for each mesh.
void ResolveMeshTransforms(
    const tinygltf::Model& model,
    std::vector<PoolItem<Mesh>>& meshes
)
{
    if (model.scenes.size() == 0) {
        return;
    }

    int scene = model.defaultScene != 0 ? model.defaultScene : 0;
    for (const auto& node : model.scenes[scene].nodes) {
        TraverseNode(model, model.nodes[node], meshes, glm::mat4(1.0f));
    }
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

    const std::vector<ComPtr<ID3D12Resource>>& resourceBuffers = outputModel.buffers;

    auto primitive = app.primitivePool.AllocateUnique();

    primitive->perPrimitiveDescriptor = outputModel.primitiveDataDescriptors + perPrimitiveDescriptorIdx;
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
    std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout = primitive->inputLayout;
    inputLayout.reserve(inputPrimitive.attributes.size());

    for (const auto& attrib : inputPrimitive.attributes) {
        auto [targetSemantic, semanticIndex] = ParseAttribToSemantic(attrib.first);
        auto semanticName = std::find(SEMANTIC_NAMES.begin(), SEMANTIC_NAMES.end(), targetSemantic);

        if (semanticName == SEMANTIC_NAMES.end()) {
            std::cout << "Unsupported semantic in " << inputMesh.name << " " << targetSemantic << "\n";
            continue;
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
                std::cout << "NO!!\n";
                std::cout << "Mesh " << inputMesh.name << "\n";
                std::cout << "Input element desc.AlignedByteOffset: " << desc.AlignedByteOffset << "\n";
                std::cout << "START ADDRESS: " << buffer->GetGPUVirtualAddress() << "\n";
                std::cout << "END ADDRESS: " << buffer->GetGPUVirtualAddress() + buffer->GetDesc().Width << "\n";
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
            std::cout << "Error: line loops are not supported";
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
            std::cout << "Error: triangle fans are not supported";
            return nullptr;
        };

    }

    if (inputPrimitive.material != -1) {
        auto& material = modelMaterials[inputPrimitive.material];
        primitive->material = material;
        // FIXME: NO! I should not be creating a PSO for every single Primitive
        if (material->materialType == MaterialType_PBR) {
            primitive->PSO = CreateMeshPBRPSO(
                app.psoManager,
                app.device.Get(),
                app.dataDir,
                app.rootSignature.Get(),
                primitive->inputLayout
            );
        } else if (material->materialType == MaterialType_AlphaBlendPBR) {
            primitive->PSO = CreateMeshAlphaBlendedPBRPSO(
                app.psoManager,
                app.device.Get(),
                app.dataDir,
                app.rootSignature.Get(),
                primitive->inputLayout
            );
        } else if (material->materialType == MaterialType_Unlit) {
            primitive->PSO = CreateMeshUnlitPSO(
                app.psoManager,
                app.device.Get(),
                app.dataDir,
                app.rootSignature.Get(),
                primitive->inputLayout
            );
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
            primitive->inputLayout
        );
    }

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
        std::cout << "GLTF mesh uses byte indices which aren't supported " << inputMesh.name;
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

void FinalizeModel(
    Model& outputModel,
    App& app,
    const tinygltf::Model& inputModel,
    const std::vector<SharedPoolItem<Material>>& modelMaterials
)
{
    const std::vector<ComPtr<ID3D12Resource>>& resourceBuffers = outputModel.buffers;

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

    ResolveMeshTransforms(inputModel, outputModel.meshes);

    for (auto& mesh : outputModel.meshes) {
        mesh->isReadyForRender = true;
    }
}

// For the time being we cannot handle GLTF meshes without normals, tangents and UVs.
bool ValidateGLTFModel(const tinygltf::Model& model)
{
    for (const auto& mesh : model.meshes)
    {
        for (const auto& primitive : mesh.primitives)
        {
            const auto& attributes = primitive.attributes;
            bool hasNormals = primitive.attributes.contains("NORMAL");
            bool hasTangents = primitive.attributes.contains("TANGENT");
            bool hasTexcoords = primitive.attributes.contains("TEXCOORD") || primitive.attributes.contains("TEXCOORD_0");
            if (!hasNormals || !hasTangents || !hasTexcoords) {
                DebugLog() << "Model with mesh " << mesh.name << " is missing required vertex attributes and will be skipped\n";
                return false;
            }
        }
    }

    return true;
}

bool ValidateSkyboxAssets(const SkyboxAssets& assets)
{
    auto resourceDesc = GetImageResourceDesc(assets.images[0]);
    for (int i = 1; i < assets.images.size(); i++) {
        if (GetImageResourceDesc(assets.images[i]) != resourceDesc) {
            DebugLog() << "Error: all skybox images must have the same image format and dimensions\n";
            return false;
        }
    }

    return true;
}

void RenderSkyboxDiffuseIrradianceMap(App& app, const SkyboxAssets& assets, FenceEvent& cubemapUploadEvent, AssetLoadProgress* progress)
{
    ScopedPerformanceTracker perf(__func__, PerformancePrecision::Milliseconds);

    progress->currentTask = "Rendering diffuse irradiance map";

    if (app.graphicsAnalysis) {
        app.graphicsAnalysis->BeginCapture();
    }

    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ASSERT_HRESULT(
        app.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&commandAllocator)
        )
    );

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ASSERT_HRESULT(
        app.device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            commandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&commandList)
        )
    );
    ASSERT_HRESULT(commandList->Close());

    // Create a new cubemap matching the skybox's cubemap resource.
    auto cubemapDesc = app.Skybox.cubemap->GetResource()->GetDesc();
    cubemapDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    ASSERT_HRESULT(
        app.mainAllocator->CreateResource(
            &allocDesc,
            &cubemapDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr,
            &app.Skybox.irradianceCubeMap,
            IID_NULL, nullptr
        )
    );

    auto rtv = app.rtvDescriptorArena.PushDescriptorStack(CubeImage_Count);

    for (int i = 0; i < CubeImage_Count; i++)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.FirstArraySlice = i;
        rtvDesc.Texture2DArray.ArraySize = 1;
        app.device->CreateRenderTargetView(app.Skybox.irradianceCubeMap->GetResource(), &rtvDesc, rtv.CPUHandle(i));
    }

    ManagedPSORef PSO = CreateSkyboxDiffuseIrradiancePSO(
        app.psoManager,
        app.device.Get(),
        app.dataDir,
        app.rootSignature.Get(),
        app.Skybox.inputLayout
    );

    // glm::mat4 projection = glm::ortho(0.0f, (float)cubemapDesc.Width, 0.0f, (float)cubemapDesc.Height);
    glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1.0f);
    glm::mat4 viewMatrices[] =
    {
        // Left/Right
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f,  0.0f,  0.0f), glm::vec3(0.0f,  -1.0f,  0.0f)),
        // Top/Bottom
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f,  0.0f), glm::vec3(0.0f,  0.0f,  1.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,  1.0f,  0.0f), glm::vec3(0.0f,  0.0f, -1.0f)),
        // Front/Back
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,  0.0f, -1.0f), glm::vec3(0.0f,  -1.0f,  0.0f)),
        glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,  0.0f,  1.0f), glm::vec3(0.0f,  -1.0f,  0.0f)),
    };

    Primitive* primitive = app.Skybox.mesh->primitives[0].get();

    float beginPercent = progress->overallPercent;
    float endPercent = 1.0f;

    // FIXME: These are very intense draw calls and I'm playing a bit of a guessing game
    // with the shader's `sampleDelta` value to avoid TDR. This could probably benefit from tiled
    // rendering.
    for (int i = 0; i < CubeImage_Count; i++)
    {
        progress->currentTask = "Diffuse Irradiance Image " + std::to_string(i);
        progress->overallPercent = beginPercent + (((endPercent - beginPercent) / (float)CubeImage_Count) * (float)i);

        ASSERT_HRESULT(commandList->Reset(commandAllocator.Get(), PSO->Get()));

        PIXBeginEvent(commandList.Get(), 0, ("CubeImage#" + std::to_string(i)).c_str());

        primitive->constantData->MVP = projection * viewMatrices[i];
        primitive->constantData->MV = viewMatrices[i];

        DEBUG_VAR(primitive->constantData->MVP);
        DEBUG_VAR(viewMatrices[i]);

        CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(cubemapDesc.Width), static_cast<float>(cubemapDesc.Height));
        CD3DX12_RECT scissorRect(0, 0, static_cast<LONG>(cubemapDesc.Width), static_cast<LONG>(cubemapDesc.Height));

        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissorRect);

        ID3D12DescriptorHeap* ppHeaps[] = { app.descriptorArena.Heap() };
        commandList->SetDescriptorHeaps(1, ppHeaps);
        commandList->SetGraphicsRootSignature(app.rootSignature.Get());
        commandList->SetPipelineState(PSO->Get());

        UINT constantValues[5] = {
            primitive->perPrimitiveDescriptor.index,
            UINT_MAX,
            UINT_MAX,
            UINT_MAX,
            app.Skybox.texcubeSRV.index,
        };
        commandList->SetGraphicsRoot32BitConstants(0, _countof(constantValues), constantValues, 0);

        auto rtvHandle = rtv.CPUHandle(i);
        commandList->OMSetRenderTargets(1, &rtvHandle, false, nullptr);

        commandList->IASetPrimitiveTopology(primitive->primitiveTopology);
        commandList->IASetVertexBuffers(0, (UINT)primitive->vertexBufferViews.size(), primitive->vertexBufferViews.data());
        commandList->IASetIndexBuffer(&primitive->indexBufferView);
        commandList->DrawIndexedInstanced(primitive->indexCount, 1, 0, 0, 0);

        if (i == CubeImage_Count - 1) {
            // On the last cubemap face we can transition the cubemap to being a shader resource view.
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(app.Skybox.irradianceCubeMap->GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &barrier);
        }

        PIXEndEvent(commandList.Get());

        // FIXME: It would be better to not block here, and create commands for every face at once.
        // However that would require creating constant buffers for each skybox face.
        commandList->Close();
        {
            std::lock_guard<std::mutex> lock(app.commandQueueMutex);
            ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
            app.commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
            app.fence.SignalAndWait(app.commandQueue.Get());
        }
    }

    app.rtvDescriptorArena.PopDescriptorStack(CubeImage_Count);


    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = cubemapDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;

    app.Skybox.irradianceCubeSRV = app.descriptorArena.AllocateDescriptors(1, "Diffuse Irradiance Cubemap SRV");
    app.device->CreateShaderResourceView(
        app.Skybox.irradianceCubeMap->GetResource(),
        &srvDesc,
        app.Skybox.irradianceCubeSRV.CPUHandle()
    );

    if (app.graphicsAnalysis) {
        app.graphicsAnalysis->EndCapture();
    }
}

void CreateSkybox(App& app, const SkyboxAssets& asset, AssetLoadProgress* progress)
{
    if (!ValidateSkyboxAssets(asset)) {
        return;
    }

    progress->currentTask = "Uploading cubemap";
    progress->overallPercent = 0.15f;

    ComPtr<D3D12MA::Allocation> cubemap;
    ComPtr<D3D12MA::Allocation> vertexBuffer;
    ComPtr<D3D12MA::Allocation> indexBuffer;
    ComPtr<D3D12MA::Allocation> perPrimitiveBuffer;

    auto cubemapDesc = GetImageResourceDesc(asset.images[0]);
    cubemapDesc.MipLevels = 1;
    cubemapDesc.DepthOrArraySize = CubeImage_Count;
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
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(PerPrimitiveConstantData));

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

    DescriptorRef perPrimitiveCbv = app.descriptorArena.AllocateDescriptors(1, "Skybox PerPrimitive CBV");
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = perPrimitiveBuffer->GetResource()->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = sizeof(PerPrimitiveConstantData);
    app.device->CreateConstantBufferView(&cbvDesc, perPrimitiveCbv.CPUHandle());

    D3D12_SUBRESOURCE_DATA cubemapSubresourceData[CubeImage_Count] = {};
    for (int i = 0; i < CubeImage_Count; i++) {
        cubemapSubresourceData[i].pData = asset.images[i].image.data();
        cubemapSubresourceData[i].RowPitch = asset.images[i].width * asset.images[i].component;
        cubemapSubresourceData[i].SlicePitch = asset.images[i].height * cubemapSubresourceData[i].RowPitch;
    }

    UploadBatch uploadBatch;
    uploadBatch.Begin(app.mainAllocator.Get(), app.copyCommandQueue.Get(), &app.copyFence);
    uploadBatch.AddTexture(cubemap->GetResource(), cubemapSubresourceData, 0, CubeImage_Count);

    DescriptorRef texcubeSRV = app.descriptorArena.AllocateDescriptors(1, "Skybox SRV");

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = cubemapDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = 1;
    app.device->CreateShaderResourceView(
        cubemap->GetResource(),
        &srvDesc,
        texcubeSRV.CPUHandle()
    );

    app.Skybox.cubemap = cubemap;
    app.Skybox.texcubeSRV = texcubeSRV;

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

    float vertexData[] = {
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
    app.copyFence.WaitCPU(cubemapUpload);

    PoolItem<Primitive> primitive = app.primitivePool.AllocateUnique();
    primitive->indexBufferView.BufferLocation = indexBuffer->GetResource()->GetGPUVirtualAddress();
    primitive->indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    primitive->indexBufferView.SizeInBytes = sizeof(indices);

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

    primitive->perPrimitiveDescriptor = perPrimitiveCbv;

    primitive->primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    primitive->indexCount = _countof(indices);

    primitive->miscDescriptorParameter = texcubeSRV;

    perPrimitiveBuffer->GetResource()->Map(0, nullptr, reinterpret_cast<void**>(&primitive->constantData));

    app.Skybox.mesh = app.meshPool.AllocateUnique();
    app.Skybox.mesh->primitives.emplace_back(std::move(primitive));
    app.Skybox.mesh->baseModelTransform = glm::scale(glm::mat4(1.0f), glm::vec3(50.0f));
    app.Skybox.mesh->name = "Skybox";

    app.Skybox.cubemap = cubemap;
    app.Skybox.indexBuffer = indexBuffer;
    app.Skybox.vertexBuffer = vertexBuffer;
    app.Skybox.perPrimitiveConstantBuffer = perPrimitiveBuffer;

    RenderSkyboxDiffuseIrradianceMap(app, asset, cubemapUpload, progress);

    Sleep(200);

    app.Skybox.mesh->isReadyForRender = true;
}

void LoadGLTFThread(App& app, const std::string& gltfFile, AssetLoadProgress* progress)
{
    tinygltf::TinyGLTF loader;
    tinygltf::Model gltfModel;
    std::string err;
    std::string warn;

    progress->assetName = gltfFile;

    progress->currentTask = "Loading GLTF file";
    progress->overallPercent = 0.0f;

    if (!loader.LoadASCIIFromFile(&gltfModel, &err, &warn, gltfFile)) {
        DebugLog() << "Failed to load GLTF file " << gltfFile << ":\n";
        DebugLog() << err << "\n";
        return;
    }
    std::cout << warn << "\n";

    if (!ValidateGLTFModel(gltfModel)) {
        return;
    }

    std::vector<UINT64> uploadOffsets;

    std::span<ComPtr<ID3D12Resource>> geometryBuffers;
    std::span<ComPtr<ID3D12Resource>> textureBuffers;

    Model model;

    // FIXME: command lists and command allocators should be reused
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ASSERT_HRESULT(app.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&commandAllocator)
    ));

    ComPtr<ID3D12CommandAllocator> copyCommandAllocator;
    ASSERT_HRESULT(app.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COPY,
        IID_PPV_ARGS(&copyCommandAllocator)
    ));

    ComPtr<ID3D12GraphicsCommandList> commandList;
    ASSERT_HRESULT(app.device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList)
    ));

    ComPtr<ID3D12GraphicsCommandList> copyCommandList;
    ASSERT_HRESULT(app.device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_COPY,
        copyCommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&copyCommandList)
    ));

    FenceEvent fenceEvent;

    // Can only call this ONCE before command list executed
    // This will need to be adapted to handle N models.
    auto resourceBuffers = UploadModelBuffers(
        model,
        app,
        gltfModel,
        commandList.Get(),
        copyCommandList.Get(),
        copyCommandAllocator.Get(),
        uploadOffsets,
        geometryBuffers,
        textureBuffers,
        fenceEvent,
        progress
    );

    std::vector<SharedPoolItem<Material>> modelMaterials;

    progress->currentTask = "Finalizing";
    CreateModelDescriptors(app, gltfModel, model, textureBuffers);
    CreateModelMaterials(app, gltfModel, model, modelMaterials);
    FinalizeModel(model, app, gltfModel, modelMaterials);

    progress->overallPercent = 1.0f;

    app.models.push_back(std::move(model));

    app.copyFence.WaitCPU(fenceEvent);

    progress->isFinished = true;
}

std::mutex g_assetMutex;

void StartAssetThread(App& app)
{
    app.AssetThread.thread = std::thread(AssetLoadThread, std::ref(app));
}

void NotifyAssetThread(App& app)
{
    std::lock_guard<std::mutex> lock(g_assetMutex);
    app.AssetThread.workEvent.notify_one();
}

void EnqueueGLTF(App& app, const std::string& filePath)
{
    std::lock_guard<std::mutex> lock(app.AssetThread.mutex);
    app.AssetThread.gltfFilesToLoad.push_front(filePath);
    NotifyAssetThread(app);
}

void EnqueueSkybox(App& app, const SkyboxImagePaths& assetPaths)
{
    std::lock_guard<std::mutex> lock(app.AssetThread.mutex);
    app.AssetThread.skyboxToLoad = assetPaths;
    NotifyAssetThread(app);
}

void LoadSkyboxThread(App& app, const SkyboxImagePaths& paths, AssetLoadProgress* progress)
{
    progress->assetName = paths.paths[0];
    progress->currentTask = "Loading skybox images";

    SkyboxAssets assets;
    assets.images[CubeImage_Front] = LoadImageFile(paths.paths[CubeImage_Front]);
    assets.images[CubeImage_Back] = LoadImageFile(paths.paths[CubeImage_Back]);
    assets.images[CubeImage_Left] = LoadImageFile(paths.paths[CubeImage_Left]);
    assets.images[CubeImage_Right] = LoadImageFile(paths.paths[CubeImage_Right]);
    assets.images[CubeImage_Top] = LoadImageFile(paths.paths[CubeImage_Top]);
    assets.images[CubeImage_Bottom] = LoadImageFile(paths.paths[CubeImage_Bottom]);
    CreateSkybox(app, assets, progress);

    progress->isFinished = true;
}

bool AreAssetsPendingLoad(const App& app)
{
    return !app.AssetThread.gltfFilesToLoad.empty() || app.AssetThread.skyboxToLoad.has_value();
}

template<class... Args>
void StartLoadThread(App& app, std::vector<std::thread>& loadThreads, Args... args)
{
    // If one of the previous load threads finished, use it
    for (int i = 0; i < app.AssetThread.assetLoadInfo.size(); i++) {
        if (app.AssetThread.assetLoadInfo[i]->isFinished) {
            app.AssetThread.assetLoadInfo[i] = std::unique_ptr<AssetLoadProgress>(new AssetLoadProgress);
            loadThreads[i].join();
            loadThreads[i] = std::thread(
                args...,
                app.AssetThread.assetLoadInfo[i].get()
            );
            return;
        }
    }

    // otherwise allocate a new thread
    app.AssetThread.assetLoadInfo.emplace_back(new AssetLoadProgress);
    AssetLoadProgress* progress = app.AssetThread.assetLoadInfo.back().get();
    loadThreads.emplace_back(
        args...,
        progress
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
                static_cast<void(*)(App&, const SkyboxImagePaths&, AssetLoadProgress*)>(LoadSkyboxThread),
                std::ref(app),
                app.AssetThread.skyboxToLoad.value()
            );
        }
        app.AssetThread.skyboxToLoad = {};

        while (!app.AssetThread.gltfFilesToLoad.empty()) {
            auto gltfFile = app.AssetThread.gltfFilesToLoad.front();
            app.AssetThread.gltfFilesToLoad.pop_front();

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
