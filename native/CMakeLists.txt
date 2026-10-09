cmake_minimum_required(VERSION 3.22)
project(badmood_emulator LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Windows: link the MSVC runtime statically. A plugin that imports MSVCP140/VCRUNTIME140
# fails to load on machines without the Visual C++ Redistributable, and the DAW reports
# only "import failed". tools/check_pe_imports.py enforces this in CI.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Header-only DSP core (C++ port of src/badmood-worklet.js). No -ffast-math and no FMA
# contraction: the parity test compares against the JS reference and needs IEEE-exact
# arithmetic. The plugin links this target too, so it gets the same flags.
add_library(badmood_dsp INTERFACE)
target_include_directories(badmood_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(badmood_dsp INTERFACE
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -ffp-contract=off>
  $<$<CXX_COMPILER_ID:MSVC>:/W3 /fp:precise /D_CRT_SECURE_NO_WARNINGS>)

add_executable(dsp_tests tests/dsp_tests.cpp)
target_link_libraries(dsp_tests PRIVATE badmood_dsp)
add_executable(badmood_render tests/render.cpp)
target_link_libraries(badmood_render PRIVATE badmood_dsp)
enable_testing()
add_test(NAME dsp_tests COMMAND dsp_tests)

option(BADMOOD_BUILD_PLUGIN "Build the JUCE standalone + VST3" OFF)
if(BADMOOD_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()
