#pragma once

#include <glm/glm.hpp>

struct App;

void HandleResize(App& app, int newWidth, int newHeight);
void InitD3D(App& app);
void UpdateRenderData(App& app, const glm::mat4& projection, const glm::mat4& view);
void WaitForPreviousFrame(App& app);
void RenderFrame(App& app);