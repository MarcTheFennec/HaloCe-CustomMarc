#!/usr/bin/env python3
"""Stage and package the PS4 launcher as a fake PKG.

The package holds only the launcher: eboot.bin plus sce_sys/param.sfo and
sce_sys/icon0.png (and optionally pic0.png).  Game content is never
packed; it is read at run time from /data/halo (see BUILDING-PS4.md).

Steps:
  1. stage   <out>/pkg_root/{eboot.bin, sce_sys/param.sfo, sce_sys/icon0.png}
  2. gp4     <out>/halo.gp4 (same layout as OpenOrbis create-gp4 output)
  3. pkg     LibOrbisPkg's "PkgTool.Core pkg_build halo.gp4 <out>" if the
             tool is available ($PKGTOOL, or PkgTool.Core on PATH); otherwise
             prints how to finish the build.

param.sfo is written here directly: the format is small and public, and
this avoids needing .NET just to produce it.

Usage:
  ps4_package.py --eboot build/ps4/eboot.bin --out build/ps4/package
                 [--assets-dir DIR] [--pic0 FILE] [--pkgtool PATH]
                 [--no-pkg]
  ps4_package.py --dump-sfo FILE
"""

import argparse
import datetime
import os
import shutil
import struct
import subprocess
import sys

TITLE = "Halo CE CustomMarc"
TITLE_ID = "HCEP02342"
# IV0000-<TITLE_ID>_00-<16 characters>
CONTENT_ID = "IV0000-" + TITLE_ID + "_00-HALOCECUSTOMMARC"
APP_VER = "01.00"
VERSION = "01.00"

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCE_SYS_DIR = os.path.join(REPO, "port", "ps4", "sce_sys")

# Extensions of Halo content files; refusing them keeps copyrighted data out
# of the package even when --assets-dir is used.
GAME_DATA_EXTENSIONS = (".map", ".xbe", ".xmv", ".wma", ".xwb", ".xsb")

SFO_MAGIC = 0x46535000  # "\0PSF"
SFO_VERSION = 0x0101
SFO_FMT_UTF8 = 0x0204
SFO_FMT_INT32 = 0x0404


def sfo_entries():
    """(key, value, max_len); ints are int32 entries."""
    return [
        ("APP_TYPE", 1, 4),
        ("APP_VER", APP_VER, 8),
        ("ATTRIBUTE", 0, 4),
        ("CATEGORY", "gde", 4),
        ("CONTENT_ID", CONTENT_ID, 48),
        ("DOWNLOAD_DATA_SIZE", 0, 4),
        ("FORMAT", "obs", 4),
        ("SYSTEM_VER", 1020, 4),
        ("TITLE", TITLE, 128),
        ("TITLE_ID", TITLE_ID, 12),
        ("VERSION", VERSION, 8),
    ]


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def build_sfo(entries):
    entries = sorted(entries, key=lambda e: e[0])
    keys = b""
    key_offsets = []
    for key, _, _ in entries:
        key_offsets.append(len(keys))
        keys += key.encode("ascii") + b"\0"
    keys += b"\0" * (align(len(keys), 4) - len(keys))

    index = b""
    data = b""
    for (key, value, max_len), key_offset in zip(entries, key_offsets):
        if isinstance(value, int):
            raw = struct.pack("<I", value & 0xFFFFFFFF)
            fmt = SFO_FMT_INT32
            max_len = 4
        else:
            raw = value.encode("utf-8") + b"\0"
            fmt = SFO_FMT_UTF8
            if len(raw) > max_len:
                raise ValueError("%s is longer than %d bytes" % (key, max_len))
        index += struct.pack("<HHIII", key_offset, fmt, len(raw), max_len,
                             len(data))
        data += raw + b"\0" * (max_len - len(raw))

    header_size = 20
    key_table_start = header_size + len(index)
    data_table_start = key_table_start + len(keys)
    header = struct.pack("<IIIII", SFO_MAGIC, SFO_VERSION, key_table_start,
                         data_table_start, len(entries))
    return header + index + keys + data


def parse_sfo(blob):
    magic, version, key_start, data_start, count = struct.unpack_from(
        "<IIIII", blob, 0)
    if magic != SFO_MAGIC:
        raise ValueError("not a param.sfo (magic %08x)" % magic)
    result = {}
    for i in range(count):
        key_offset, fmt, length, max_len, data_offset = struct.unpack_from(
            "<HHIII", blob, 20 + 16 * i)
        key_end = blob.index(b"\0", key_start + key_offset)
        key = blob[key_start + key_offset:key_end].decode("ascii")
        raw = blob[data_start + data_offset:data_start + data_offset + length]
        if fmt == SFO_FMT_INT32:
            result[key] = struct.unpack("<I", raw)[0]
        else:
            result[key] = raw.rstrip(b"\0").decode("utf-8")
    return version, result


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)
    return struct.unpack(">II", head[16:24])


def build_gp4(content_id, files, orig_prefix=""):
    """Mirror OpenOrbis create-gp4's output for a list of relative paths.
    orig_path is resolved by PkgTool relative to the .gp4's directory."""
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    file_tags = "\n".join(
        '\t\t<file targ_path="%s" orig_path="%s%s" />' % (f, orig_prefix, f)
        for f in files)

    tree = {}
    for f in files:
        parts = f.split("/")[:-1]
        node = tree
        for part in parts:
            node = node.setdefault(part, {})

    def dirs(node, depth):
        out = []
        for name in sorted(node):
            pad = "\t" * depth
            if node[name]:
                out.append('%s<dir targ_name="%s">' % (pad, name))
                out.extend(dirs(node[name], depth + 1))
                out.append("%s</dir>" % pad)
            else:
                out.append('%s<dir targ_name="%s"></dir>' % (pad, name))
        return out

    rootdir = "\n".join(["\t<rootdir>"] + dirs(tree, 2) + ["\t</rootdir>"])
    return ("<?xml version=\"1.0\"?>\n"
            "<psproject xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
            "fmt=\"gp4\" version=\"1000\">\n"
            "\t<volume>\n"
            "\t\t<volume_type>pkg_ps4_app</volume_type>\n"
            "\t\t<volume_id>PS4VOLUME</volume_id>\n"
            "\t\t<volume_ts>%s</volume_ts>\n"
            "\t\t<package content_id=\"%s\" "
            "passcode=\"00000000000000000000000000000000\"\n"
            "\t\t\tstorage_type=\"digital50\" app_type=\"full\" />\n"
            "\t\t<chunk_info chunk_count=\"1\" scenario_count=\"1\">\n"
            "\t\t\t<chunks>\n"
            "\t\t\t\t<chunk id=\"0\" layer_no=\"0\" label=\"Chunk #0\" />\n"
            "\t\t\t</chunks>\n"
            "\t\t\t<scenarios default_id=\"0\">\n"
            "\t\t\t\t<scenario id=\"0\" type=\"sp\" initial_chunk_count=\"1\" "
            "label=\"Scenario #0\">0</scenario>\n"
            "\t\t\t</scenarios>\n"
            "\t\t</chunk_info>\n"
            "\t</volume>\n"
            "\t<files img_no=\"0\">\n"
            "%s\n"
            "\t</files>\n"
            "%s\n"
            "</psproject>\n") % (timestamp, content_id, file_tags, rootdir)


def find_pkgtool(explicit):
    for candidate in (explicit, os.environ.get("PKGTOOL")):
        if candidate:
            return candidate
    for name in ("PkgTool.Core", "PkgTool.Core.exe", "PkgTool"):
        path = shutil.which(name)
        if path:
            return path
    dll = shutil.which("PkgTool.Core.dll")
    if dll and shutil.which("dotnet"):
        return "dotnet " + dll
    return None


def stage(args):
    root = os.path.join(args.out, "pkg_root")
    if os.path.isdir(root):
        shutil.rmtree(root)
    os.makedirs(os.path.join(root, "sce_sys"))

    files = []

    def add(src, rel):
        dst = os.path.join(root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        files.append(rel)

    if not os.path.isfile(args.eboot):
        sys.exit("error: %s not found (run: ninja ps4_eboot)" % args.eboot)
    with open(args.eboot, "rb") as f:
        if f.read(4) != b"\x4f\x15\x3d\x1d":
            sys.exit("error: %s is not a SELF (bad magic)" % args.eboot)
    add(args.eboot, "eboot.bin")

    sfo_path = os.path.join(root, "sce_sys", "param.sfo")
    with open(sfo_path, "wb") as f:
        f.write(build_sfo(sfo_entries()))
    files.append("sce_sys/param.sfo")

    icon = os.path.join(SCE_SYS_DIR, "icon0.png")
    if png_size(icon) != (512, 512):
        sys.exit("error: icon0.png must be 512x512")
    add(icon, "sce_sys/icon0.png")

    if args.pic0:
        if png_size(args.pic0) != (1920, 1080):
            sys.exit("error: pic0.png must be 1920x1080")
        add(args.pic0, "sce_sys/pic0.png")

    if args.assets_dir:
        for dirpath, _, names in os.walk(args.assets_dir):
            for name in sorted(names):
                src = os.path.join(dirpath, name)
                if name.lower().endswith(GAME_DATA_EXTENSIONS):
                    sys.exit("error: refusing to pack game data file %s; game "
                             "content belongs in /data/halo on the console"
                             % src)
                rel = os.path.relpath(src, args.assets_dir).replace(os.sep, "/")
                add(src, "extra/" + rel)

    gp4_path = os.path.join(args.out, "halo.gp4")
    with open(gp4_path, "w") as f:
        f.write(build_gp4(CONTENT_ID, files, "pkg_root/"))
    return root, gp4_path, files


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--eboot", default="build/ps4/eboot.bin")
    parser.add_argument("--out", default="build/ps4/package")
    parser.add_argument("--assets-dir", help="extra launcher-only files "
                        "(never game data); off by default")
    parser.add_argument("--pic0", help="optional 1920x1080 background PNG")
    parser.add_argument("--pkgtool", help="PkgTool.Core command")
    parser.add_argument("--no-pkg", action="store_true",
                        help="stage and write the gp4 only")
    parser.add_argument("--dump-sfo", metavar="FILE")
    args = parser.parse_args()

    if args.dump_sfo:
        with open(args.dump_sfo, "rb") as f:
            version, values = parse_sfo(f.read())
        print("param.sfo version %x" % version)
        for key in sorted(values):
            print("  %-20s %r" % (key, values[key]))
        return 0

    os.makedirs(args.out, exist_ok=True)
    root, gp4_path, files = stage(args)
    print("staged %d files in %s" % (len(files), root))
    for rel in files:
        print("  %s" % rel)
    print("wrote %s" % gp4_path)

    if args.no_pkg:
        return 0
    pkgtool = find_pkgtool(args.pkgtool)
    pkg_name = CONTENT_ID + ".pkg"
    if not pkgtool:
        print("\nPkgTool.Core (LibOrbisPkg) was not found, so the .pkg itself was "
              "not built.\nInstall it (see BUILDING-PS4.md), then either rerun "
              "with PKGTOOL=/path/to/PkgTool.Core\nor run:\n"
              "  cd %s && PkgTool.Core pkg_build halo.gp4 .\n"
              "The result is %s." % (os.path.abspath(args.out), pkg_name))
        return 2
    command = pkgtool.split() + ["pkg_build", "halo.gp4", "."]
    print("running: %s" % " ".join(command))
    work = os.path.abspath(args.out)
    subprocess.check_call(command, cwd=work)
    pkg_path = os.path.join(work, pkg_name)
    if not os.path.isfile(pkg_path):
        sys.exit("error: PkgTool finished but %s is missing" % pkg_path)
    with open(pkg_path, "rb") as f:
        if f.read(4) != b"\x7fCNT":
            sys.exit("error: %s does not start with the PKG magic" % pkg_path)
    print("built %s (%d bytes)" % (pkg_path, os.path.getsize(pkg_path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
