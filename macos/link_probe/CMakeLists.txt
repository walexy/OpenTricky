# Link probe (MACOS_PORT_PLAN.md 0.1): the xboxrecomp runtime libraries plus a
# dummy recomp_lookup() linked into one arm64 executable, without the disc image.
# Compiling alone hides missing symbols; this lists them.
#
#   cmake -S macos/link_probe -B build/link_probe -G Ninja -DOT_HEADLESS=ON
#   cmake --build build/link_probe -k 0
cmake_minimum_required(VERSION 3.20)
project(ot_link_probe C)
set(CMAKE_C_STANDARD 11)

set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl" CACHE PATH "Homebrew OpenSSL")
set(OT_HEADLESS ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../xboxrecomp ${CMAKE_BINARY_DIR}/xboxrecomp)

add_executable(ot_link_probe probe_main.c)
target_link_libraries(ot_link_probe PRIVATE xboxrecomp)

# Pull in every object of every static library, so unresolved references show up
# even though probe_main.c calls nothing.
if(APPLE)
    target_link_options(ot_link_probe PRIVATE -Wl,-all_load -Wl,-undefined,error)
endif()

# Guest-window test (0.3): the real xbox_MemoryLayoutInit on a blank XBE.
add_executable(ot_layout_test ../layout_test/layout_test.c ../layout_test/stubs.c)
target_include_directories(ot_layout_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../port/src/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../xboxrecomp/src)
target_link_libraries(ot_layout_test PRIVATE xboxrecomp)
