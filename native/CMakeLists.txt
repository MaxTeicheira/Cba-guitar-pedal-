cmake_minimum_required(VERSION 3.22)
project(mood_emulator LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Windows: link the MSVC runtime statically. A plugin that imports MSVCP140/VCRUNTIME140
# fails to load on machines without the Visual C++ Redistributable, and the DAW reports
# only "import failed". tools/check_pe_imports.py enforces this in CI.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Header-only DSP core (C++ port of src/mood-worklet.js). No -ffast-math: the parity
# test compares against the JS reference and needs IEEE-exact arithmetic.
add_library(mood_dsp INTERFACE)
target_include_directories(mood_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(mood_dsp INTERFACE
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -ffp-contract=off>
  $<$<CXX_COMPILER_ID:MSVC>:/W3 /fp:precise /D_CRT_SECURE_NO_WARNINGS>)

add_executable(dsp_tests tests/dsp_tests.cpp)
target_link_libraries(dsp_tests PRIVATE mood_dsp)
add_executable(mood_render tests/render.cpp)
target_link_libraries(mood_render PRIVATE mood_dsp)
enable_testing()
add_test(NAME dsp_tests COMMAND dsp_tests)

option(MOOD_BUILD_PLUGIN "Build the JUCE standalone + VST3" OFF)
if(MOOD_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()
