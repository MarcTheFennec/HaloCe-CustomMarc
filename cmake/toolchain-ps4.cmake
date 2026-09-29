# CMake toolchain file for PS4 homebrew (GoldHEN) with the OpenOrbis toolchain.
#
#   cmake -S port/ps4 -B build/ps4/cmake -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ps4.cmake \
#         [-DOO_PS4_TOOLCHAIN=/path/to/OpenOrbis]
#
# OO_PS4_TOOLCHAIN (cache variable, else the environment variable of the same
# name, else $PS4_OPENORBIS, else /opt/oo) may have either layout:
#   - an OpenOrbis-PS4-Toolchain release: include/, lib/ (libc.a, crt1.o and
#     system library stubs), link.x, bin/linux/create-fself;
#   - the layout tools/ps4_setup_toolchain.sh builds from source: src/ (the
#     toolchain repository: include/, link.x), sysroot/ (OpenOrbis musl:
#     include/, lib/libc.a, lib/crt1.o), ps4libdoc/ (stubs are generated
#     from it), and create-fself on PATH.
# Compiler and linker: clang and ld.lld from PATH (or PS4_CLANG / PS4_LLD).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(PS4 TRUE)
set(PS4_TARGET_TRIPLE x86_64-pc-freebsd12-elf)

if(NOT OO_PS4_TOOLCHAIN)
  if(DEFINED ENV{OO_PS4_TOOLCHAIN})
    set(OO_PS4_TOOLCHAIN "$ENV{OO_PS4_TOOLCHAIN}")
  elseif(DEFINED ENV{PS4_OPENORBIS})
    set(OO_PS4_TOOLCHAIN "$ENV{PS4_OPENORBIS}")
  else()
    set(OO_PS4_TOOLCHAIN /opt/oo)
  endif()
endif()
set(OO_PS4_TOOLCHAIN "${OO_PS4_TOOLCHAIN}" CACHE PATH "OpenOrbis toolchain directory")
# try_compile projects re-read this file; pass the directory on
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES OO_PS4_TOOLCHAIN)

if(EXISTS "${OO_PS4_TOOLCHAIN}/src/link.x")
  set(PS4_OO_SOURCE_DIR "${OO_PS4_TOOLCHAIN}/src")
  set(PS4_LINKER_SCRIPT "${OO_PS4_TOOLCHAIN}/src/link.x")
  set(PS4_SYSTEM_INCLUDES "${OO_PS4_TOOLCHAIN}/sysroot/include" "${OO_PS4_TOOLCHAIN}/src/include")
  set(PS4_LIB_DIR "${OO_PS4_TOOLCHAIN}/sysroot/lib")
  set(PS4_LIBDOC_DIR "${OO_PS4_TOOLCHAIN}/ps4libdoc")
elseif(EXISTS "${OO_PS4_TOOLCHAIN}/link.x")
  set(PS4_OO_SOURCE_DIR "${OO_PS4_TOOLCHAIN}")
  set(PS4_LINKER_SCRIPT "${OO_PS4_TOOLCHAIN}/link.x")
  set(PS4_SYSTEM_INCLUDES "${OO_PS4_TOOLCHAIN}/include")
  set(PS4_LIB_DIR "${OO_PS4_TOOLCHAIN}/lib")
  set(PS4_LIBDOC_DIR "")
else()
  message(FATAL_ERROR "OO_PS4_TOOLCHAIN=${OO_PS4_TOOLCHAIN} is not an OpenOrbis toolchain "
                      "(no link.x or src/link.x); run tools/ps4_setup_toolchain.sh or "
                      "pass -DOO_PS4_TOOLCHAIN=")
endif()

if(DEFINED ENV{PS4_CLANG})
  set(PS4_CLANG "$ENV{PS4_CLANG}")
else()
  find_program(PS4_CLANG NAMES clang clang-18 clang-17 clang-16 clang-15 REQUIRED)
endif()
if(DEFINED ENV{PS4_LLD})
  set(PS4_LLD "$ENV{PS4_LLD}")
else()
  find_program(PS4_LLD NAMES ld.lld ld.lld-18 ld.lld-17 ld.lld-16 ld.lld-15 REQUIRED)
endif()
find_program(PS4_CREATE_FSELF NAMES create-fself
             HINTS "${OO_PS4_TOOLCHAIN}/bin/linux" "${OO_PS4_TOOLCHAIN}/bin/macos"
                   "${OO_PS4_TOOLCHAIN}/create-fself")

set(CMAKE_C_COMPILER "${PS4_CLANG}")
set(CMAKE_ASM_COMPILER "${PS4_CLANG}")
set(CMAKE_C_COMPILER_TARGET ${PS4_TARGET_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${PS4_TARGET_TRIPLE})
set(CMAKE_LINKER "${PS4_LLD}")
# the compiler cannot link a test program without link.x and crt1.o
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

execute_process(COMMAND "${PS4_CLANG}" --target=${PS4_TARGET_TRIPLE} -print-resource-dir
                OUTPUT_VARIABLE PS4_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)

set(_ps4_includes "")
foreach(dir IN LISTS PS4_SYSTEM_INCLUDES)
  string(APPEND _ps4_includes " -isystem \"${dir}\"")
endforeach()
set(_ps4_flags "-D__ORBIS__ -D__PS4__ -fPIC -funwind-tables -nostdinc${_ps4_includes} -isystem \"${PS4_CLANG_RESOURCE_DIR}/include\"")
set(CMAKE_C_FLAGS_INIT "${_ps4_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_ps4_flags}")

# Executables are linked by ld.lld directly with OpenOrbis' link.x; crt1.o
# comes last as in the OpenOrbis samples. Libraries are named without the
# "lib" prefix (target_link_libraries(x kernel ScePad)); ps4_add_executable
# adds the -L paths for the system library stubs.
set(CMAKE_C_LINK_EXECUTABLE
    "\"${PS4_LLD}\" -m elf_x86_64 -pie --script \"${PS4_LINKER_SCRIPT}\" --eh-frame-hdr <LINK_FLAGS> -o <TARGET> <OBJECTS> -L\"${PS4_LIB_DIR}\" <LINK_LIBRARIES> \"${PS4_LIB_DIR}/crt1.o\"")
set(CMAKE_C_LINK_LIBRARY_FLAG "-l")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

set(CMAKE_FIND_ROOT_PATH ${PS4_LIB_DIR} ${PS4_SYSTEM_INCLUDES})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
