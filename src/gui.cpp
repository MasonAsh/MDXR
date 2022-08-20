#include "gui.h"

#include "util.h"
#include "app.h"
#include "assets.h"
#include "scene.h"

#include <directx/d3dx12.h>
#include <tinyfiledialogs.h>

void DebugTextureGUI(App& app, ID3D12Resource* resource, D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc)
{
    app.ImGui.debugSRV = AllocateDescriptorsUnique(app.ImGui.srvHeap, 1, "ImGUI Debug SRV");

    app.device->CreateShaderResourceView(
        resource,
        srvDesc,
        app.ImGui.debugSRV.CPUHandle()
    );
}

void InitImGui(App& app)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 10;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        app.ImGui.srvHeap.Initialize(app.device.Get(), desc, "ImGUI DescriptorPool");
    }

    CHECK(
        ImGui_ImplSDL2_InitForD3D(app.window)
    );

    app.ImGui.fontSRV = AllocateDescriptorsUnique(app.ImGui.srvHeap, 1, "ImGUI Fonts SRV");

    CHECK(
        ImGui_ImplDX12_Init(
            app.device.Get(),
            FrameBufferCount,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            app.ImGui.srvHeap.Heap(),
            app.ImGui.fontSRV.CPUHandle(),
            app.ImGui.fontSRV.GPUHandle()
        )
    );
}

void CleanImGui()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void DrawMeshEditor(App& app)
{
    static int selectedMeshIdx = -1;
    if (ImGui::CollapsingHeader("Mesh Editor")) {
        Mesh* selectedMesh = nullptr;

        if (ImGui::BeginListBox("Meshes")) {
            int meshIdx = 0;
            for (auto& model : app.models) {
                for (auto& mesh : model.meshes) {
                    std::string label = mesh->name;
                    bool isSelected = meshIdx == selectedMeshIdx;

                    if (isSelected) {
                        selectedMesh = mesh.get();
                    }

                    ImGui::PushID(mesh.get());
                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        selectedMeshIdx = meshIdx;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();

                    meshIdx++;
                }
            }
            ImGui::EndListBox();
        }

        ImGui::Separator();

        if (selectedMesh != nullptr) {
            glm::vec3 eulerDegrees = glm::degrees(selectedMesh->euler);

            ImGui::PushID("Mesh");

            ImGui::DragFloat3("Position", (float*)&selectedMesh->translation, 0.1f);
            ImGui::DragFloat3("Euler", (float*)&eulerDegrees, 0.1f);
            ImGui::DragFloat3("Scale", (float*)&selectedMesh->scale, 0.1f);

            selectedMesh->euler = glm::radians(eulerDegrees);

            ImGui::Separator();
            ImGui::Text("Culled primitives:");
            for (const auto& prim : selectedMesh->primitives) {
                ImGui::Text(prim->cull ? "True" : "False");
            }

            ImGui::PopID();
        }
    }
}

void DrawMaterialEditor(App& app)
{
    static int selectedMaterialIdx = -1;

    if (ImGui::CollapsingHeader("Material Editor")) {
        Material* selectedMaterial = nullptr;

        if (ImGui::BeginListBox("Materials")) {
            int materialIdx = 0;
            auto materialIter = app.materials.Begin();

            while (materialIter) {
                bool isSelected = materialIdx == selectedMaterialIdx;

                if (isSelected) {
                    selectedMaterial = materialIter.item;
                }

                const char* materialName = materialIter->name.c_str();
                if (materialIter->name.empty()) {
                    materialName = "UNNAMED";
                }

                ImGui::PushID(materialIter.item);
                if (ImGui::Selectable(materialName, isSelected)) {
                    selectedMaterialIdx = materialIdx;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();

                materialIdx++;
                materialIter = app.materials.Next(materialIter);
            }
            ImGui::EndListBox();
        }

        ImGui::Separator();

        if (selectedMaterial != nullptr) {
            bool materialDirty = false;
            if (ImGui::ColorEdit4("Base Color Factor", &selectedMaterial->baseColorFactor[0])) {
                materialDirty = true;
            }

            if (ImGui::SliderFloat("Roughness", &selectedMaterial->metalRoughnessFactor.y, 0.0f, 1.0f)) {
                materialDirty = true;
            }

            if (ImGui::SliderFloat("Metallic", &selectedMaterial->metalRoughnessFactor.z, 0.0f, 1.0f)) {
                materialDirty = true;
            }

            if (materialDirty) {
                selectedMaterial->UpdateConstantData();
            }
        }
    }
}

void DrawLightEditor(App& app)
{
    static int selectedLightIdx = 0;

    if (ImGui::CollapsingHeader("Lights")) {
        // const char* const* pLabels = (const char* const*)labels;
        if (ImGui::BeginListBox("Lights")) {
            for (UINT i = 0; i < app.LightBuffer.count; i++) {
                std::string label = "Light #" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), i == selectedLightIdx))
                {
                    selectedLightIdx = i;
                    break;
                }
            }
            ImGui::EndListBox();
        }

        if (ImGui::Button("New light")) {
            app.LightBuffer.count = std::min(app.LightBuffer.count + 1, MaxLightCount);
            selectedLightIdx = app.LightBuffer.count - 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove light")) {
            if (app.LightBuffer.count > 0) {
                app.LightBuffer.count--;
            }
            if (selectedLightIdx == app.LightBuffer.count) {
                selectedLightIdx--;
            }
        }

        ImGui::Separator();
        if (selectedLightIdx != -1) {
            auto& light = app.lights[selectedLightIdx];

            static const char* LightTypeLabels[] = {
                "Point",
                "Directional"
            };

            ImGui::PushID("Light");

            if (ImGui::BeginCombo("Light Type", LightTypeLabels[light.lightType])) {
                for (int i = 0; i < _countof(LightTypeLabels); i++) {
                    if (ImGui::Selectable(LightTypeLabels[i], i == light.lightType)) {
                        light.lightType = (LightType)i;
                    }
                }
                ImGui::EndCombo();
            }


            ImGui::ColorEdit3("Color", (float*)&light.color, ImGuiColorEditFlags_PickerHueWheel);
            if (light.lightType == LightType_Point) {
                ImGui::DragFloat3("Position", (float*)&light.position, 0.1f);
            }
            if (light.lightType == LightType_Directional) {
                ImGui::DragFloat3("Direction", (float*)&light.direction, 0.1f);
            }
            ImGui::DragFloat("Range", &light.range, 0.1f, 0.0f, 1000.0f, nullptr, 1.0f);
            ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f, nullptr, 1.0f);

            ImGui::PopID();
        } else {
            ImGui::Text("No light selected");
        }
    }
}

void DrawEnvironmentEditor(App& app)
{
    if (ImGui::CollapsingHeader("Environment")) {
        ImGui::DragFloat3("Environment Intensity", &app.LightBuffer.passData->environmentIntensity[0]);
        ImGui::DragFloat("Gamma", &app.PostProcessPass.gamma, 0.1f, 0.0f, 3.0f, nullptr, 1.0f);
        ImGui::DragFloat("Exposure", &app.PostProcessPass.exposure, 0.1f, 0.0f, 2.0f, nullptr, 1.0f);
    }
}

void DrawStats(App& app)
{
    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoMove;

    if (app.ImGui.showStats) {
        float frameTimeMS = (float)app.Stats.lastFrameTimeNS / (float)1E6;
        ImGui::SetNextWindowSize(ImVec2(300, 200));
        if (ImGui::Begin("Stats", &app.ImGui.showStats, windowFlags)) {
            ImVec4 textColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);

            ImGui::SetWindowPos({ 0.0f, 20.0f });
            ImGui::TextColored(textColor, "Frame time: %.2fms", frameTimeMS);
            ImGui::TextColored(textColor, "FPS: %.0f", 1000.0f / frameTimeMS);
            ImGui::TextColored(textColor, "Triangles: %d", app.Stats.triangleCount);
            ImGui::TextColored(textColor, "Draw calls: %d", app.Stats.drawCalls.load());
        }
        ImGui::End();
    }
}

void DrawMenuBar(App& app)
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Add GLTF")) {
                const char* filters[] = { "*.gltf" };
                char* gltfFile = tinyfd_openFileDialog(
                    "Choose GLTF File",
                    app.dataDir.c_str(),
                    _countof(filters),
                    filters,
                    NULL,
                    0
                );

                if (gltfFile != nullptr) {
                    AssetBundle assets;
                    tinygltf::TinyGLTF loader;
                    EnqueueGLTF(app, gltfFile, AddModelToScene);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Reload Shaders")) {
                app.psoManager.Reload(app.device.Get());
            }
            if (ImGui::MenuItem("Reset Camera")) {
                InitializeCamera(app);
            }
            if (ImGui::Checkbox("Shader Debug Flag", (bool*)&app.LightBuffer.passData->debug)) {}
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            ImGui::Checkbox("Tools", &app.ImGui.toolsOpen);
            ImGui::Checkbox("ImGui Demo Window", &app.ImGui.demoOpen);
            ImGui::Checkbox("Show stats", &app.ImGui.showStats);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void DrawGeekMenu(App& app)
{
    static bool geekOpen = false;
    if (ImGui::CollapsingHeader("Nerd Stuff", &geekOpen)) {
        static bool debugSkybox = false;
        if (ImGui::Checkbox("Debug Diffuse IBL", &debugSkybox)) {
            if (app.Skybox.mesh && app.Skybox.mesh->isReadyForRender) {
                app.Skybox.mesh->primitives[0]->miscDescriptorParameter =
                    debugSkybox ? app.Skybox.irradianceCubeSRV.Ref() : app.Skybox.texcubeSRV.Ref();
            }
        }

        float degreesFOV = glm::degrees(app.camera.fovY);
        ImGui::DragFloat("Camera FOVy Degrees", &degreesFOV, 0.05f, 0.01f, 180.0f);
        app.camera.fovY = glm::radians(degreesFOV);

        if (app.ImGui.debugSRV.IsValid()) {
            ImGui::Text("Debug texture:");
            auto handle = app.ImGui.debugSRV.GPUHandle();
            ImVec2 debugSize = ImGui::GetContentRegionAvail();
            ImGui::Image(*reinterpret_cast<void**>(&handle), debugSize);
        }

        for (const auto& loadInfo : app.AssetThread.assetLoadInfo) {
            if (loadInfo->isFinished) {
                continue;
            }
            ImGui::Text("Loading %s %c", loadInfo->assetPath.c_str(), "|/-\\"[(int)(ImGui::GetTime() / 0.05f) & 3]);
            ImGui::Indent();
            ImGui::Text("%f%% %s", loadInfo->overallPercent * 100.0f, loadInfo->currentTask.c_str());
            ImGui::Unindent();
        }

        ImGui::Checkbox("Disable Shadows", &app.RenderSettings.disableShadows);
    }
}

void BeginGUI(App& app)
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (ImGui::Begin("Tools", &app.ImGui.toolsOpen)) {
        DrawLightEditor(app);
        DrawEnvironmentEditor(app);
        DrawMaterialEditor(app);
        DrawMeshEditor(app);
        DrawGeekMenu(app);
    }
    ImGui::End();

    DrawStats(app);
    DrawMenuBar(app);

    if (app.ImGui.demoOpen) {
        ImGui::ShowDemoWindow(&app.ImGui.demoOpen);
    }
}