#include "mdxr.h"
#include <directx/d3dx12.h>
#include <iostream>
#include <fstream>
#include "util.h"

// Use the Agility SDK
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 600;}
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

int main(int argc, char** argv)
{
    {
        // Clear debug log
        std::ofstream debugLog("debug.txt", std::ios_base::out);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        DebugLog() << "Failed to initialize the SDL2 library";
        DebugLog() << "SDL2 Error: " << SDL_GetError();
        return -1;
    }

    return RunMDXR(argc, argv);
}