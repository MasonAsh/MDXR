#pragma once

#include "util.h"
#include "app.h"

#include <stb_image.h>
#include <tiny_gltf.h>

#include <string>

struct App;
struct SkyboxImagePaths;

struct SkyboxAssets
{
    std::array<tinygltf::Image, CubeImage_Count> images;
};

struct AssetBundle
{
    std::vector<tinygltf::Model> models;
    std::optional<SkyboxAssets> skybox;
};

void LoadModelAsset(AssetBundle& assets, tinygltf::TinyGLTF& loader, const std::string& filePath);
std::vector<unsigned char> LoadBinaryFile(const std::string& filePath);
tinygltf::Image LoadImageFile(const std::string& imagePath);
void ProcessAssets(App& app, AssetBundle& assets);

void StartAssetThread(App& app);
void NotifyAssetThread(App& app);
void EnqueueGLTF(App& app, const std::string& filePath);
void EnqueueSkybox(App& app, const SkyboxImagePaths& assetPaths);

void LoadSkyboxThread(App& app, const SkyboxImagePaths& assets);

void LoadModel(App& app, const tinygltf::Model& inputModel);

void AssetLoadThread(App& app);