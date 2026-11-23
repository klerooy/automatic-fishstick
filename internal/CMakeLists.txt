cmake_minimum_required(VERSION 3.20)
project(RRInternal LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8-docking
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageworker/minhook.git
    GIT_TAG master
)
FetchContent_MakeAvailable(minhook)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC d3d11 dxgi)

add_library(RRInternal SHARED
    src/dllmain.cpp
    src/unity.cpp
    src/game.cpp
    src/hooks.cpp
    src/render.cpp
    src/aim.cpp
    src/config.cpp
)
target_include_directories(RRInternal PRIVATE src)
target_link_libraries(RRInternal PRIVATE imgui_lib minhook d3d11 dxgi d3dcompiler)
target_compile_definitions(RRInternal PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(RRInternal PROPERTIES
    OUTPUT_NAME "RRInternal"
    PREFIX ""
    SUFFIX ".dll"
)