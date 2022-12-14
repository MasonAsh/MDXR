#pragma once

#include "util.h"
#include "app.h"

#include <stb_image.h>
#include <tiny_gltf.h>

#include <string>

struct App;
struct SkyboxImagePaths;

struct HDRImage
{
    std::vector<float> data;
    int width;
    int height;
};

struct SkyboxAssets
{
    std::array<HDRImage, CubeImage_Count> images;
};

struct AssetBundle
{
    std::vector<tinygltf::Model> models;
    std::optional<SkyboxAssets> skybox;
};

std::vector<unsigned char> LoadBinaryFile(const std::string& filePath);
std::optional<tinygltf::Image> LoadImageFromMemory(const unsigned char* bytes, int size);
std::optional<tinygltf::Image> LoadImageFile(const std::string& imagePath);
std::optional<HDRImage> LoadHDRImage(const std::string& filePath);
void ProcessAssets(App& app, AssetBundle& assets);

void StartAssetThread(App& app);
void NotifyAssetThread(App& app);
void EnqueueGLTF(App& app, const std::string& filePath, ModelFinishCallback finishCB);
void EnqueueSkybox(App& app, const SkyboxImagePaths& assetPaths);

void LoadSkyboxThread(App& app, const SkyboxImagePaths& assets);

void LoadModel(App& app, const tinygltf::Model& inputModel);

void AssetLoadThread(App& app);