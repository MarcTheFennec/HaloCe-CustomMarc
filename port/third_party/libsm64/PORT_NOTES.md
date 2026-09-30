# libsm64 in this repository

`port/third_party/libsm64` is a copy of [libsm64](https://github.com/libsm64/libsm64)
at commit `fd11813208272b4271d92bd92feb8f3fdbe61be5`, unchanged except for this
file and the removal of its build outputs. libsm64 is CC0 1.0 Universal
(`LICENSE.md`), like this port.

The copy is here, rather than a submodule, so that `configure.py
--enable-libsm64` works on a machine with no network access to fetch it.

## What is not here, and why

libsm64 needs two things that this repository must not contain:

- **Mario's geometry** (`src/decomp/mario/geo.inc.c` and `model.inc.c`), which
  upstream fetches at build time with `import-mario-geo.py`. It is generated
  here, is not committed, and is derived from a Nintendo game.
- **A Super Mario 64 US ROM** (`baserom.us.z64`), which libsm64 reads at run
  time for Mario's textures and animation data. Supply your own; see
  `port/libsm64_port/README_LIBSM64_PORT.md`.

`port/libsm64_port/tools/build_libsm64.py` runs libsm64's own import script
(network), or the offline equivalent from a local cache of the two files
(`port/libsm64_port/tools/offline_import_geo.py`).
