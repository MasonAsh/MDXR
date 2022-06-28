#include "mdxr.h"
#include "app.h"
#include "util.h"
#include "d3dutils.h"
#include "assets.h"
#include "scene.h"
#include "pool.h"
#include "incrementalfence.h"
#include "uploadbatch.h"
#include "pso.h"
#include "gbuffer.h"
#include "gui.h"
#include "renderer.h"

#include <mutex>
#include <chrono>

#include <tiny_gltf.h>
#include <dxgidebug.h>

IncrementSizes G_IncrementSizes;

using namespace std::chrono;

void InitWindow(App& app)
{
    Uint32 windowFlags = SDL_WINDOW_RESIZABLE;
    if (app.borderlessFullscreen) {
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    SDL_Window* window = SDL_CreateWindow("MDXR",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        app.windowWidth, app.windowHeight,
        windowFlags
    );

    assert(window);

    app.window = window;

    SDL_SysWMinfo wmInfo = {};
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
        std::cout << "Failed to fetch window info from SDL\n";
        std::cout << "SDL_GetError(): " << SDL_GetError() << std::endl;
        abort();
    }
    app.hwnd = wmInfo.info.win.window;

    app.keyState = SDL_GetKeyboardState(nullptr);
}

void CreateDataDirWatchHandle(App& app)
{
    app.shaderWatchHandle = FindFirstChangeNotificationW(app.wDataDir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    assert(app.shaderWatchHandle != INVALID_HANDLE_VALUE);
}

void InitApp(App& app, int argc, char** argv)
{
    app.viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(app.windowWidth), static_cast<float>(app.windowHeight));
    app.scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(app.windowWidth), static_cast<LONG>(app.windowHeight));

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--datadir")) {
            if (i + 1 < argc) {
                app.dataDir = argv[i + 1];
            }
        } else if (!strcmp(argv[i], "--borderless")) {
            app.borderlessFullscreen = true;
        } else if (!strcmp(argv[i], "--gpudebug")) {
            app.gpuDebug = true;
        }
    }

    if (app.dataDir.empty()) {
        std::cout << "Error: no data directory specified\n";
        std::cout << "The program will now exit" << std::endl;
        abort();
    }

    app.wDataDir = convert_to_wstring(app.dataDir);
    CreateDataDirWatchHandle(app);
}

// Updates the camera to behave like a flying camera.
// WASD moves laterally and Q and E lower and raise the camera.
// Only updates when the user is pressing the right mouse button.
glm::mat4 UpdateFlyCamera(App& app, float deltaSeconds)
{
    glm::vec3 cameraMovement(0.0f);

    if (!app.camera.locked) {
        app.camera.targetSpeed += app.mouseState.scrollDelta * 1.0f;
        app.camera.targetSpeed = glm::clamp(app.camera.targetSpeed, app.camera.minSpeed, app.camera.maxSpeed);
        const float RADIANS_PER_PIXEL = glm::radians(0.1f);
        app.camera.yaw -= (float)app.mouseState.xrel * RADIANS_PER_PIXEL;
        app.camera.pitch -= (float)app.mouseState.yrel * RADIANS_PER_PIXEL;
        app.camera.pitch = glm::clamp(app.camera.pitch, -app.camera.maxPitch, app.camera.maxPitch);
    }

    float yaw = app.camera.yaw;
    float pitch = app.camera.pitch;

    glm::vec3 cameraForward;
    cameraForward.z = cos(yaw) * cos(pitch);
    cameraForward.y = sin(pitch);
    cameraForward.x = sin(yaw) * cos(pitch);
    cameraForward = glm::normalize(cameraForward);

    float right = app.keyState[SDL_SCANCODE_D] ? 1.0f : 0.0f;
    float left = app.keyState[SDL_SCANCODE_A] ? -1.0f : 0.0f;
    float forward = app.keyState[SDL_SCANCODE_W] ? 1.0f : 0.0f;
    float backward = app.keyState[SDL_SCANCODE_S] ? -1.0f : 0.0f;
    float up = app.keyState[SDL_SCANCODE_E] ? 1.0f : 0.0f;
    float down = app.keyState[SDL_SCANCODE_Q] ? -1.0f : 0.0f;

    glm::vec3 inputVector = glm::vec3(right + left, up + down, forward + backward);


    glm::vec3 upVector = glm::vec3(0.0f, 1.0f, 0.0f);

    float speed = app.camera.targetSpeed * deltaSeconds;

    if (!app.camera.locked) {
        glm::vec3 vecUp(0.0f, 1.0f, 0.0f);
        cameraMovement += inputVector.z * cameraForward;
        cameraMovement += inputVector.x * glm::normalize(glm::cross(cameraForward, vecUp));
        cameraMovement.y += inputVector.y;
        cameraMovement *= speed;
        app.camera.translation += cameraMovement;
    }

    return glm::lookAt(app.camera.translation, app.camera.translation + cameraForward, glm::vec3(0, 1, 0));
}

void UpdateScene(App& app)
{
    long long currentTick = steady_clock::now().time_since_epoch().count();
    long long ticksSinceStart = currentTick - app.startTick;
    float timeSeconds = (float)ticksSinceStart / (float)1e9;
    long long deltaTicks = currentTick - app.lastFrameTick;
    float deltaSeconds = (float)deltaTicks / (float)1e9;

    glm::mat4 projection = glm::perspective(app.camera.fovY, (float)app.windowWidth / (float)app.windowHeight, 0.1f, 1000.0f);
    glm::mat4 view = UpdateFlyCamera(app, deltaSeconds);

    static std::once_flag onceFlag;
    std::call_once(onceFlag, [view] {DEBUG_VAR(view)});

    if (app.Skybox.mesh) {
        app.Skybox.mesh->translation = app.camera.translation;
    }

    UpdateRenderData(app, projection, view);
}

void ReloadIfShaderChanged(App& app)
{
    auto status = WaitForSingleObject(app.shaderWatchHandle, 0);
    if (status == WAIT_OBJECT_0) {
        std::cout << "Data directory changed. Reloading shaders.\n";
        app.psoManager.Reload(app.device.Get());
        CreateDataDirWatchHandle(app);
    }
}

void ToggleBorderlessWindow(App& app)
{
    int flags = !app.borderlessFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    SDL_SetWindowFullscreen(app.window, flags);

    int width, height;
    SDL_GetWindowSize(app.window, &width, &height);
    HandleResize(app, width, height);


    app.borderlessFullscreen = !app.borderlessFullscreen;
}

int RunApp(int argc, char** argv)
{
    App app;

    InitApp(app, argc, argv);
    InitWindow(app);
    InitD3D(app);
    InitImGui(app);

    {
        AssetBundle assets = LoadAssets(app.dataDir);
        ProcessAssets(app, assets);
    }

    InitializeScene(app);

    SDL_Event e;

    app.startTick = steady_clock::now().time_since_epoch().count();
    app.lastFrameTick = app.startTick;

    int mouseX, mouseY;
    int buttonState = SDL_GetMouseState(&mouseX, &mouseY);

    app.running = true;
    while (app.running) {
        long long frameTick = steady_clock::now().time_since_epoch().count();
        app.mouseState.xrel = 0;
        app.mouseState.yrel = 0;
        app.mouseState.scrollDelta = 0;
        while (SDL_PollEvent(&e) > 0) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                app.running = false;
            } else if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                    int newWidth = e.window.data1;
                    int newHeight = e.window.data2;
                    HandleResize(app, newWidth, newHeight);
                }
            } else if (e.type == SDL_MOUSEMOTION) {
                app.mouseState.xrel += e.motion.xrel;
                app.mouseState.yrel += e.motion.yrel;
            } else if (e.type == SDL_MOUSEWHEEL) {
                app.mouseState.scrollDelta = e.wheel.preciseY;
            } else if (e.type == SDL_KEYUP) {
                if (e.key.keysym.sym == SDLK_F5) {
                    app.psoManager.Reload(app.device.Get());
                } else if (e.key.keysym.sym == SDLK_RETURN && e.key.keysym.mod & KMOD_ALT) {
                    ToggleBorderlessWindow(app);
                }
            }
        }

        ReloadIfShaderChanged(app);

        buttonState = SDL_GetMouseState(&mouseX, &mouseY);

        app.camera.locked = (buttonState & SDL_BUTTON_RMASK) == 0;
        if (!app.camera.locked) {
            if (!SDL_GetRelativeMouseMode()) {
                SDL_SetRelativeMouseMode(SDL_TRUE);
                SDL_SetWindowGrab(app.window, SDL_TRUE);
            }
        } else {
            if (SDL_GetRelativeMouseMode()) {
                SDL_SetRelativeMouseMode(SDL_FALSE);
                SDL_SetWindowGrab(app.window, SDL_FALSE);
            }
        }

        BeginGUI(app);
        UpdateScene(app);
        ImGui::Render();

        RenderFrame(app);

        long long endTick = steady_clock::now().time_since_epoch().count();
        app.Stats.lastFrameTimeNS = endTick - frameTick;

        app.lastFrameTick = frameTick;
    }

    WaitForPreviousFrame(app);

    CleanImGui();
    SDL_DestroyWindow(app.window);

    return 0;
}

int RunMDXR(int argc, char** argv)
{
    int status = RunApp(argc, argv);

#if defined(_DEBUG)
    {
        ComPtr<IDXGIDebug1> dxgiDebug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
        {
            dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        }
    }
#endif

    return status;
}
