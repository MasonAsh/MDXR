#include <SDL.h>
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

    SDL_Window* window = SDL_CreateWindow("MDXR",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        680, 480,
        0);

    if (!window)
    {
        std::cout << "Failed to create window\n";
        std::cout << "SDL2 Error: " << SDL_GetError() << "\n";
        return -1;
    }

    SDL_Event e;

    bool running = true;
    while (running) {
        while (SDL_PollEvent(&e) > 0) {
            if (e.type == SDL_QUIT) {
                running = false;
            }
        }
    }

    return 0;
}