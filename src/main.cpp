#include "mdxr.h"
#include <directx/d3dx12.h>
#include <iostream>

int main(int argc, char** argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        std::cout << "Failed to initialize the SDL2 library\n";
        std::cout << "SDL2 Error: " << SDL_GetError() << "\n";
        return -1;
    }

    RunMDXR(argc, argv);
}