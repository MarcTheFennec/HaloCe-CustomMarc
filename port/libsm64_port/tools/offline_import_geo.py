#!/usr/bin/env python3
"""Offline equivalent of libsm64's ``import-mario-geo.py``.

Upstream libsm64 downloads Mario's geometry (``actors/mario/geo.inc.c`` and
``actors/mario/model.inc.c`` from the SM64 decompilation) at build time with
``urllib``.  That needs ``raw.githubusercontent.com``.  This script performs
the *same* rewriting, but reads the two files from a local cache directory,
which is how the build works on a machine without that access.

    python3 offline_import_geo.py --fetch <cache-dir>        # fill a cache from the decompilation
    python3 offline_import_geo.py <libsm64-dir> <cache-dir>  # rewrite a cache into libsm64

build_libsm64.py calls both: --fetch when there is no cache yet, then the
rewrite.

The cache directory must hold the unmodified ``geo.inc.c`` and ``model.inc.c``
of commit 06ec56df7f951f88da05f468cdcacecba496145a of n64decomp/sm64, which is
the commit upstream pins.  Nothing here is committed to the repository: the
geometry is fetched at build time, exactly as upstream does (see
port/libsm64_port/README_LIBSM64_PORT.md).
"""

import io
import os
import shutil
import sys
import tarfile
import urllib.request

GEO_INC_C_HEADER = """
#include "../include/sm64.h"
#include "../include/types.h"
#include "../include/geo_commands.h"
#include "../game/rendering_graph_node.h"
#include "../shim.h"
#include "../game/object_stuff.h"
#include "../game/behavior_actions.h"
#include "model.inc.h"

#define SHADOW_CIRCLE_PLAYER 99
"""

GEO_INC_C_FOOTER = """
const GeoLayout mario_geo_libsm64[] = {
   GEO_SHADOW(SHADOW_CIRCLE_PLAYER, 0xB4, 100),
   GEO_OPEN_NODE(),
      GEO_ZBUFFER(1),
      GEO_OPEN_NODE(),
         GEO_SCALE(0x00, 16384),
         GEO_OPEN_NODE(),
            GEO_ASM(0, geo_mirror_mario_backface_culling),
            GEO_ASM(0, geo_mirror_mario_set_alpha),
            GEO_BRANCH(1, mario_geo_load_body),
            GEO_ASM(1, geo_mirror_mario_backface_culling),
         GEO_CLOSE_NODE(),
      GEO_CLOSE_NODE(),
   GEO_CLOSE_NODE(),
   GEO_END(),
};

void *mario_geo_ptr = (void*)mario_geo_libsm64;

"""

GEO_INC_H = """
#pragma once

extern void *mario_geo_ptr;
"""

MODEL_INC_H = """
#pragma once

#include "../include/types.h"
#include "../include/PR/gbi.h"
"""


def rewrite_model_inc_c(model_inc_c: str) -> str:
    """Comment out the ALIGNED8 texture blobs and add libsm64's includes."""
    lines = model_inc_c.splitlines()

    skip = 0
    for i in range(len(lines)):
        if skip > 0:
            skip -= 1
            lines[i] = "//" + lines[i]
        elif lines[i].startswith("ALIGNED8 static const u8 mario_"):
            skip = 2
            lines[i] = "//" + lines[i]

    lines.insert(0, '#include "../../load_tex_data.h"')
    lines.insert(0, '#include "../../gfx_macros.h"')
    return "\n".join(lines)


# The commit libsm64 pins, and the one archive of it that can be fetched from a
# machine where raw.githubusercontent.com is blocked.
SM64_COMMIT = "06ec56df7f951f88da05f468cdcacecba496145a"
SM64_ARCHIVE = "https://codeload.github.com/n64decomp/sm64/tar.gz/" + SM64_COMMIT


def fetch(cache_dir: str) -> int:
    """fills a cache directory with the two unmodified files, by downloading
    one archive of the decompilation at the commit libsm64 pins"""
    os.makedirs(cache_dir, exist_ok=True)
    print("offline_import_geo: fetching %s" % SM64_ARCHIVE)
    with urllib.request.urlopen(SM64_ARCHIVE, timeout=300) as response:
        archive = response.read()
    found = 0
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        for member in tar.getmembers():
            for name in ("geo.inc.c", "model.inc.c"):
                if member.name.endswith("/actors/mario/" + name):
                    with open(os.path.join(cache_dir, name), "wb") as out:
                        out.write(tar.extractfile(member).read())
                    print("offline_import_geo: wrote %s" % os.path.join(cache_dir, name))
                    found += 1
    if found != 2:
        sys.stderr.write("offline_import_geo: the archive held no actors/mario geometry\n")
        return 1
    return 0


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "--fetch":
        return fetch(sys.argv[2])
    if len(sys.argv) != 3:
        sys.stderr.write("usage: offline_import_geo.py --fetch <cache-dir>\n"
                         "       offline_import_geo.py <libsm64-dir> <cache-dir>\n")
        return 2

    libsm64_dir, cache_dir = sys.argv[1], sys.argv[2]
    out_dir = os.path.join(libsm64_dir, "src", "decomp", "mario")

    for name in ("geo.inc.c", "model.inc.c"):
        path = os.path.join(cache_dir, name)
        if not os.path.isfile(path):
            sys.stderr.write("offline_import: missing %s\n" % path)
            return 1

    geo_raw = open(os.path.join(cache_dir, "geo.inc.c"), encoding="utf8").read()
    model_raw = open(os.path.join(cache_dir, "model.inc.c"), encoding="utf8").read()

    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir, exist_ok=True)

    model_lines = rewrite_model_inc_c(model_raw)
    model_inc_h = MODEL_INC_H
    for line in model_lines.splitlines():
        if line.startswith("const "):
            model_inc_h += "\nextern " + line.replace(" = {", ";")

    with open(os.path.join(out_dir, "geo.inc.c"), "w", encoding="utf8") as f:
        f.write(GEO_INC_C_HEADER + geo_raw + GEO_INC_C_FOOTER)
    with open(os.path.join(out_dir, "model.inc.c"), "w", encoding="utf8") as f:
        f.write(model_lines)
    with open(os.path.join(out_dir, "model.inc.h"), "w", encoding="utf8") as f:
        f.write(model_inc_h)
    with open(os.path.join(out_dir, "geo.inc.h"), "w", encoding="utf8") as f:
        f.write(GEO_INC_H)

    print("offline_import: wrote %s" % out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
