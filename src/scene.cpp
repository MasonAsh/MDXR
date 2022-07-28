#include "scene.h"

#include <glm/glm.hpp>

void InitializeCamera(App& app)
{
    app.camera.translation = glm::vec3(0.0f, 2.0f, 0.0f);
    app.camera.pitch = 0.0f;
    app.camera.yaw = -glm::pi<float>() / 2.0f;
}

void InitializeLights(App& app)
{
    app.LightBuffer.count = 0;

    app.lights[0].lightType = LightType_Directional;
    app.lights[0].color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    app.lights[0].intensity = 1.5f;
    app.lights[0].position = glm::vec3(0.0f);
    app.lights[0].direction = glm::normalize(glm::vec3(1.0f, -0.4f, -1.0f));
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

    app.PostProcessPass.exposure = 0.1f;
}

void StartSceneAssetLoad(App& app)
{
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    const std::string& dataDir = app.dataDir;

    // EnqueueGLTF(app, dataDir + "/floor/floor.gltf");
    // EnqueueGLTF(app, dataDir + "/LightTest/LightTest.gltf");
    // EnqueueGLTF(app, dataDir + "/metallicsphere.gltf");
    // EnqueueGLTF(app, dataDir + "/FlightHelmet/FlightHelmet.gltf");
    EnqueueGLTF(app, "C:\\Users\\mason\\dev\\glTF-Sample-Models\\Main\\tangified\\sponza_tangents.gltf");
    EnqueueGLTF(app, "C:/Users/mason/dev/glTF-Sample-Models/Main/PKG_A_Curtains/NewSponza_Curtains_glTF_with_tangents.gltf");
    EnqueueGLTF(app, "C:/Users/mason/dev/glTF-Sample-Models/Main/PKG_D_Candles/NewSponza_100sOfCandles_glTF_OmniLights_with_tangents.gltf");

    SkyboxImagePaths images;
    const std::string skyboxDir = "/Forest/";
    const std::string extension = ".hdr";
    // const std::string skyboxDir = "/DebugSky/";
    // const std::string extension = ".png";
    images.paths[CubeImage_Front] = dataDir + skyboxDir + "pz" + extension;
    images.paths[CubeImage_Back] = dataDir + skyboxDir + "nz" + extension;
    images.paths[CubeImage_Right] = dataDir + skyboxDir + "px" + extension;
    images.paths[CubeImage_Left] = dataDir + skyboxDir + "nx" + extension;
    images.paths[CubeImage_Top] = dataDir + skyboxDir + "py" + extension;
    images.paths[CubeImage_Bottom] = dataDir + skyboxDir + "ny" + extension;

    EnqueueSkybox(app, images);
}
