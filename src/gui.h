#pragma once

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_sdl.h>


struct App;

void InitImGui(App& app);
void CleanImGui();
void BeginGUI(App& app);
