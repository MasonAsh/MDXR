#include "scene.h"

#include <glm/glm.hpp>

void InitializeCamera(App& app)
{
    app.camera.translation = glm::vec3(0.0f, 0.5f, 3.0f);
    app.camera.pitch = 0.0f;
    app.camera.yaw = glm::pi<float>();
}

void InitializeLights(App& app)
{
    app.LightBuffer.count = 1;

    app.lights[0].lightType = LightType_Directional;
    app.lights[0].color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    app.lights[0].intensity = 1.5f;
    app.lights[0].position = glm::vec3(0.0f);
    app.lights[0].direction = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));
    app.lights[0].range = 5.0f;

    for (int i = 1; i < MaxLightCount; i++) {
        float angle = i * glm::two_pi<float>() / 4;
        float x = cos(angle);
        float z = sin(angle);
        app.lights[i].color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        app.lights[i].intensity = 8.0f;
        app.lights[i].position = glm::vec3(x, 2.0f, z);
        app.lights[i].direction = glm::vec3(0.0f, 0.0f, 0.0f);
        app.lights[i].range = 5.0f;
        app.lights[i].lightType = LightType_Point;
    }
}

void InitializeScene(App& app)
{
    InitializeCamera(app);
    InitializeLights(app);
    //app.models[1].meshes[0]->translation = glm::vec3(0, 0.5f, 0.0f);
}

void StartSceneAssetLoad(App& app)
{
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    const std::string& dataDir = app.dataDir;

    EnqueueGLTF(app, dataDir + "/floor/floor.gltf");
    EnqueueGLTF(app, dataDir + "/metallicsphere.gltf");

    SkyboxImagePaths images;
    images.paths[CubeImage_Front] = dataDir + "/ColorfulStudio/pz.png";
    images.paths[CubeImage_Back] = dataDir + "/ColorfulStudio/nz.png";
    images.paths[CubeImage_Right] = dataDir + "/ColorfulStudio/px.png";
    images.paths[CubeImage_Left] = dataDir + "/ColorfulStudio/nx.png";
    images.paths[CubeImage_Top] = dataDir + "/ColorfulStudio/py.png";
    images.paths[CubeImage_Bottom] = dataDir + "/ColorfulStudio/ny.png";
    // images.paths[CubeImage_Front] = dataDir + "/DebugSky/pz.png";
    // images.paths[CubeImage_Back] = dataDir + "/DebugSky/pz.png";
    // images.paths[CubeImage_Left] = dataDir + "/DebugSky/px.png";
    // images.paths[CubeImage_Right] = dataDir + "/DebugSky/px.png";
    // images.paths[CubeImage_Top] = dataDir + "/DebugSky/py.png";
    // images.paths[CubeImage_Bottom] = dataDir + "/DebugSky/py.png";

    EnqueueSkybox(app, images);
}
