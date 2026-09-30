#!/usr/bin/env python3
"""Builds the vendored libsm64 (``port/third_party/libsm64``) into the shared
library the port loads at run time.

The native port builds call this through ``ninja`` (see
``tools/libsm64_build.py``), so that libsm64 is compiled with the same
compiler as the port and with the flags its target needs.  You can also run it
by hand::

    python3 port/libsm64_port/tools/build_libsm64.py --out /tmp/libsm64.so

Two things libsm64 needs are not in this repository:

* Mario's geometry (``src/decomp/mario/geo.inc.c`` and ``model.inc.c``), which
  upstream fetches at build time with ``import-mario-geo.py``.  When
  ``--cache DIR`` (or ``$HALO_SM64_GEO_CACHE``) names a directory holding the
  two unmodified files, this script rewrites them offline
  (``offline_import_geo.py``); otherwise upstream's script runs, which needs
  network access to ``raw.githubusercontent.com``.
* A Super Mario 64 US ROM, which libsm64 reads at *run* time.  The user
  supplies it; see ``port/libsm64_port/README_LIBSM64_PORT.md``.

CC0 1.0 Universal, like the rest of the port.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SOURCE = Path("port/third_party/libsm64")
DEFAULT_CACHE_DIRS = (
    Path.home() / ".cache" / "halo-sm64-port" / "mario-geo",
    HERE.parent / "cache",
)

# The files upstream generates before it compiles anything.
IMPORTED = (
    Path("src") / "decomp" / "mario" / "geo.inc.c",
    Path("src") / "decomp" / "mario" / "geo.inc.h",
    Path("src") / "decomp" / "mario" / "model.inc.c",
    Path("src") / "decomp" / "mario" / "model.inc.h",
)

# What the Makefile produces, by system.
LIBRARY_NAMES = ("libsm64.so", "sm64.dll", "libsm64.dylib")

# libsm64's own source directories, from its Makefile.
SOURCE_DIRS = (
    "src", "src/decomp", "src/decomp/engine", "src/decomp/include/PR",
    "src/decomp/game", "src/decomp/pc", "src/decomp/pc/audio",
    "src/decomp/mario", "src/decomp/tools", "src/decomp/audio",
)

# libsm64's own CFLAGS (its Makefile sets them with `:=`, so an override
# replaces rather than extends them: ours are added to this list, not to a
# variable that does not exist).
UPSTREAM_CFLAGS = (
    "-fno-strict-aliasing -g -Wall -Wno-unused-function -fPIC"
    " -fvisibility=hidden -DSM64_LIB_EXPORT -DGBI_FLOATS -DVERSION_US"
    " -DNO_SEGMENTED_MEMORY"
)


def default_ldflags(cc: str) -> str:
    """what libsm64 links with, by what it is being built for: the Microsoft
    target has no --as-needed, and its math functions are in the C runtime"""
    if "windows" in cc or "windows" in (os.environ.get("HALO_SM64_TARGET") or ""):
        return "-shared"
    return "-Wl,--no-as-needed -lm -shared"


def compile_without_make(source: Path, cc: str, cflags: str, ldflags: str,
                         output: Path, jobs: int) -> None:
    """libsm64's own build is a Makefile; where there is no make (a Windows
    runner, say), compile the same sources the same way with this port's
    compiler instead."""
    from concurrent.futures import ThreadPoolExecutor

    # every command below runs with libsm64's tree as its working directory,
    # so the paths have to be absolute
    source = source.resolve()
    build = source / "build" / "direct"
    build.mkdir(parents=True, exist_ok=True)

    sources = []
    seen = set()
    for directory in SOURCE_DIRS:
        for path in sorted((source / directory).glob("*.c")):
            if path.resolve() not in seen:
                seen.add(path.resolve())
                sources.append(path)
    # the generated geometry, which src/decomp/mario also holds once it exists
    for path in IMPORTED:
        if path.suffix == ".c" and (source / path).is_file() and (source / path).resolve() not in seen:
            seen.add((source / path).resolve())
            sources.append(source / path)
    if not sources:
        sys.exit("build_libsm64: no sources under %s" % source)

    include = source / "src" / "decomp" / "include"
    objects = []

    def compile_one(path: Path) -> Path:
        object_file = build / (str(path.relative_to(source)).replace(os.sep, "_") + ".o")
        run([cc, *cflags.split(), "-I", include, "-c", path, "-o", object_file], source)
        return object_file

    if jobs > 1:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            objects = list(pool.map(compile_one, sources))
    else:
        objects = [compile_one(path) for path in sources]

    output.parent.mkdir(parents=True, exist_ok=True)
    run([cc, *ldflags.split(), *objects, "-o", output], source)


def run_optional(command) -> bool:
    """one subprocess that is allowed to fail: returns whether it worked"""
    printable = " ".join(str(part) for part in command)
    print("build_libsm64: %s" % printable)
    try:
        return subprocess.run([str(part) for part in command]).returncode == 0
    except OSError as error:
        print("build_libsm64: %s (%s)" % (printable, error))
        return False


def run(command, cwd):
    """one subprocess, with its output going straight to ours"""
    printable = " ".join(str(part) for part in command)
    print("build_libsm64: %s" % printable)
    result = subprocess.run([str(part) for part in command], cwd=str(cwd))
    if result.returncode != 0:
        sys.exit("build_libsm64: %s failed with %d" % (printable, result.returncode))


def find_cache(cache_argument):
    """the directory of unmodified geo.inc.c and model.inc.c, or None"""
    candidates = []
    if cache_argument:
        candidates.append(Path(cache_argument))
    if os.environ.get("HALO_SM64_GEO_CACHE"):
        candidates.append(Path(os.environ["HALO_SM64_GEO_CACHE"]))
    candidates.extend(DEFAULT_CACHE_DIRS)
    for directory in candidates:
        if (directory / "geo.inc.c").is_file() and (directory / "model.inc.c").is_file():
            return directory
    return None


def import_geometry(source, cache):
    """the offline rewrite when there is a cache, and a cache fetched from the
    decompilation when there is not: upstream's own script is the last resort
    (it needs raw.githubusercontent.com)"""
    if all((source / path).is_file() for path in IMPORTED):
        return

    if cache is None:
        # the first cache directory we can write to, which the next build reuses
        cache = DEFAULT_CACHE_DIRS[0]
        if run_optional([sys.executable, HERE / "offline_import_geo.py", "--fetch", cache]):
            cache = Path(cache)
        else:
            cache = None

    if cache is not None:
        run([sys.executable, HERE / "offline_import_geo.py", source.resolve(), cache.resolve()],
            source)
        return

    script = source / "import-mario-geo.py"
    if not script.is_file():
        sys.exit(
            "build_libsm64: %s is missing, and Mario's geometry could neither be\n"
            "fetched nor found in a cache; see\n"
            "port/libsm64_port/README_LIBSM64_PORT.md" % script
        )
    print("build_libsm64: fetching Mario's geometry (import-mario-geo.py)")
    run([sys.executable, script], source)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE,
                        help="libsm64's source tree (default: %(default)s)")
    parser.add_argument("--cc", default=None,
                        help="the compiler to build it with (default: cc)")
    parser.add_argument("--cflags", default="",
                        help="extra compiler flags, on top of libsm64's own")
    parser.add_argument("--ldflags", default="",
                        help="extra linker flags, on top of libsm64's own")
    parser.add_argument("--jobs", type=int, default=0,
                        help="how many compiles to run at once (default: all cores)")
    parser.add_argument("--cache", default=None,
                        help="a directory of unmodified geo.inc.c and model.inc.c, "
                             "for building without network access")
    parser.add_argument("--no-make", action="store_true",
                        help="compile the sources directly instead of running libsm64's "
                             "Makefile, as on a machine with no make")
    parser.add_argument("--out", type=Path, required=True,
                        help="where to copy the finished library")
    arguments = parser.parse_args()

    source = arguments.source
    if not (source / "Makefile").is_file():
        sys.exit("build_libsm64: no libsm64 at %s" % source)

    import_geometry(source, find_cache(arguments.cache))

    # libsm64's Makefile links with `$(CC) $(LDFLAGS)`, and its CFLAGS are
    # fixed; ours are added, with -lm kept last so that the shared library
    # keeps its libm symbols (sqrtf, and the rest).
    ldflags = arguments.ldflags or default_ldflags(arguments.cc or "")
    if arguments.no_make or shutil.which("make") is None:
        compile_without_make(source, arguments.cc or "cc",
                             "%s %s" % (UPSTREAM_CFLAGS, arguments.cflags),
                             ldflags, arguments.out, arguments.jobs or (os.cpu_count() or 4))
        header = source / "src" / "libsm64.h"
        if header.is_file():
            shutil.copyfile(header, arguments.out.with_suffix(".h"))
        print("build_libsm64: %s" % arguments.out)
        return 0

    command = ["make", "-C", str(source), "lib"]
    if arguments.cc:
        command.append("CC=%s" % arguments.cc)
    if arguments.cflags:
        command.append("CFLAGS=%s %s" % (UPSTREAM_CFLAGS, arguments.cflags))
    if ldflags:
        command.append("LDFLAGS=%s" % ldflags)
    if arguments.jobs:
        command.append("-j%d" % arguments.jobs)
    run(command, ".")

    dist = source / "dist"
    built = [dist / name for name in LIBRARY_NAMES if (dist / name).is_file()]
    if not built:
        sys.exit("build_libsm64: %s holds no library" % dist)
    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(built[0], arguments.out)
    header = source / "dist" / "include" / "libsm64.h"
    if header.is_file():
        shutil.copyfile(header, arguments.out.with_suffix(".h"))
    print("build_libsm64: %s" % arguments.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
