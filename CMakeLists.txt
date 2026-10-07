cmake_minimum_required(VERSION 3.20)
project(ClxpEQ CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(ClxpEQ SHARED main.cpp)
set_target_properties(ClxpEQ PROPERTIES OUTPUT_NAME "@Clxp" PREFIX "")
target_link_libraries(ClxpEQ PRIVATE gdiplus user32 gdi32)
target_compile_definitions(ClxpEQ PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
