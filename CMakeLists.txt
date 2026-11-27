cmake_minimum_required(VERSION 3.20)

project(SigmaTweaks LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "Sigma Tweaks is a native Windows application.")
endif()

add_executable(SigmaTweaks WIN32
    src/main.cpp
    src/SigmaTweaks.rc
)

target_compile_features(SigmaTweaks PRIVATE cxx_std_17)
target_compile_definitions(SigmaTweaks PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_include_directories(SigmaTweaks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(SigmaTweaks PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
target_link_libraries(SigmaTweaks PRIVATE user32 gdi32)
