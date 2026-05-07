#
# Multikernel-AMP vdtu_ring library
#
# Vendored from semperos-sel4-ref/{components/include,src}/vdtu_ring.{h,c}
# with ARMv8 acquire/release atomics in place of compiler barriers.
#

cmake_minimum_required(VERSION 3.8.2)

project(multikernel_vdtu C)

add_library(multikernel_vdtu STATIC EXCLUDE_FROM_ALL
    src/vdtu_ring.c
    src/mht.c
    src/threadmgr.c
    src/captable.c
    src/kernelcall.c
    src/revocations.c
    src/vpe.c
    src/syscall.c
)

target_include_directories(multikernel_vdtu PUBLIC include)
target_link_libraries(multikernel_vdtu PRIVATE muslc)
