#!/bin/sh
# Build the PS4 (GoldHEN) launcher: configure -> compile -> link ->
# create-fself -> create-pkg.
#
#   ./build-ps4.sh [--toolchain DIR] [--build-dir DIR] [--no-pkg] [--require-pkg]
#
#   --toolchain DIR  OpenOrbis toolchain (default $OO_PS4_TOOLCHAIN,
#                    $PS4_OPENORBIS or /opt/oo; tools/ps4_setup_toolchain.sh
#                    builds one from source)
#   --build-dir DIR  CMake build directory (default build/ps4/cmake)
#   --no-pkg         stop after eboot.bin
#   --require-pkg    fail if the .pkg cannot be built (no PkgTool.Core)
#
# Outputs: <build-dir>/eboot.bin and <build-dir>/package/ (pkg_root/,
# halo.gp4, IV0000-HCEP02342_00-HALOCECUSTOMMARC.pkg when PkgTool.Core is
# available). The package never contains game data: copy your maps to
# /data/halo on the console (BUILDING-PS4.md).
set -eu
cd "$(dirname "$0")"

build_dir=build/ps4/cmake
pkg=1
require_pkg=0
while [ $# -gt 0 ]; do
    case "$1" in
        --toolchain) OO_PS4_TOOLCHAIN=$2; export OO_PS4_TOOLCHAIN; shift 2 ;;
        --build-dir) build_dir=$2; shift 2 ;;
        --no-pkg) pkg=0; shift ;;
        --require-pkg) require_pkg=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "build-ps4.sh: unknown option $1" >&2; exit 1 ;;
    esac
done

generator=
command -v ninja >/dev/null 2>&1 && generator="-G Ninja"
toolchain_arg=
[ -n "${OO_PS4_TOOLCHAIN:-}" ] && toolchain_arg="-DOO_PS4_TOOLCHAIN=$OO_PS4_TOOLCHAIN"

echo "== configure"
# shellcheck disable=SC2086
cmake -S port/ps4 -B "$build_dir" $generator -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-ps4.cmake" $toolchain_arg
echo "== compile, link, create-fself"
cmake --build "$build_dir"
echo "   $build_dir/eboot.bin"

[ "$pkg" = 1 ] || exit 0
echo "== create-pkg"
status=0
python3 tools/ps4_package.py --eboot "$build_dir/eboot.bin" --out "$build_dir/package" || status=$?
if [ "$status" = 2 ] && [ "$require_pkg" = 0 ]; then
    echo "build-ps4.sh: eboot.bin and the staged package are ready; the .pkg needs PkgTool.Core" >&2
    exit 0
fi
exit "$status"
