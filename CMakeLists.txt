cmake_minimum_required(VERSION 3.20)

project(arkoverlay)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(arkoverlay_demo
    demo_main.cpp
    arkoverlay.cpp
)

target_compile_definitions(arkoverlay_demo PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

target_link_libraries(arkoverlay_demo PRIVATE
    gdi32
    user32
    gdiplus
)