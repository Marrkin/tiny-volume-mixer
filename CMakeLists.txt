cmake_minimum_required(VERSION 3.24)
project(TinyVolumeMixer VERSION 0.2.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "TinyVolumeMixer targets Windows. Use MSVC or a Windows cross toolchain.")
endif()
enable_language(RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(tvm_options INTERFACE)
target_compile_definitions(tvm_options INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
    target_compile_options(tvm_options INTERFACE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(tvm_options INTERFACE -Wall -Wextra -Wpedantic)
endif()
if(MINGW)
    target_link_options(tvm_options INTERFACE -static)
endif()

add_library(tvm_audio STATIC src/audio_engine.cpp src/model.cpp src/app_identity.cpp)
target_include_directories(tvm_audio PUBLIC src)
target_link_libraries(tvm_audio PUBLIC tvm_options ole32 uuid propsys version shell32)

add_executable(TinyVolumeMixer WIN32
    src/main.cpp src/mixer_window.cpp src/settings.cpp src/theme.cpp resources/app.rc)
target_include_directories(TinyVolumeMixer PRIVATE resources)
target_link_libraries(TinyVolumeMixer PRIVATE tvm_audio
    comctl32 shell32 advapi32 gdi32 user32 dwmapi uxtheme shcore gdiplus)
if(MINGW)
    target_link_options(TinyVolumeMixer PRIVATE -municode "$<$<CONFIG:Release>:-s>")
endif()
if(MSVC)
    target_link_options(TinyVolumeMixer PRIVATE /MANIFEST:NO)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(tvm_model_tests tests/model_tests.cpp src/model.cpp)
    target_include_directories(tvm_model_tests PRIVATE src)
    target_link_libraries(tvm_model_tests PRIVATE tvm_options)
    add_test(NAME model COMMAND tvm_model_tests)

    add_executable(tvm_painting_tests tests/painting_tests.cpp src/theme.cpp)
    target_include_directories(tvm_painting_tests PRIVATE src)
    target_link_libraries(tvm_painting_tests PRIVATE tvm_options gdiplus gdi32 user32 advapi32 ole32)
    add_test(NAME painting COMMAND tvm_painting_tests)

    # Run manually on a Windows desktop with an output device, never on the CI runner.
    add_executable(tvm_audio_smoke tests/audio_smoke.cpp)
    target_link_libraries(tvm_audio_smoke PRIVATE tvm_audio user32)
    add_executable(tvm_audio_inventory tests/audio_inventory.cpp)
    target_link_libraries(tvm_audio_inventory PRIVATE tvm_audio)
endif()

install(TARGETS TinyVolumeMixer RUNTIME DESTINATION .)
install(FILES LICENSE README.md DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs FILES_MATCHING PATTERN "*.md" PATTERN "*.png")
