#pragma once

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_sdl.h>


struct App;
struct ID3D12Resource;
struct D3D12_SHADER_RESOURCE_VIEW_DESC;

void DebugTextureGUI(App& app, ID3D12Resource* resource, D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc);

void InitImGui(App& app);
void CleanImGui();
void BeginGUI(App& app);
