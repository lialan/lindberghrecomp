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
# The build must be 32-bit. The CPU model in pcrecomp/runtime/recomp32_cpu is
# flat - a guest register holds a real host address - and a Lindbergh ELF is
# linked at 0x08048000, which only exists as an address in a 32-bit process.

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
    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            message(FATAL_ERROR
                "${target} is being built 64-bit. On Windows the recompiled code holds "
                "guest addresses in host registers and the game maps at 0x08048000, "
                "so the host must be 32-bit: cmake -B build -A Win32")
        endif()
        set(platform_sources
            "${LINDBERGH_RUNTIME_DIR}/hle_window.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_gl.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_cg.c")
    else()
        # A 64-bit POSIX host (macOS, Android): guest addresses are offsets into
        # one 4 GB window (guest_mem.c), and GL goes through typed thunks.
        set(platform_sources
            "${LINDBERGH_RUNTIME_DIR}/guest_mem.c"
            "${LINDBERGH_RUNTIME_DIR}/host_sdl.c"
            "${LINDBERGH_RUNTIME_DIR}/host_settings.c"
            "${LINDBERGH_RUNTIME_DIR}/game_profile.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_gl_posix.c"
            "${LINDBERGH_RUNTIME_DIR}/arbprog.c"
            "${LINDBERGH_RUNTIME_DIR}/glsl_pp.c"
            "${LINDBERGH_RUNTIME_DIR}/stereo.c"
            "${LINDBERGH_RUNTIME_DIR}/gl_thread.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_openal.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_glx_posix.c"
            "${LINDBERGH_RUNTIME_DIR}/hle_cg.c"
            "${LINDBERGH_RUNTIME_DIR}/hud_widen.c"
            "${LINDBERGH_RUNTIME_DIR}/solib.c")
        target_compile_definitions(${target} PRIVATE RECOMP_GUEST_BASE=1)
    endif()
    target_sources(${target} PRIVATE
        "${LINDBERGH_RUNTIME_DIR}/guest.c"
        "${LINDBERGH_RUNTIME_DIR}/syscall.c"
        "${LINDBERGH_RUNTIME_DIR}/hle.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_libc.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_pthread.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_vidmode.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_matrix.c"
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
        target_link_libraries(${target} PRIVATE SDL2::SDL2 z)
        if(ANDROID)
            # Meta Quest: GL 2.1 through gl4es on GLES 3, OpenAL Soft, and
            # the headset through OpenXR (host_xr.c). Paths to the three are
            # given at configure time - see docs/QUEST.md.
            foreach(v GL4ES_DIR OPENAL_DIR OPENXR_DIR)
                if(NOT ${v})
                    message(FATAL_ERROR "Android build: set -D${v}=... (docs/QUEST.md)")
                endif()
            endforeach()
            target_sources(${target} PRIVATE "${LINDBERGH_RUNTIME_DIR}/host_xr.c"
                "${LINDBERGH_RUNTIME_DIR}/xr_overlay.c" "${LINDBERGH_RUNTIME_DIR}/xr_menu.c")
            target_compile_definitions(${target} PRIVATE LINDBERGH_GL4ES)
            # printf and friends are handed the guest's own format strings
            target_compile_options(${target} PRIVATE -Wno-format-security)
            target_include_directories(${target} PRIVATE
                "${GL4ES_DIR}/include" "${OPENAL_DIR}/include" "${OPENXR_DIR}/include")
            target_link_libraries(${target} PRIVATE
                "${GL4ES_LIB}" "${OPENAL_LIB}" "${OPENXR_LIB}" EGL log android dl m)
            target_link_options(${target} PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,--no-undefined")
        endif()
        if(APPLE)
            target_link_libraries(${target} PRIVATE "-framework OpenGL" "-framework AppKit" "-framework OpenAL" objc)
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
    add_executable(lindbergh_rt_selftest tests/selftest/selftest.c)
    lindbergh_runtime(lindbergh_rt_selftest "${CMAKE_CURRENT_SOURCE_DIR}/tests/selftest")
    add_test(NAME lindbergh_rt_selftest COMMAND lindbergh_rt_selftest)
endif()
