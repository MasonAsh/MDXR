cmake_minimum_required(VERSION 3.7)

project(mdxr)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/modules/")

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION 19041)

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIR})

add_subdirectory(DirectX-Headers)

set(IMGUI_SOURCES
    thirdparty/imgui/imgui.cpp
    thirdparty/imgui/imgui_demo.cpp
    thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_widgets.cpp
    thirdparty/imgui/imgui_tables.cpp
    thirdparty/imgui/backends/imgui_impl_sdl.cpp
    thirdparty/imgui/backends/imgui_impl_dx12.cpp
)

add_executable(mdxr
    src/main.cpp
    src/util.h
    src/mdxr.h
    src/mdxr.cpp
    src/tiny_gltf_impl.cpp

    # third party
    src/stb_image.h
    src/stb_image_write.h
    src/json.hpp
    src/tiny_gltf.h

    ${IMGUI_SOURCES}
)

include_directories(thirdparty/include)
include_directories(thirdparty/imgui/)
include_directories(thirdparty/imgui/backends)

target_compile_definitions(mdxr
    PRIVATE
        GLM_FORCE_RADIANS
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        _CRT_SECURE_NO_WARNINGS
    PUBLIC
        $<$<CONFIG:Debug>:D3DCOMPILE_DEBUG=1>
        $<$<CONFIG:RelWithDebInfo>:D3DCOMPILE_DEBUG=1>
)

target_link_libraries(mdxr ${SDL2_LIBRARY} ${SDL2MAIN_LIBRARY} DirectX-Headers DirectX-Guids D3d12 DXGI D3DCompiler DXGuid)

# file(COPY ${CMAKE_SOURCE_DIR}/D3D12 DESTINATION ${TARGET_FILE_DIR})

add_custom_command(TARGET mdxr POST_BUILD 
               COMMAND ${CMAKE_COMMAND} -E copy_directory
               "${CMAKE_SOURCE_DIR}/D3D12"              
                $<TARGET_FILE_DIR:mdxr>/D3D12)