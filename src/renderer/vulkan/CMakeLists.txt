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
add_library(lindbergh_vulkan STATIC device.cpp frame_ring.cpp renderer.cpp ${shaders})
target_include_directories(lindbergh_vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(lindbergh_vulkan PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(lindbergh_vulkan PUBLIC Vulkan::Vulkan)
target_compile_options(lindbergh_vulkan PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

include(CTest)
if(BUILD_TESTING)
    add_executable(lindbergh_vulkan_selftest ../../../tests/vulkan/selftest.cpp)
    target_link_libraries(lindbergh_vulkan_selftest PRIVATE lindbergh_vulkan)
    add_test(NAME lindbergh_vulkan_selftest COMMAND lindbergh_vulkan_selftest)
endif()
