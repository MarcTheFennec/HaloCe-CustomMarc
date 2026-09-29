# PS4 launcher (eboot.bin) through CMake with the OpenOrbis toolchain.
#
#   cmake -S port/ps4 -B build/ps4/cmake -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ps4.cmake
#   cmake --build build/ps4/cmake             # eboot.bin
#   cmake --build build/ps4/cmake -t halo_pkg # staged package + .pkg
#
# (build-ps4.sh runs these steps.) The game itself is the x32 guest image
# (build/ps4/halo_guest.elf), built by the repository's configure.py/ninja
# build like the other platforms; this project builds the native PS4 host
# around it and embeds it. Game data is never packed; see BUILDING-PS4.md.

cmake_minimum_required(VERSION 3.16)
get_filename_component(HALO_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${HALO_REPO}/cmake/toolchain-ps4.cmake")
endif()
project(halo_ps4 C ASM)
include("${HALO_REPO}/cmake/ps4.cmake")

set(HALO_BUILD "${HALO_REPO}/build/ps4")
set(HALO_GUEST_IMAGE "${HALO_BUILD}/halo_guest.elf")
set(HALO_HOST_GEN "${HALO_BUILD}/host_gen")
set(HALO_GENERATED "${HALO_HOST_GEN}/host_import_table.c" "${HALO_HOST_GEN}/host_gl_dispatch.c")

# the guest image and the host tables generated from it come from the
# repository's ninja build (configured for PS4 on first use)
find_program(HALO_NINJA ninja REQUIRED)
add_custom_target(halo_guest
  COMMAND sh -c "grep -q ps4_guest build.ninja 2>/dev/null || \"$0\" configure.py --ps4" "${Python3_EXECUTABLE}"
  COMMAND "${HALO_NINJA}" build/ps4/halo_guest.elf build/ps4/host_gen/host_import_table.c
          build/ps4/host_gen/host_gl_dispatch.c
  BYPRODUCTS "${HALO_GUEST_IMAGE}" ${HALO_GENERATED}
  WORKING_DIRECTORY "${HALO_REPO}"
  COMMENT "PS4 guest image (repository ninja build)"
  USES_TERMINAL
  VERBATIM)

set(HOST_DIR "${HALO_REPO}/port/ps4/host")
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS "${HOST_DIR}/*.c")
list(REMOVE_ITEM HOST_SOURCES "${HOST_DIR}/host_platform_null.c")
set(POSIX_SOURCES "${HALO_REPO}/port/linux/src/posix_files.c" "${HALO_REPO}/port/linux/src/posix_net.c")

ps4_add_executable(halo_eboot
  SOURCES ${HOST_SOURCES} ${POSIX_SOURCES}
          "${HALO_REPO}/port/third_party/tomlc17/tomlc17.c"
          ${HALO_GENERATED}
          "${HOST_DIR}/host_image_blob.S"
  # keep in sync with EBOOT_LIBRARIES in tools/ps4_build.py
  LIBRARIES libScePad libSceAudioOut libSceUserService libSceSystemService libSceNet)
add_dependencies(halo_eboot halo_guest)
set_source_files_properties(${HALO_GENERATED} PROPERTIES GENERATED TRUE)

target_compile_options(halo_eboot PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-O2 -g -Wall -Wno-unused-function>)
target_compile_definitions(halo_eboot PRIVATE
  $<$<COMPILE_LANGUAGE:C>:_GNU_SOURCE SDL_BYTEORDER=1234>)
# EGL/KHR come from the OpenOrbis headers (Piglet's native window type), so
# only the OpenGL registry's GLES 3 headers are added
target_include_directories(halo_eboot PRIVATE
  "${HOST_DIR}" "${HALO_REPO}/port/ps4/include" "${HALO_HOST_GEN}" "${HALO_REPO}/port/linux/src"
  "${HALO_REPO}/port/third_party/tomlc17" "${HALO_BUILD}/third_party/SDL3/include"
  "${HALO_BUILD}/third_party/khronos/OpenGL-Registry/api")
set_source_files_properties(${POSIX_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${HOST_DIR}/host_fs_redirect.h"
  OBJECT_DEPENDS "${HOST_DIR}/host_fs_redirect.h")
set_source_files_properties("${HOST_DIR}/host_image_blob.S" PROPERTIES
  COMPILE_DEFINITIONS "HALO_GUEST_IMAGE=\"${HALO_GUEST_IMAGE}\""
  OBJECT_DEPENDS "${HALO_GUEST_IMAGE}")

# Piglet (OpenGL ES) needs a system application's PAID and auth info: the
# values of OpenOrbis' piglet sample (as tools/ps4_build.py)
ps4_create_fself(halo_eboot
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin"
  PAID 0x3800000000000035
  AUTHINFO 000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000)

ps4_create_pkg(halo_pkg FSELF halo_eboot OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/package")
