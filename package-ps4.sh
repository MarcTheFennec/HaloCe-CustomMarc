#!/bin/sh
# Package an existing eboot.bin as a launcher-only fake PKG.
#
#   ./package-ps4.sh [--eboot FILE] [--out DIR] [--pic0 PNG]
#                    [--pkgtool CMD] [--assets-dir DIR] [--no-pkg]
#
# Defaults: --eboot build/ps4/cmake/eboot.bin (else build/ps4/eboot.bin from
# "ninja ps4_eboot"), --out build/ps4/package. The .pkg is built by
# LibOrbisPkg's PkgTool.Core (--pkgtool, $PKGTOOL or PATH); without it the
# staged files and halo.gp4 are left for a manual "PkgTool.Core pkg_build".
#
# No game data is packed. --assets-dir is off by default and only for extra
# launcher files; Halo content files (.map, .xbe, ...) are refused. Game data
# goes to /data/halo on the console (BUILDING-PS4.md, scripts/upload-maps.sh).
set -eu
cd "$(dirname "$0")"
eboot=build/ps4/cmake/eboot.bin
[ -f "$eboot" ] || eboot=build/ps4/eboot.bin
for arg in "$@"; do
    [ "$arg" = --eboot ] && eboot=
done
if [ -n "$eboot" ]; then
    exec python3 tools/ps4_package.py --eboot "$eboot" --out build/ps4/package "$@"
fi
exec python3 tools/ps4_package.py --out build/ps4/package "$@"
