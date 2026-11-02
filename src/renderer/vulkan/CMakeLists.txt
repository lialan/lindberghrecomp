cmake_minimum_required(VERSION 3.20)
project(lindbergh_vulkan LANGUAGES C CXX)
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
foreach(name overlay_gun.vert overlay_gun.frag overlay_beam.vert overlay_beam.frag overlay_panel.vert overlay_panel.frag)
    string(REPLACE "." "_" symbol "${name}")
    set(shader "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.spv")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.hpp")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name}" -o "${shader}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.py" "${shader}" "${header}" "${symbol}"
        DEPENDS "shaders/${name}" embed_spirv.py VERBATIM)
    list(APPEND shaders "${header}")
endforeach()
add_library(lindbergh_vulkan STATIC ../../runtime/xr_panel.c ../../runtime/xr_menu.c ../../runtime/host_stats.c ../../runtime/xr_overlay_geometry.c ../../runtime/stereo_projection.c ../../runtime/xr_controls.c controller_input.cpp settings_panel.cpp overlay.cpp pipeline_recipes.cpp device.cpp frame_ring.cpp renderer.cpp guest_state.cpp stereo_scene.cpp uniforms.cpp resources.cpp guest_arrays.cpp guest_indices.cpp guest_pixels.cpp shader_pack.cpp ${shaders})
target_include_directories(lindbergh_vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(lindbergh_vulkan PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(lindbergh_vulkan PUBLIC Vulkan::Vulkan)
target_compile_options(lindbergh_vulkan PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# Runtime GLSL -> SPIR-V, for guest programs missing from a game's offline
# pack (its draws would otherwise be skipped). glslang is compiled from the
# copy the Android NDK ships (shaderc's third_party), on Quest and on the Mac
# alike; without it the runtime keeps skipping such draws.
set(LINDBERGH_GLSLANG_SOURCE "" CACHE PATH "glslang source tree (default: the Android NDK's shaderc copy)")
set(glslang_source "${LINDBERGH_GLSLANG_SOURCE}")
if(NOT glslang_source)
    set(ndk_roots "${CMAKE_ANDROID_NDK}" "${ANDROID_NDK}" "$ENV{ANDROID_NDK_HOME}")
    file(GLOB sdk_ndks "$ENV{HOME}/Library/Android/sdk/ndk/*")
    list(SORT sdk_ndks COMPARE NATURAL ORDER DESCENDING)
    foreach(root IN LISTS ndk_roots sdk_ndks)
        if(root AND EXISTS "${root}/sources/third_party/shaderc/third_party/glslang/glslang/Public/ShaderLang.h")
            set(glslang_source "${root}/sources/third_party/shaderc/third_party/glslang")
            break()
        endif()
    endforeach()
endif()
if(glslang_source AND EXISTS "${glslang_source}/glslang/Public/ShaderLang.h")
    file(GLOB glslang_sources
        "${glslang_source}/glslang/MachineIndependent/*.cpp"
        "${glslang_source}/glslang/MachineIndependent/preprocessor/*.cpp"
        "${glslang_source}/glslang/GenericCodeGen/*.cpp")
    list(APPEND glslang_sources "${glslang_source}/glslang/OSDependent/Unix/ossource.cpp"
        "${glslang_source}/OGLCompilersDLL/InitializeDll.cpp"
        "${glslang_source}/SPIRV/GlslangToSpv.cpp" "${glslang_source}/SPIRV/InReadableOrder.cpp"
        "${glslang_source}/SPIRV/Logger.cpp" "${glslang_source}/SPIRV/SpvBuilder.cpp"
        "${glslang_source}/SPIRV/SpvPostProcess.cpp" "${glslang_source}/SPIRV/doc.cpp")
    file(STRINGS "${glslang_source}/CHANGES.md" glslang_release REGEX "^## [0-9]+\\.[0-9]+\\.[0-9]+" LIMIT_COUNT 1)
    string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" glslang_version "${glslang_release}")
    set(glslang_info "${CMAKE_CURRENT_BINARY_DIR}/glslang-generated/glslang/build_info.h")
    file(READ "${glslang_source}/build_info.h.tmpl" glslang_template)
    string(REPLACE "@major@" "${CMAKE_MATCH_1}" glslang_template "${glslang_template}")
    string(REPLACE "@minor@" "${CMAKE_MATCH_2}" glslang_template "${glslang_template}")
    string(REPLACE "@patch@" "${CMAKE_MATCH_3}" glslang_template "${glslang_template}")
    string(REPLACE "@flavor@" "" glslang_template "${glslang_template}")
    file(WRITE "${glslang_info}" "${glslang_template}")
    add_library(lindbergh_glslang STATIC ${glslang_sources})
    target_include_directories(lindbergh_glslang PUBLIC "${glslang_source}" "${CMAKE_CURRENT_BINARY_DIR}/glslang-generated")
    target_compile_definitions(lindbergh_glslang PUBLIC GLSLANG_OSINCLUDE_UNIX)
    target_compile_options(lindbergh_glslang PRIVATE -w)
    set_target_properties(lindbergh_glslang PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # The lowering's GLSL prelude, the same text compile_shaders.py prepends.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/vulkan/legacy.glsl" LVK_LEGACY_GLSL)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/vulkan/sampling.glsl" LVK_SAMPLING_GLSL)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/runtime_shader_text.h"
        CONTENT "static const char lvk_legacy_glsl[] = R\"lvkglsl(@LVK_LEGACY_GLSL@)lvkglsl\";\nstatic const char lvk_sampling_glsl[] = R\"lvkglsl(@LVK_SAMPLING_GLSL@)lvkglsl\";\n" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/vulkan/legacy.glsl" "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/vulkan/sampling.glsl")
    target_sources(lindbergh_vulkan PRIVATE runtime_shaders.cpp)
    target_link_libraries(lindbergh_vulkan PUBLIC lindbergh_glslang)
    target_compile_definitions(lindbergh_vulkan PUBLIC LINDBERGH_RUNTIME_SHADERS=1)
    message(STATUS "Vulkan: runtime shader compiler from ${glslang_source} (glslang ${glslang_version})")
    if(NOT ANDROID)
        # Local check against a game's offline pack (game shaders: not in CI).
        add_executable(lindbergh_runtime_shader_check ../../../tests/vulkan/runtime_shaders_check.cpp)
        target_link_libraries(lindbergh_runtime_shader_check PRIVATE lindbergh_vulkan)
    endif()
else()
    message(STATUS "Vulkan: no glslang source; guest programs missing from the offline pack skip their draws")
endif()

include(CTest)
if(BUILD_TESTING)
    find_program(SPIRV_CROSS spirv-cross REQUIRED)
    set(fixture "${CMAKE_CURRENT_BINARY_DIR}/legacy-fixture")
    add_custom_command(OUTPUT "${fixture}/vert.spv" "${fixture}/frag.spv" "${fixture}/fixture.lvsp"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/vulkan/compile_fixture.py"
            "${fixture}" "${GLSLANG_VALIDATOR}" "${SPIRV_CROSS}"
        DEPENDS ../../../tests/vulkan/compile_fixture.py ../../../tests/vulkan/legacy.vert
            ../../../tests/vulkan/legacy.frag ../../../tools/vulkan/compile_shaders.py
            ../../../tools/vulkan/legacy.glsl ../../../tools/vulkan/sampling.glsl ../../../tools/vulkan/pack_shaders.py VERBATIM)
    add_custom_target(vulkan_legacy_fixture DEPENDS "${fixture}/vert.spv" "${fixture}/frag.spv" "${fixture}/fixture.lvsp")
    add_executable(lindbergh_vulkan_selftest ../../../tests/vulkan/selftest.cpp)
    add_dependencies(lindbergh_vulkan_selftest vulkan_legacy_fixture)
    target_compile_definitions(lindbergh_vulkan_selftest PRIVATE LV_TEST_SHADER_DIR="${fixture}" LV_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/vulkan")
    target_link_libraries(lindbergh_vulkan_selftest PRIVATE lindbergh_vulkan)
    add_test(NAME lindbergh_vulkan_selftest COMMAND lindbergh_vulkan_selftest)
    if(NOT WIN32)   # forks per fake /sys layout
        add_executable(lindbergh_stats_test ../../../tests/stats/stats_test.c ../../runtime/host_stats.c)
        target_include_directories(lindbergh_stats_test PRIVATE ../../runtime)
        add_test(NAME lindbergh_stats_test COMMAND lindbergh_stats_test)
    endif()
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
    add_library(lindbergh_vulkan_openxr STATIC openxr.cpp ../../runtime/xr_actions.c)
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
        ../../../tools/vulkan/compile_shaders.py ../../../tools/vulkan/pack_shaders.py ../../../tools/vulkan/legacy.glsl ../../../tools/vulkan/sampling.glsl VERBATIM)
add_custom_target(vulkan_fixed_pack DEPENDS "${fixed_archive}")
add_dependencies(lindbergh_vulkan vulkan_fixed_pack)
set(LINDBERGH_VULKAN_FIXED_ARCHIVE "${fixed_archive}" CACHE INTERNAL "Fixed-function Vulkan shaders" FORCE)

option(LINDBERGH_VULKAN_GUEST_TEST "Build offscreen real guest-ABI target lifecycle test (requires SDL2 and GL ABI headers)" OFF)
if(BUILD_TESTING AND LINDBERGH_VULKAN_GUEST_TEST)
    find_package(SDL2 REQUIRED)
    if(NOT LINDBERGH_GL_HEADERS)
        message(FATAL_ERROR "LINDBERGH_VULKAN_GUEST_TEST requires LINDBERGH_GL_HEADERS for ABI declarations only")
    endif()
    set(guest_test "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/vulkan/guest_target_test.cpp")
    set(guest_abi "${CMAKE_CURRENT_BINARY_DIR}/guest-test-abi")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/vulkan/gen_abi.py"
        "${guest_abi}" "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/vulkan_guest.cpp" "${LINDBERGH_GL_HEADERS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/hle_gl_posix.c" "${guest_test}" COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${guest_test}" "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/vulkan_guest.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/vulkan/gen_abi.py" "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/hle_gl_posix.c" ${LINDBERGH_GL_HEADERS})
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fixed.frag" fixed_fragment_text)
    file(CONFIGURE OUTPUT "${guest_abi}/fixed_fragment_source.h"
        CONTENT "static const char lvk_fixed_fragment_source[] = R\"lvkfixed(@fixed_fragment_text@)lvkfixed\";\n" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fixed.frag")
    add_executable(lindbergh_vulkan_guest_target_test "${guest_test}" ../../runtime/vulkan_guest.cpp)
    add_dependencies(lindbergh_vulkan_guest_target_test vulkan_legacy_fixture)
    target_include_directories(lindbergh_vulkan_guest_target_test PRIVATE "${guest_abi}" "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime")
    foreach(header IN LISTS LINDBERGH_GL_HEADERS)
        get_filename_component(abi_header_dir "${header}" DIRECTORY)
        target_include_directories(lindbergh_vulkan_guest_target_test PRIVATE "${abi_header_dir}" "${abi_header_dir}/..")
    endforeach()
    target_compile_definitions(lindbergh_vulkan_guest_target_test PRIVATE LINDBERGH_VULKAN=1 LINDBERGH_VULKAN_OFFSCREEN=1
        LINDBERGH_VULKAN_FIXED_ARCHIVE="${fixed_archive}" LV_TEST_SHADER_PACK="${fixture}/fixture.lvsp")
    target_link_libraries(lindbergh_vulkan_guest_target_test PRIVATE lindbergh_vulkan SDL2::SDL2)
    add_test(NAME lindbergh_vulkan_guest_target_test COMMAND lindbergh_vulkan_guest_target_test)
endif()
