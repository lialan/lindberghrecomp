cmake_minimum_required(VERSION 3.20)
project(lindbergh_vulkan LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Vulkan REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
foreach(stage vert frag)
    set(shader "${CMAKE_CURRENT_BINARY_DIR}/colored_${stage}.spv")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/colored_${stage}.hpp")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/colored.${stage}" -o "${shader}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.py"
            "${shader}" "${header}" "colored_${stage}"
        DEPENDS "shaders/colored.${stage}" embed_spirv.py VERBATIM)
    list(APPEND shaders "${header}")
endforeach()
add_library(lindbergh_vulkan STATIC device.cpp frame_ring.cpp renderer.cpp guest_state.cpp uniforms.cpp resources.cpp guest_arrays.cpp shader_pack.cpp ${shaders})
target_include_directories(lindbergh_vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(lindbergh_vulkan PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(lindbergh_vulkan PUBLIC Vulkan::Vulkan)
target_compile_options(lindbergh_vulkan PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

include(CTest)
if(BUILD_TESTING)
    find_program(SPIRV_CROSS spirv-cross REQUIRED)
    set(fixture "${CMAKE_CURRENT_BINARY_DIR}/legacy-fixture")
    add_custom_command(OUTPUT "${fixture}/vert.spv" "${fixture}/frag.spv" "${fixture}/fixture.lvsp"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/vulkan/compile_fixture.py"
            "${fixture}" "${GLSLANG_VALIDATOR}" "${SPIRV_CROSS}"
        DEPENDS ../../../tests/vulkan/compile_fixture.py ../../../tests/vulkan/legacy.vert
            ../../../tests/vulkan/legacy.frag ../../../tools/vulkan/compile_shaders.py
            ../../../tools/vulkan/legacy.glsl ../../../tools/vulkan/pack_shaders.py VERBATIM)
    add_custom_target(vulkan_legacy_fixture DEPENDS "${fixture}/vert.spv" "${fixture}/frag.spv" "${fixture}/fixture.lvsp")
    add_executable(lindbergh_vulkan_selftest ../../../tests/vulkan/selftest.cpp)
    add_dependencies(lindbergh_vulkan_selftest vulkan_legacy_fixture)
    target_compile_definitions(lindbergh_vulkan_selftest PRIVATE LV_TEST_SHADER_DIR="${fixture}" LV_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/vulkan")
    target_link_libraries(lindbergh_vulkan_selftest PRIVATE lindbergh_vulkan)
    add_test(NAME lindbergh_vulkan_selftest COMMAND lindbergh_vulkan_selftest)
endif()

option(LINDBERGH_VULKAN_SDL "Build SDL Vulkan desktop presentation" OFF)
if(LINDBERGH_VULKAN_SDL)
    find_package(SDL2 REQUIRED)
    add_library(lindbergh_vulkan_sdl STATIC window.cpp)
    target_link_libraries(lindbergh_vulkan_sdl PUBLIC lindbergh_vulkan SDL2::SDL2)
    if(BUILD_TESTING)
        add_executable(lindbergh_vulkan_window_test ../../../tests/vulkan/window_test.cpp)
        target_link_libraries(lindbergh_vulkan_window_test PRIVATE lindbergh_vulkan_sdl)
    endif()
endif()

option(LINDBERGH_VULKAN_OPENXR "Build direct OpenXR Vulkan host and diagnostic" OFF)
if(LINDBERGH_VULKAN_OPENXR)
    find_package(OpenXR 1.1 CONFIG REQUIRED)
    add_library(lindbergh_vulkan_openxr STATIC openxr.cpp)
    target_link_libraries(lindbergh_vulkan_openxr PUBLIC lindbergh_vulkan OpenXR::openxr_loader)
    if(ANDROID AND BUILD_TESTING)
        add_library(lindbergh_vulkan_android_xr_probe SHARED ../../../tests/vulkan/android_probe.cpp)
        target_link_libraries(lindbergh_vulkan_android_xr_probe PRIVATE lindbergh_vulkan_openxr android log)
    endif()
    add_executable(lindbergh_vulkan_xr_probe ../../../tests/vulkan/openxr_probe.cpp)
    target_link_libraries(lindbergh_vulkan_xr_probe PRIVATE lindbergh_vulkan_openxr)
endif()

find_program(SPIRV_CROSS_BUILD spirv-cross REQUIRED)
set(fixed_archive "${CMAKE_CURRENT_BINARY_DIR}/fixed-pack/fixed.lvsp")
add_custom_command(OUTPUT "${fixed_archive}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/vulkan/build_fixed.py"
        "${CMAKE_CURRENT_BINARY_DIR}/fixed-pack" "${GLSLANG_VALIDATOR}" "${SPIRV_CROSS_BUILD}"
    DEPENDS shaders/fixed.vert shaders/fixed.frag ../../../tools/vulkan/build_fixed.py
        ../../../tools/vulkan/compile_shaders.py ../../../tools/vulkan/pack_shaders.py ../../../tools/vulkan/legacy.glsl VERBATIM)
add_custom_target(vulkan_fixed_pack DEPENDS "${fixed_archive}")
add_dependencies(lindbergh_vulkan vulkan_fixed_pack)
set(LINDBERGH_VULKAN_FIXED_ARCHIVE "${fixed_archive}" CACHE INTERNAL "Fixed-function Vulkan shaders" FORCE)
