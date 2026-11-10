# Release (T3) check: the application's own filesystem tool manifest validation
# (src/core/partition_file_system_tool_manifest.cpp), built on its own and run against a packaged
# tree. Only .github/workflows/release.yml builds it; it is not part of the product build and never
# ships. See main.cpp for why it exists.

cmake_minimum_required(VERSION 3.28)
project(sak_filesystem_manifest_check LANGUAGES CXX)

if(NOT SAK_SOURCE_DIR)
    message(FATAL_ERROR "Pass -DSAK_SOURCE_DIR=<repository root>")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core)

add_executable(sak_filesystem_manifest_check
    main.cpp
    "${SAK_SOURCE_DIR}/src/core/partition_file_system_tool_manifest.cpp"
)
target_include_directories(sak_filesystem_manifest_check PRIVATE "${SAK_SOURCE_DIR}/include")
target_link_libraries(sak_filesystem_manifest_check PRIVATE Qt6::Core)
target_compile_definitions(sak_filesystem_manifest_check PRIVATE NOMINMAX)
if(MSVC)
    # The product build's warning policy (CMakeLists.txt), so the validator compiles here as it does there.
    target_compile_options(sak_filesystem_manifest_check PRIVATE
        /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
endif()
