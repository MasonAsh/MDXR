cmake_minimum_required(VERSION 3.7)

project(mdxr)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION 19041)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIR})

find_package(NsightAftermath)
get_filename_component(NsightAftermath_LIBRARY_DIR ${NsightAftermath_LIBRARY} DIRECTORY)
message(STATUS "Nsight Aftermath FOUND = ${NsightAftermath_FOUND}")
message(STATUS "Nsight Aftermath Lib Dir = ${NsightAftermath_LIBRARY_DIR}")
message(STATUS "Nsight Aftermath Lib = ${NsightAftermath_LIBRARY}")
message(STATUS "Nsight Aftermath Headers Include = ${NsightAftermath_INCLUDE_DIR}")

include_directories(${NsightAftermath_INCLUDE_DIR})

find_package(WinPixEventRuntime)
include_directories(${WinPixEventRuntime_INCLUDE_DIR})

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

add_executable(mdxr WIN32
    src/main.cpp
    src/util.h
    src/mdxr.h
    src/mdxr.cpp
    src/app.h
    src/pool.h
    src/gltftangentspace.cpp
    src/uploadbatch.h
    src/incrementalfence.h
    src/pso.h
    src/pso.cpp
    src/gbuffer.h
    src/d3dutils.h
    src/assets.h
    src/assets.cpp
    src/scene.h
    src/scene.cpp
    src/gui.h
    src/gui.cpp
    src/renderer.h
    src/renderer.cpp
    src/commandqueue.h
    src/descriptorpool.h
    src/crc32.h
    src/constantbufferstructures.h
    src/internalmeshes.h
    src/internalmeshes.cpp

    # third party
    thirdparty/tiny_gltf_impl.cpp
    thirdparty/tinyfiledialogs.c
    thirdparty/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp
    thirdparty/mikktspace.c

    ${IMGUI_SOURCES}
)

include_directories(thirdparty/include)
include_directories(thirdparty/imgui/)
include_directories(thirdparty/imgui/backends)
include_directories(thirdparty/D3D12MemoryAllocator/include)

target_compile_definitions(mdxr
    PRIVATE
        GLM_FORCE_RADIANS
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        _CRT_SECURE_NO_WARNINGS
    PUBLIC
        $<$<CONFIG:Debug>:MDXR_DEBUG=1>
        $<$<CONFIG:RelWithDebInfo>:MDXR_DEBUG=1 USE_PIX=1>
)

target_link_libraries(mdxr 
    ${SDL2_LIBRARY}
    ${SDL2MAIN_LIBRARY} 
    DirectX-Headers 
    DirectX-Guids 
    D3d12 
    DXGI 
    D3DCompiler 
    DXGuid
    ${NsightAftermath_LIBRARY}
    WinPixEventRuntime::WinPixEventRuntime
)

# file(COPY ${CMAKE_SOURCE_DIR}/D3D12 DESTINATION ${TARGET_FILE_DIR})

add_custom_command(TARGET mdxr POST_BUILD 
               COMMAND ${CMAKE_COMMAND} -E copy_directory
               "${CMAKE_SOURCE_DIR}/D3D12"              
                $<TARGET_FILE_DIR:mdxr>/D3D12)