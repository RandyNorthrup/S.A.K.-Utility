# ThreadSanitizer harness (R5-G23-1).
#
# WHY THIS EXISTS SEPARATELY
# TSan does not exist on any Windows toolchain -- MSVC has none and clang-cl does not implement
# -fsanitize=thread -- so the main Windows build can never produce a TSan binary. It IS reachable
# through the WSL archlinux the repo already uses for apfsck: clang + qt6-base are installed
# there, and the cross-platform half of this codebase compiles against them.
#
# SCOPE, stated honestly: this builds the PORTABLE, THREADED units and their existing tests. It
# is not the whole application -- most workers include <windows.h> and cannot compile here at
# all. What it buys is real race DETECTION on the units that can be built, instead of the manual
# audit that was standing in for it. Units are added one at a time, each with the test that
# actually exercises its threading; a unit whose test is single-threaded proves nothing and is
# not worth a row here.
#
# Not wired into the Windows build or CI. Driven by scripts/run_tsan_wsl.ps1.

cmake_minimum_required(VERSION 3.21)
project(sak_tsan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Test)

get_filename_component(SAK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# -O1 with frame pointers: TSan needs optimization low enough to keep stacks readable, and
# -fno-omit-frame-pointer is what makes the reported stack usable rather than a list of
# addresses. halt_on_error is set at RUN time (see the runner) so a race fails the run.
set(TSAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g -O1)

# One row per portable threaded unit: target name, the production source, and the test that
# drives its threads.
function(sak_add_tsan_test target production_source test_source)
    add_executable(${target} "${SAK_ROOT}/${production_source}" "${SAK_ROOT}/${test_source}")
    target_include_directories(${target} PRIVATE "${SAK_ROOT}/include" "${SAK_ROOT}/src")
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Test)
    target_compile_options(${target} PRIVATE ${TSAN_FLAGS})
    target_link_options(${target} PRIVATE ${TSAN_FLAGS})
endfunction()

# sak::logger -- a Meyer's singleton written to from several threads at once. Its test spawns
# four threads x 25 messages specifically to prove the writes do not interleave, which makes it
# the one unit whose existing test is already a race exerciser.
sak_add_tsan_test(tsan_logger src/core/logger.cpp tests/unit/test_logger.cpp)
