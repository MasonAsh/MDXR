#pragma once

#include "util.h"

#include <stb_image.h>
#include <tiny_gltf.h>

#include <string>

struct App;

enum CubeImageIndex
{
    CubeImage_Right,
    CubeImage_Left,
    CubeImage_Top,
    CubeImage_Bottom,
    CubeImage_Front,
    CubeImage_Back,
    CubeImage_Count, // The amount of faces in a cube may fluctuate in the future
};

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
AssetBundle LoadAssets(const std::string& dataDir);
void ProcessAssets(App& app, AssetBundle& assets);
