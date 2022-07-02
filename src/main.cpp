#include "mdxr.h"
#include <directx/d3dx12.h>
#include <iostream>

// Use the Agility SDK
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 600;}
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

int main(int argc, char** argv)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0)
    {
        std::cout << "Failed to initialize the SDL2 library\n";
        std::cout << "SDL2 Error: " << SDL_GetError() << "\n";
        return -1;
    }

    return RunMDXR(argc, argv);
}