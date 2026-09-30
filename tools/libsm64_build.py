"""Ninja rules shared by the native ports for the libsm64 module in
``port/libsm64_port``.

A port that supports the module builds two things:

* **the module's units** (``port/libsm64_port/src/*.c``), compiled as the
  game's own sources are, because the game side calls the game's functions and
  includes its headers; and
* **libsm64 itself** (``port/third_party/libsm64``), a shared library the
  module opens at run time rather than links, built by
  ``port/libsm64_port/tools/build_libsm64.py`` with the port's own compiler.

Both exist only when the configuration asks for them (``configure.py
--enable-libsm64``); without it, no port compiles a line of this and the game
is exactly what it was.

Android is not supported: its guest is a static binary with a bundled libc
that has no dynamic loader, so it cannot open libsm64. See DECISIONS.md.

CC0 1.0 Universal, like the rest of the port.
"""

import json
import os
import re
from pathlib import Path
from typing import Any, List, Optional

MODULE_DIR = Path("port/libsm64_port")
MODULE_SOURCES = MODULE_DIR / "src"
MODULE_INCLUDE = MODULE_DIR / "include"
LIBSM64_DIR = Path("port/third_party/libsm64")
BUILD_TOOL = MODULE_DIR / "tools" / "build_libsm64.py"
OFFLINE_IMPORT_TOOL = MODULE_DIR / "tools" / "offline_import_geo.py"

# The module's own sources: the portable core, and the game side.
SOURCES = [
    MODULE_SOURCES / "sm64_port_core.c",
    MODULE_SOURCES / "sm64_port_halo.c",
]


def _quote(path: Any) -> str:
    """a path as it has to appear in a ninja command line (linux_build.py
    quotes the game's own include directories the same way: "source/saved
    films" holds a space)"""
    text = str(path).replace(os.sep, "/")
    return '"-I' + text + '"' if " " in text else "-I" + text


def enabled(sln: Any) -> bool:
    """was this configuration made with configure.py --enable-libsm64?"""
    return bool(getattr(sln, "enable_libsm64", False))


def game_include_dirs(sln: Any) -> List[str]:
    """the -I flags the game's own units get, which the module needs too: the
    game's headers include each other by bare name (data.h includes
    tag_files.h) and are reached from the module through port/libsm64_port"""
    config_path = Path(getattr(sln, "config_dir") or "config") / "config.json"
    try:
        config = json.loads(re.sub(r"//.*", "", config_path.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        return ["-Isource", "-Isource/cseries"]

    directories = []
    for project in config.get("projects", []):
        for directory in (project.get("options") or {}).get("include_dirs") or []:
            if directory != "xbox/include" and directory not in directories:
                directories.append(directory)
    return [_quote(directory) for directory in sorted(directories)]


# The ABI libsm64 has to be compiled and linked with, by port. The game is
# 32-bit, so the library it opens has to be too, whatever the machine that
# builds it is: a 64-bit library simply cannot be loaded.
LIBRARY_ABI = {
    "linux": "--target=i686-linux-gnu -m32",
    "windows": "--target=i686-pc-windows-msvc",
}


def library_cflags(platform: str) -> str:
    """what libsm64's own sources are compiled with for this port"""
    return "%s -O2 -fPIC" % LIBRARY_ABI[platform]


def library_ldflags(platform: str) -> str:
    """what it is linked with: the same ABI, and libm where there is one"""
    if platform == "windows":
        return "%s -shared" % LIBRARY_ABI[platform]
    return "%s -Wl,--no-as-needed -lm -shared" % LIBRARY_ABI[platform]


def emit_library(n: Any, output: Path, cc: str, cflags: str, ldflags: str) -> Path:
    """builds the vendored libsm64 with this port's compiler, into the
    directory the port's executable runs from (the module looks for the
    library beside it)"""
    n.rule(
        name="libsm64_lib",
        command=("$python %s --out $out --cc \"%s\" --cflags \"$cflags\""
                 " --ldflags \"$ldflags\" --jobs 8" % (BUILD_TOOL, cc)),
        description="LIBSM64 $out",
        # Once per configuration, not once per unit: the tool decides itself
        # whether anything needs rebuilding, and its own output should stay
        # readable.
        pool="console",
    )
    n.build(
        outputs=output,
        rule="libsm64_lib",
        variables={"cflags": cflags, "ldflags": ldflags},
        implicit=[BUILD_TOOL, OFFLINE_IMPORT_TOOL, LIBSM64_DIR / "Makefile"],
    )
    n.newline()
    return output


def add_module(n: Any, sln: Any, obj_dir: Path, rule: str, abi: str,
               prefix_flags: str, extra_includes: Optional[List[str]] = None,
               implicit: Optional[List[Path]] = None) -> List[Path]:
    """compiles the module's units with the game's flags and returns the
    objects, for the port to link"""
    cflags = " ".join([
        abi,
        "-std=gnu89",
        "-D__STRICT_ANSI__",
        "-w",
        # the module's own feature flag: with it, the game's hooks are live
        "-DHALO_CE_ENABLE_LIBSM64=1",
        prefix_flags,
        *game_include_dirs(sln),
        "-I%s" % MODULE_INCLUDE,
        *(extra_includes or []),
    ])

    objects = []
    for source in SOURCES:
        output = obj_dir / ("libsm64_port_" + source.with_suffix(".o").name)
        n.build(outputs=output, rule=rule, inputs=[source], variables={"cflags": cflags},
                implicit=implicit or [])
        objects.append(output)
    n.newline()
    return objects
