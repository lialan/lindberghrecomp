cmake_minimum_required(VERSION 3.20)
project(lindberghrecomp VERSION 0.1.0 LANGUAGES C)
if(APPLE)
    enable_language(OBJC)       # host_menu_mac.m, the menu bar
endif()

# Runtime for statically recompiled Sega Lindbergh games.
#
# The runtime cannot be a plain static library: two of its headers
# (recomp_imports.h, recomp_funcs_list.h) are generated from the game's own
# ELF, so the sources only compile in the context of one title. Hence a
# function a game project calls instead:
#
#   add_subdirectory(lindberghrecomp)
#   add_executable(mygame src/host.c ${GENERATED})
#   lindbergh_runtime(mygame games/mygame/generated)
#
# 64-bit hosts use the shared guest-memory window and typed GL thunks.
# Legacy 32-bit Windows retains its direct-address backend.
option(LINDBERGH_VULKAN_OFFSCREEN "Diagnostic game renderer without window surface or OpenXR" OFF)
# Native Linux (AArch64 Steam Frame / SteamOS, or any desktop Linux): Vulkan
# and OpenXR are opt-in there (-DLINDBERGH_RENDERER=VULKAN -DLINDBERGH_OPENXR=ON).
# (cached: lindbergh_runtime() runs in the calling project's scope)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    set(LINDBERGH_LINUX ON CACHE INTERNAL "native Linux host")
else()
    set(LINDBERGH_LINUX OFF CACHE INTERNAL "native Linux host")
endif()
# Native Vulkan is the default wherever its host exists (macOS, Android/Quest);
# -DLINDBERGH_RENDERER=OPENGL keeps the OpenGL renderer. Other hosts stay on
# OpenGL until the Vulkan host supports them.
if(APPLE OR ANDROID)
    set(lindbergh_default_renderer "VULKAN")
else()
    set(lindbergh_default_renderer "OPENGL")
endif()
set(LINDBERGH_RENDERER "${lindbergh_default_renderer}" CACHE STRING "Host renderer: OPENGL or VULKAN")
set_property(CACHE LINDBERGH_RENDERER PROPERTY STRINGS OPENGL VULKAN)
message(STATUS "Lindbergh renderer: ${LINDBERGH_RENDERER} (-DLINDBERGH_RENDERER=OPENGL|VULKAN)")
if(LINDBERGH_RENDERER STREQUAL "VULKAN")
    if(LINDBERGH_VULKAN_OFFSCREEN AND LINDBERGH_OPENXR)
        message(FATAL_ERROR "Offscreen diagnostics require -DLINDBERGH_OPENXR=OFF")
    endif()
    if(NOT APPLE AND NOT ANDROID AND NOT LINDBERGH_LINUX)
        message(FATAL_ERROR "Game Vulkan integration currently supports macOS, Android and Linux")
    endif()
    enable_language(CXX)
    if(LINDBERGH_VULKAN_OFFSCREEN)
        set(LINDBERGH_VULKAN_SDL OFF CACHE BOOL "SDL Vulkan host" FORCE)
        set(LINDBERGH_VULKAN_OPENXR OFF CACHE BOOL "Native Vulkan OpenXR host" FORCE)
    else()
        set(LINDBERGH_VULKAN_SDL ON CACHE BOOL "SDL Vulkan host" FORCE)
    endif()
    if(ANDROID)
        if(NOT DEFINED LINDBERGH_OPENXR OR LINDBERGH_OPENXR)
            set(LINDBERGH_VULKAN_OPENXR ON CACHE BOOL "Native Vulkan OpenXR host" FORCE)
        else()
            set(LINDBERGH_VULKAN_OPENXR OFF CACHE BOOL "Native Vulkan OpenXR host" FORCE)
        endif()
    elseif(LINDBERGH_LINUX AND NOT LINDBERGH_VULKAN_OFFSCREEN)
        if(LINDBERGH_OPENXR)
            set(LINDBERGH_VULKAN_OPENXR ON CACHE BOOL "Native Vulkan OpenXR host" FORCE)
        else()
            set(LINDBERGH_VULKAN_OPENXR OFF CACHE BOOL "Native Vulkan OpenXR host" FORCE)
        endif()
    endif()
    add_subdirectory(src/renderer/vulkan)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(LINDBERGH_VULKAN_PYTHON "${Python3_EXECUTABLE}" CACHE INTERNAL "Vulkan build Python")
    if(APPLE)
        execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE mac_sdk OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        set(default_gl_headers "${mac_sdk}/System/Library/Frameworks/OpenGL.framework/Headers/gl.h;${mac_sdk}/System/Library/Frameworks/OpenGL.framework/Headers/glext.h")
    elseif(LINDBERGH_LINUX AND EXISTS "/usr/include/GL/gl.h" AND EXISTS "/usr/include/GL/glext.h")
        set(default_gl_headers "/usr/include/GL/gl.h;/usr/include/GL/glext.h")
    endif()
    # These headers describe the guest ABI only; never link a GL driver.
    # Android may use Mesa's portable GL headers without building Mesa.
    set(LINDBERGH_GL_HEADERS "${default_gl_headers}" CACHE STRING "Semicolon-separated guest GL ABI declaration headers")
    if(NOT LINDBERGH_GL_HEADERS)
        message(FATAL_ERROR "Vulkan guest ABI requires -DLINDBERGH_GL_HEADERS=/path/to/GL/gl.h\;/path/to/GL/glext.h")
    endif()
    foreach(header IN LISTS LINDBERGH_GL_HEADERS)
        if(NOT EXISTS "${header}")
            message(FATAL_ERROR "Guest ABI header missing: ${header}")
        endif()
    endforeach()
    set(LINDBERGH_VULKAN_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/tools/vulkan/gen_abi.py" CACHE INTERNAL "guest ABI generator")
elseif(NOT LINDBERGH_RENDERER STREQUAL "OPENGL")
    message(FATAL_ERROR "Unknown LINDBERGH_RENDERER=${LINDBERGH_RENDERER}")
endif()

option(LINDBERGH_OPENXR "Build the shared OpenXR VR host" ${ANDROID})
if(LINDBERGH_VULKAN_OFFSCREEN AND (LINDBERGH_OPENXR OR NOT LINDBERGH_RENDERER STREQUAL "VULKAN"))
    message(FATAL_ERROR "Offscreen diagnostics require VULKAN and -DLINDBERGH_OPENXR=OFF")
endif()
if(LINDBERGH_OPENXR AND NOT ANDROID AND NOT (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
   AND NOT (LINDBERGH_LINUX AND LINDBERGH_RENDERER STREQUAL "VULKAN"))
    message(FATAL_ERROR "OpenXR host supports Android, Windows x64 and Linux (Vulkan)")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LINDBERGH_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
    CACHE INTERNAL "")
set(LINDBERGH_CPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pcrecomp/runtime/recomp32_cpu"
    CACHE INTERNAL "")

if(NOT EXISTS "${LINDBERGH_CPU_DIR}/cpu.h")
    message(FATAL_ERROR
        "pcrecomp submodule is missing (${LINDBERGH_CPU_DIR}/cpu.h).\n"
        "  git submodule update --init --recursive")
endif()

function(lindbergh_runtime target generated_dir)
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT MINGW)
        message(FATAL_ERROR "Windows x64 requires MinGW-w64 GNU C; use the MINGW64 build instructions")
    endif()
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(platform_sources
            "${LINDBERGH_RUNTIME_DIR}/hle_window.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_gl.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_cg.c")
    else()
        # A 64-bit host (macOS, Android, Windows): guest addresses are offsets into
        # one 4 GB window (guest_mem.c), and GL goes through typed thunks.
        set(platform_sources
            "${LINDBERGH_RUNTIME_DIR}/guest_mem.c"
            "${LINDBERGH_RUNTIME_DIR}/host_sdl.c"
            "${LINDBERGH_RUNTIME_DIR}/frame_timing.c"
            "${LINDBERGH_RUNTIME_DIR}/display_cadence.c"
            "${LINDBERGH_RUNTIME_DIR}/xr_settings.c"
            "${LINDBERGH_RUNTIME_DIR}/xr_controls.c"
            "${LINDBERGH_RUNTIME_DIR}/xr_panel.c"
            "${LINDBERGH_RUNTIME_DIR}/host_settings.c"
            "${LINDBERGH_RUNTIME_DIR}/game_profile.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_gl_posix.c"
            "${LINDBERGH_RUNTIME_DIR}/arbprog.c"
            "${LINDBERGH_RUNTIME_DIR}/glsl_pp.c"
            "${LINDBERGH_RUNTIME_DIR}/glsl_compat.c"
            "${LINDBERGH_RUNTIME_DIR}/stereo.c"
            "${LINDBERGH_RUNTIME_DIR}/stereo_projection.c"
            "${LINDBERGH_RUNTIME_DIR}/gl_thread.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_openal.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_alsa.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_glx_posix.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_cg.c"
            "${LINDBERGH_RUNTIME_DIR}/hud_widen.c"
            "${LINDBERGH_RUNTIME_DIR}/solib.c")
        target_compile_definitions(${target} PRIVATE RECOMP_GUEST_BASE=1)
    endif()
    if(LINDBERGH_RENDERER STREQUAL "VULKAN")
        list(REMOVE_ITEM platform_sources "${LINDBERGH_RUNTIME_DIR}/stereo.c" "${LINDBERGH_RUNTIME_DIR}/stereo_projection.c" "${LINDBERGH_RUNTIME_DIR}/xr_controls.c" "${LINDBERGH_RUNTIME_DIR}/xr_panel.c")
        set(abi "${CMAKE_CURRENT_BINARY_DIR}/vulkan-abi-${target}")
        set(abi_inputs "${LINDBERGH_RUNTIME_DIR}/hle_gl_posix.c"
            "${LINDBERGH_RUNTIME_DIR}/arbprog.c" "${LINDBERGH_RUNTIME_DIR}/host_sdl.c"
            "${LINDBERGH_RUNTIME_DIR}/gl_thread.c" "${LINDBERGH_RUNTIME_DIR}/hud_widen.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_glut.c" "${LINDBERGH_RUNTIME_DIR}/hle_glx_posix.c")
        if(DEFINED LINDBERGH_VULKAN_THUNKS_${target})
            list(APPEND abi_inputs "${LINDBERGH_VULKAN_THUNKS_${target}}")
        elseif(EXISTS "${generated_dir}/recomp_gl_thunks.c")
            list(APPEND abi_inputs "${generated_dir}/recomp_gl_thunks.c")
        endif()
        execute_process(COMMAND "${LINDBERGH_VULKAN_PYTHON}" "${LINDBERGH_VULKAN_GENERATOR}" "${abi}"
            "${LINDBERGH_RUNTIME_DIR}/vulkan_guest.cpp" "${LINDBERGH_GL_HEADERS}" ${abi_inputs}
            COMMAND_ERROR_IS_FATAL ANY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LINDBERGH_VULKAN_GENERATOR}"
            "${LINDBERGH_RUNTIME_DIR}/vulkan_guest.cpp" ${abi_inputs})
        target_sources(${target} PRIVATE "${LINDBERGH_RUNTIME_DIR}/vulkan_guest.cpp"
            "${abi}/vulkan_missing.cpp" "${abi}/vulkan_desktop_stereo.c")
        # fixed.frag's text: the fragment stage of guest programs without one.
        set(fixed_fragment "${LINDBERGH_RUNTIME_DIR}/../renderer/vulkan/shaders/fixed.frag")
        file(READ "${fixed_fragment}" fixed_fragment_text)
        file(CONFIGURE OUTPUT "${abi}/fixed_fragment_source.h"
            CONTENT "static const char lvk_fixed_fragment_source[] = R\"lvkfixed(@fixed_fragment_text@)lvkfixed\";\n" @ONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fixed_fragment}")
        target_include_directories(${target} PRIVATE "${abi}")
        target_compile_definitions(${target} PRIVATE LINDBERGH_VULKAN=1 LINDBERGH_VULKAN_FIXED_ARCHIVE="${LINDBERGH_VULKAN_FIXED_ARCHIVE}")
        if(LINDBERGH_VULKAN_OFFSCREEN)
            target_link_libraries(${target} PRIVATE lindbergh_vulkan)
            target_compile_definitions(${target} PRIVATE LINDBERGH_VULKAN_OFFSCREEN)
        else()
            target_link_libraries(${target} PRIVATE lindbergh_vulkan_sdl)
        endif()
        set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    endif()
    target_sources(${target} PRIVATE
        "${LINDBERGH_RUNTIME_DIR}/guest.c"
        "${LINDBERGH_RUNTIME_DIR}/syscall.c"
        "${LINDBERGH_RUNTIME_DIR}/hle.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_libc.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_pthread.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_vidmode.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_matrix.c"
        "${LINDBERGH_RUNTIME_DIR}/nn_culling.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_sega.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_rideboard.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_glut.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_zlib.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_cxx.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_segaapi.c"
        "${LINDBERGH_RUNTIME_DIR}/overrides.c"
        "${LINDBERGH_RUNTIME_DIR}/dispatch.c"
        "${LINDBERGH_RUNTIME_DIR}/crash_report.c"
        ${platform_sources}
    )
    target_include_directories(${target} PRIVATE
        "${LINDBERGH_RUNTIME_DIR}"
        "${LINDBERGH_CPU_DIR}"
        "${generated_dir}"
    )
    if(MSVC)
        # Generated code is correct and ugly: it converts between widths on
        # every other line and leaves labels no branch reaches.
        target_compile_options(${target} PRIVATE
            /W1 /wd4244 /wd4267 /wd4018 /wd4102 /wd4101 /wd4146 /wd4996)
        # No ASLR: the image is mapped at the addresses it was linked for.
        #
        # LARGEADDRESSAWARE because the guest is a Linux process and Linux
        # gives one 3 GB of user address space - it puts the stack up at
        # 0xBFFF0000. A 32-bit Windows process gets 2 GB by default, so that
        # address simply does not exist and the stack cannot be mapped. With
        # the flag set, a 32-bit process on 64-bit Windows gets the full 4 GB
        # and the guest can be laid out where it expects to be.
        target_link_options(${target} PRIVATE
            /DYNAMICBASE:NO /LARGEADDRESSAWARE /STACK:8388608)
        target_link_libraries(${target} PRIVATE opengl32 gdi32 user32 zlibstatic)
    else()
        target_compile_options(${target} PRIVATE
            -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
            -Wno-parentheses-equality -Wno-deprecated-declarations
            -Wno-constant-conversion -Wno-tautological-constant-out-of-range-compare
            -fno-strict-aliasing -fwrapv
            # Release builds leave out the toolchains' hardening defaults,
            # which cost the hot paths: a stack canary in every function with
            # a local array (each lifted one holding an XMM register) and
            # checked memcpy/strcpy. Debug builds keep them.
            "$<$<CONFIG:Release>:-fno-stack-protector;-U_FORTIFY_SOURCE>")
        find_package(SDL2 REQUIRED)
        target_link_libraries(${target} PRIVATE SDL2::SDL2)
        if(WIN32)
            find_package(ZLIB REQUIRED)
            target_link_libraries(${target} PRIVATE ZLIB::ZLIB)
        else()
            target_link_libraries(${target} PRIVATE z)
        endif()
        if(ANDROID)
            # Meta Quest: GL 2.1 through gl4es on GLES 3, OpenAL Soft, and
            # the headset through OpenXR (host_xr.c). Paths to the three are
            # given at configure time - see docs/QUEST.md.
            set(android_deps OPENAL_DIR)
            if(LINDBERGH_RENDERER STREQUAL "VULKAN")
                if(LINDBERGH_OPENXR)
                    target_compile_definitions(${target} PRIVATE LINDBERGH_OPENXR LINDBERGH_VULKAN_XR)
                    target_link_libraries(${target} PRIVATE lindbergh_vulkan_openxr)
                endif()
                foreach(header IN LISTS LINDBERGH_GL_HEADERS)
                    get_filename_component(gl_dir "${header}" DIRECTORY)
                    target_include_directories(${target} PRIVATE "${gl_dir}/..")
                endforeach()
            else()
                list(APPEND android_deps GL4ES_DIR OPENXR_DIR)
            endif()
            foreach(v IN LISTS android_deps)
                if(NOT ${v})
                    message(FATAL_ERROR "Android build: set -D${v}=... (docs/QUEST.md)")
                endif()
            endforeach()
            if(NOT LINDBERGH_RENDERER STREQUAL "VULKAN")
                target_sources(${target} PRIVATE "${LINDBERGH_RUNTIME_DIR}/host_xr.c"
                    "${LINDBERGH_RUNTIME_DIR}/xr_actions.c"
                    "${LINDBERGH_RUNTIME_DIR}/xr_overlay.c" "${LINDBERGH_RUNTIME_DIR}/xr_overlay_geometry.c" "${LINDBERGH_RUNTIME_DIR}/xr_menu.c")
                target_compile_definitions(${target} PRIVATE LINDBERGH_GL4ES LINDBERGH_OPENXR)
                target_include_directories(${target} PRIVATE "${GL4ES_DIR}/include" "${OPENXR_DIR}/include")
                target_link_libraries(${target} PRIVATE "${GL4ES_LIB}" "${OPENXR_LIB}" EGL)
            endif()
            # printf and friends are handed the guest's own format strings
            target_compile_options(${target} PRIVATE -Wno-format-security)
            target_include_directories(${target} PRIVATE "${OPENAL_DIR}/include")
            target_link_libraries(${target} PRIVATE "${OPENAL_LIB}" log android dl m)
            target_link_options(${target} PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,--no-undefined")
        endif()
        if(WIN32)
            if(NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
                message(FATAL_ERROR "Shared Windows runtime requires MinGW-w64 x64 (GNU C)")
            endif()
            find_package(GLEW REQUIRED)
            find_package(OpenAL REQUIRED)
            target_compile_definitions(${target} PRIVATE SDL_MAIN_HANDLED _WIN32_WINNT=0x0601)
            target_link_libraries(${target} PRIVATE GLEW::GLEW OpenAL::OpenAL opengl32 winpthread)
            target_link_options(${target} PRIVATE -Wl,--stack,268435456 -static-libgcc)
            if(LINDBERGH_OPENXR)
                find_package(OpenXR CONFIG REQUIRED)
                target_sources(${target} PRIVATE "${LINDBERGH_RUNTIME_DIR}/host_xr.c"
                    "${LINDBERGH_RUNTIME_DIR}/xr_actions.c"
                    "${LINDBERGH_RUNTIME_DIR}/xr_overlay.c" "${LINDBERGH_RUNTIME_DIR}/xr_overlay_geometry.c" "${LINDBERGH_RUNTIME_DIR}/xr_menu.c")
                target_compile_definitions(${target} PRIVATE LINDBERGH_OPENXR)
                target_link_libraries(${target} PRIVATE OpenXR::openxr_loader)
            endif()
        endif()
        if(LINDBERGH_LINUX)
            # Native Linux, e.g. Steam Frame (SteamOS, AArch64) in the Steam
            # Linux Runtime: OpenAL Soft and, with Vulkan, the OpenXR host.
            find_package(OpenAL REQUIRED)
            if(TARGET OpenAL::OpenAL)      # OpenAL Soft's own CMake package
                target_link_libraries(${target} PRIVATE OpenAL::OpenAL)
            else()                         # CMake's FindOpenAL
                target_include_directories(${target} PRIVATE ${OPENAL_INCLUDE_DIR})
                target_link_libraries(${target} PRIVATE ${OPENAL_LIBRARY})
            endif()
            target_link_libraries(${target} PRIVATE dl m pthread)
            # loads on 4 KB and 16 KB page kernels alike; the main thread's
            # stack comes from RLIMIT_STACK, which the launcher raises
            target_link_options(${target} PRIVATE "-Wl,-z,max-page-size=16384")
            if(LINDBERGH_RENDERER STREQUAL "VULKAN")
                if(LINDBERGH_OPENXR)
                    target_compile_definitions(${target} PRIVATE LINDBERGH_OPENXR LINDBERGH_VULKAN_XR)
                    target_link_libraries(${target} PRIVATE lindbergh_vulkan_openxr)
                endif()
                foreach(header IN LISTS LINDBERGH_GL_HEADERS)
                    get_filename_component(gl_dir "${header}" DIRECTORY)
                    target_include_directories(${target} PRIVATE "${gl_dir}/..")
                endforeach()
            endif()
            # printf and friends are handed the guest's own format strings
            target_compile_options(${target} PRIVATE -Wno-format-security)
        endif()
        if(APPLE)
            target_link_libraries(${target} PRIVATE "-framework AppKit" "-framework OpenAL" objc)
            if(NOT LINDBERGH_RENDERER STREQUAL "VULKAN")
                target_link_libraries(${target} PRIVATE "-framework OpenGL")
            endif()
            # the menu bar (Controls, Aim, Sound, Language)
            target_sources(${target} PRIVATE "${LINDBERGH_RUNTIME_DIR}/host_menu_mac.m")
            set_source_files_properties("${LINDBERGH_RUNTIME_DIR}/host_menu_mac.m"
                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
            # Every guest call is a host call: give the main thread room.
            target_link_options(${target} PRIVATE "LINKER:-stack_size,0x10000000")
        endif()
    endif()
endfunction()

# Self-check: builds the runtime against stub generated headers and exercises
# the parts that do not need a game - the dispatch table and the syscall layer.
# guest_load() is covered by the first real title.
if(NOT ANDROID)     # it runs on the build machine
    enable_testing()
    add_executable(lindbergh_rt_selftest tests/selftest/selftest.c tests/selftest/runtime_abis.c)
    lindbergh_runtime(lindbergh_rt_selftest "${CMAKE_CURRENT_SOURCE_DIR}/tests/selftest")
    add_test(NAME lindbergh_rt_selftest COMMAND lindbergh_rt_selftest)
endif()
