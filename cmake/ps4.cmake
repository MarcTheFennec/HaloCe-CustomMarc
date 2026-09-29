# PS4 (OpenOrbis / GoldHEN) helper functions. Include after project() in a
# build configured with cmake/toolchain-ps4.cmake.
#
#   ps4_add_executable(<target> SOURCES <files...> [LIBRARIES <libs...>])
#       An OpenOrbis ELF linked with link.x, OpenOrbis musl (libc) and
#       libkernel plus the listed system libraries (libScePad or ScePad...).
#       When the toolchain has no prebuilt stubs (source layout), link stubs
#       are generated from ps4libdoc by tools/ps4_stub_libs.py.
#   ps4_create_fself(<target> [OUTPUT <eboot.bin>] [PAID <hex>] [AUTHINFO <hex>])
#       Target <target>_fself: create-fself turns the ELF into a fake-signed
#       SELF (eboot.bin). Defaults: <binary dir>/eboot.bin, a normal
#       application's PAID and auth info.
#   ps4_create_pkg(<name> FSELF <target> [OUTPUT_DIR <dir>] [PIC0 <png>])
#       Target <name>: tools/ps4_package.py stages eboot.bin, sce_sys/param.sfo
#       and sce_sys/icon0.png, writes halo.gp4 and runs LibOrbisPkg's
#       PkgTool.Core when available (PKGTOOL or PATH). Never packs game data.

if(NOT PS4)
  message(FATAL_ERROR "cmake/ps4.cmake needs -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ps4.cmake")
endif()

get_filename_component(PS4_REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# the OpenOrbis samples' default (Piglet apps need 0x3800000000000035 + auth info)
set(PS4_DEFAULT_PAID 0x3800000000000011)

function(_ps4_library_name out lib)
  string(REGEX REPLACE "^lib" "" name "${lib}")
  set(${out} "${name}" PARENT_SCOPE)
endfunction()

function(ps4_add_executable target)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LIBRARIES")
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "ps4_add_executable(${target}): no SOURCES")
  endif()
  add_executable(${target} ${ARG_SOURCES})

  set(libs kernel)
  foreach(lib IN LISTS ARG_LIBRARIES)
    _ps4_library_name(name "${lib}")
    if(NOT name STREQUAL "kernel" AND NOT name STREQUAL "c")
      list(APPEND libs "${name}")
    endif()
  endforeach()

  if(PS4_LIBDOC_DIR)
    set(stub_dir "${CMAKE_BINARY_DIR}/ps4_stubs/${target}")
    set(stub_files "")
    set(stub_names "")
    foreach(name IN LISTS libs)
      list(APPEND stub_files "${stub_dir}/lib${name}.so")
      list(APPEND stub_names "lib${name}")
    endforeach()
    add_custom_command(
      OUTPUT ${stub_files}
      COMMAND "${Python3_EXECUTABLE}" "${PS4_REPO_DIR}/tools/ps4_stub_libs.py"
              --cc "${PS4_CLANG}" --ld "${PS4_LLD}" "${PS4_LIBDOC_DIR}" "${stub_dir}" ${stub_names}
      DEPENDS "${PS4_REPO_DIR}/tools/ps4_stub_libs.py"
      COMMENT "PS4 link stubs for ${target}"
      VERBATIM)
    add_custom_target(${target}_stubs DEPENDS ${stub_files})
    add_dependencies(${target} ${target}_stubs)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${stub_files})
  else()
    set(stub_dir "${PS4_LIB_DIR}")
  endif()
  set_target_properties(${target} PROPERTIES PS4_STUB_DIR "${stub_dir}")
  target_link_options(${target} PRIVATE "-L${stub_dir}")
  # musl first: its system calls resolve against libkernel
  target_link_libraries(${target} PRIVATE c ${libs})
endfunction()

function(ps4_create_fself target)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "OUTPUT;PAID;AUTHINFO" "")
  if(NOT PS4_CREATE_FSELF)
    message(FATAL_ERROR "create-fself not found (OpenOrbis bin/linux or PATH)")
  endif()
  if(NOT ARG_OUTPUT)
    set(ARG_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin")
  endif()
  if(NOT ARG_PAID)
    set(ARG_PAID ${PS4_DEFAULT_PAID})
  endif()
  set(auth "")
  if(ARG_AUTHINFO)
    set(auth --authinfo ${ARG_AUTHINFO})
  endif()
  get_filename_component(out_dir "${ARG_OUTPUT}" DIRECTORY)
  get_filename_component(out_name "${ARG_OUTPUT}" NAME_WE)
  set(oelf "${out_dir}/${out_name}.oelf")
  add_custom_command(
    OUTPUT "${ARG_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E env "OO_PS4_TOOLCHAIN=${PS4_OO_SOURCE_DIR}"
            "${PS4_CREATE_FSELF}" "-in=$<TARGET_FILE:${target}>" "-out=${oelf}"
            --eboot "${ARG_OUTPUT}" --paid ${ARG_PAID} ${auth}
            --library-path "$<TARGET_PROPERTY:${target},PS4_STUB_DIR>"
    DEPENDS ${target}
    COMMENT "PS4 FSELF ${ARG_OUTPUT}"
    VERBATIM)
  add_custom_target(${target}_fself ALL DEPENDS "${ARG_OUTPUT}")
  set_target_properties(${target}_fself PROPERTIES PS4_FSELF_FILE "${ARG_OUTPUT}")
endfunction()

function(ps4_create_pkg name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "FSELF;OUTPUT_DIR;PIC0" "")
  if(NOT ARG_FSELF)
    message(FATAL_ERROR "ps4_create_pkg(${name}): FSELF <target> is required")
  endif()
  if(NOT ARG_OUTPUT_DIR)
    set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/package")
  endif()
  set(pic0 "")
  if(ARG_PIC0)
    set(pic0 --pic0 "${ARG_PIC0}")
  endif()
  add_custom_target(${name}
    COMMAND "${Python3_EXECUTABLE}" "${PS4_REPO_DIR}/tools/ps4_package.py"
            --eboot "$<TARGET_PROPERTY:${ARG_FSELF}_fself,PS4_FSELF_FILE>"
            --out "${ARG_OUTPUT_DIR}" ${pic0}
    DEPENDS ${ARG_FSELF}_fself
    COMMENT "PS4 PKG (launcher only) in ${ARG_OUTPUT_DIR}"
    VERBATIM)
endfunction()
