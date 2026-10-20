# NV2A GPU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Register handlers: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC, PFIFO, PGRAPH
# MMIO interception via VEH (Vectored Exception Handling)
# Push buffer parsing and command dispatch

# OT_HEADLESS (macOS bring-up): the Direct3D 11 translator and the files that
# hang off it (HD textures, button icons, the live push-buffer driver that
# creates a host D3D8 device) are replaced by the null backend in ../d3d.
set(NV2A_SOURCES
    nv2a_core.c
    nv2a_vsh_cpu.c
    nv2a_psh.c
    nv2a_pb_replay.c
    nv2a_mmio_hook.c
    nv2a_pb_test.c
)
if(NOT OT_HEADLESS)
    list(APPEND NV2A_SOURCES nv2a_pgraph_d3d11.c nv2a_hdtex.c nv2a_btnicons.c nv2a_live_pb.c)
endif()
add_library(xbox_nv2a STATIC ${NV2A_SOURCES})

target_include_directories(xbox_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_nv2a PRIVATE
        /W3
        /wd4244
        /wd4146
        /wd4267
    )
    target_compile_definitions(xbox_nv2a PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(xbox_nv2a PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_link_libraries(xbox_nv2a PRIVATE platform)
# nv2a_live_pb.c creates the host D3D8 device the translator draws into
target_link_libraries(xbox_nv2a PUBLIC xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_nv2a PRIVATE kernel32)
endif()
