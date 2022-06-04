cmake_minimum_required(VERSION 3.7)

project(mdxr)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/modules/")

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION 19041)

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIR})

add_subdirectory(DirectX-Headers)

add_executable(mdxr
    src/main.cpp
    src/util.h
    src/mdxr.h
    src/mdxr.cpp
    src/tiny_gltf_impl.cpp
    src/mesh.h
    src/mesh.cpp

    # third party
    src/stb_image.h
    src/stb_image_write.h
    src/json.hpp
    src/tiny_gltf.h
)

include_directories(thirdparty/include)

target_compile_definitions(mdxr
    PRIVATE
        GLM_FORCE_RADIANS
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_FORCE_LEFT_HANDED
        _CRT_SECURE_NO_WARNINGS
    PUBLIC
        $<$<CONFIG:Debug>:D3DCOMPILE_DEBUG=1>
        $<$<CONFIG:RelWithDebInfo>:D3DCOMPILE_DEBUG=1>
)

target_link_libraries(mdxr ${SDL2_LIBRARY} ${SDL2MAIN_LIBRARY} DirectX-Headers DirectX-Guids D3d12 DXGI D3DCompiler)