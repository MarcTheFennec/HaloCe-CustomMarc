"""Ninja rules for the PS4 build (``ninja ps4`` and ``ninja ps4_pkg``).

The PS4 port (port/ps4/PLAN.md) runs the game as x32 code - x86-64
instructions with 32-bit pointers, as the game's data formats require -
inside a 64-bit OpenOrbis homebrew application. This graph builds

- the host, build/ps4/eboot.bin: port/ps4/host with the OpenOrbis PS4
  Toolchain (clang for x86_64-pc-freebsd12-elf, ld.lld with the
  toolchain's link.x, then create-fself), and
- the package, build/ps4/pkg/<content id>.pkg: eboot.bin, param.sfo,
  icon0.png and the toolchain's open-source support modules (libc.prx,
  libSceFios2.prx), with PkgTool.Core.

Milestone 0 builds the host alone. The guest image (the game as x32 code,
M1) will be added to the package as a plain file the host loads at
start-up, as the Android build stages halo_guest.elf as an asset.

The toolchain is found in OO_PS4_TOOLCHAIN or given with
``configure.py --ps4-toolchain``. Without one, the graph has no PS4 rules,
like the Android build without an NDK.

Like the Linux build, this is independent of the byte-matching graph.
"""

import os
import shutil
import sys
from pathlib import Path
from typing import Any, List, Optional

from .ninja_syntax import Writer

PORT_DIR = Path("port/ps4")
BUILD = Path("build/ps4")

TITLE = "Halo: Combat Evolved"
TITLE_ID = "HALO00001"
CONTENT_ID = f"IV0000-{TITLE_ID}_00-HALOCE0000000000"
VERSION = "01.00"
# the process's authority information, as the OpenOrbis samples
PAID = "0x3800000000000035"
AUTHINFO = ("000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040"
            "000000000000008000000000000000080040FFFF000000F0000000000000000000000000000000000000000000000000"
            "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")

# the libraries the host imports; each is a stub in <toolchain>/lib
HOST_LIBRARIES = [
    "c", "kernel", "SceSysmodule", "SceSystemService", "SceUserService", "ScePad",
    "SceAudioOut", "SceVideoOut", "SceNet", "SceNetCtl", "SceCommonDialog", "SceMsgDialog",
]

HOST_CFLAGS = [
    "--target=x86_64-pc-freebsd12-elf",
    "-fPIC",
    "-funwind-tables",
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    # the toolchain's own headers trip these
    "-Wno-missing-declarations",
    "-Wno-visibility",
    "-D_GNU_SOURCE",
]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def find_toolchain(explicit: Optional[str]) -> Optional[Path]:
    """The OpenOrbis toolchain root: the option, then OO_PS4_TOOLCHAIN."""
    for candidate in (explicit, os.environ.get("OO_PS4_TOOLCHAIN")):
        if candidate and (Path(candidate) / "link.x").is_file():
            return Path(candidate)
    return None


def _toolchain_bin(toolchain: Path) -> Path:
    if sys.platform.startswith("win"):
        return toolchain / "bin" / "windows"
    if sys.platform == "darwin":
        return toolchain / "bin" / "macos"
    return toolchain / "bin" / "linux"


def _support_module(toolchain: Path, name: str) -> Optional[Path]:
    """One of the toolchain's open-source support modules (built from its
    src/modules): bin/data/modules in the release tarball, else the first
    sample that has it (the source checkout stages them with every sample)."""
    candidate = toolchain / "bin" / "data" / "modules" / name
    if candidate.is_file():
        return candidate
    samples = toolchain / "samples"
    if not samples.is_dir():
        return None
    for sample in sorted(samples.iterdir()):
        for folder in ("sce_module", "sce_sys/about"):
            candidate = sample / folder / name
            if candidate.is_file():
                return candidate
    return None


def sfo_entries(version: str = VERSION) -> List[List[str]]:
    """param.sfo, as PkgTool.Core sfo_setentry arguments."""
    return [
        ["APP_TYPE", "--type", "Integer", "--maxsize", "4", "--value", "1"],
        ["APP_VER", "--type", "Utf8", "--maxsize", "8", "--value", version],
        ["ATTRIBUTE", "--type", "Integer", "--maxsize", "4", "--value", "0"],
        ["CATEGORY", "--type", "Utf8", "--maxsize", "4", "--value", "gd"],
        ["CONTENT_ID", "--type", "Utf8", "--maxsize", "48", "--value", CONTENT_ID],
        ["DOWNLOAD_DATA_SIZE", "--type", "Integer", "--maxsize", "4", "--value", "0"],
        ["SYSTEM_VER", "--type", "Integer", "--maxsize", "4", "--value", "0"],
        ["TITLE", "--type", "Utf8", "--maxsize", "128", "--value", TITLE],
        ["TITLE_ID", "--type", "Utf8", "--maxsize", "12", "--value", TITLE_ID],
        ["VERSION", "--type", "Utf8", "--maxsize", "8", "--value", version],
    ]


def gp4_text(files: List[str]) -> str:
    """A PkgTool project listing the files, whose paths are relative to the
    project file and are also the paths inside the package."""
    directories = sorted({str(Path(file).parent).replace(os.sep, "/") for file in files} - {"."})
    tree: dict = {}
    for directory in directories:
        node = tree
        for part in directory.split("/"):
            node = node.setdefault(part, {})

    def render(node: dict, depth: int) -> str:
        indent = "\t" * depth
        text = ""
        for name, children in node.items():
            if children:
                text += f'{indent}<dir targ_name="{name}">\n{render(children, depth + 1)}{indent}</dir>\n'
            else:
                text += f'{indent}<dir targ_name="{name}" />\n'
        return text

    file_lines = "".join(f'\t\t<file targ_path="{file}" orig_path="{file}" />\n' for file in files)
    return (
        '<?xml version="1.0"?>\n'
        '<psproject xmlns:xsd="http://www.w3.org/2001/XMLSchema" '
        'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" fmt="gp4" version="1000">\n'
        "\t<volume>\n"
        "\t\t<volume_type>pkg_ps4_app</volume_type>\n"
        "\t\t<volume_id>PS4VOLUME</volume_id>\n"
        "\t\t<volume_ts>2026-01-01 00:00:00</volume_ts>\n"
        f'\t\t<package content_id="{CONTENT_ID}" passcode="00000000000000000000000000000000" '
        'storage_type="digital50" app_type="full" />\n'
        '\t\t<chunk_info chunk_count="1" scenario_count="1">\n'
        "\t\t\t<chunks>\n"
        '\t\t\t\t<chunk id="0" layer_no="0" label="Chunk #0" />\n'
        "\t\t\t</chunks>\n"
        '\t\t\t<scenarios default_id="0">\n'
        '\t\t\t\t<scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>\n'
        "\t\t\t</scenarios>\n"
        "\t\t</chunk_info>\n"
        "\t</volume>\n"
        f'\t<files img_no="0">\n{file_lines}\t</files>\n'
        f"\t<rootdir>\n{render(tree, 2)}\t</rootdir>\n"
        "</psproject>\n"
    )


def config_header_text(version: str, log_address: str, release: bool) -> str:
    def c_string(text: str) -> str:
        return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'

    return (
        "/* generated by tools/ps4_build.py at configure time */\n"
        "#ifndef __HALO_PS4_HOST_CONFIG_H\n"
        "#define __HALO_PS4_HOST_CONFIG_H\n"
        f"#define HALO_PS4_VERSION {c_string(version)}\n"
        f"#define HALO_PS4_LOG_ADDRESS {c_string(log_address)}\n"
        f"#define HALO_PS4_RELEASE {1 if release else 0}\n"
        "#endif\n"
    )


def write_config_header(path: Path, version: str, log_address: str, release: bool) -> None:
    """Writes the header only when its text changes, so ninja does not
    rebuild the host after every configure."""
    text = config_header_text(version, log_address, release)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def write_gp4(path: Path, files: List[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(gp4_text(files), encoding="utf-8")


def ps4_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "host", PORT_DIR / "include", PORT_DIR / "pkg"]


def generate_ps4_build(n: Writer, sln: Any) -> None:
    if not (PORT_DIR / "host").is_dir():
        return
    toolchain = find_toolchain(getattr(sln, "ps4_toolchain", None))
    if not toolchain:
        n.comment("PS4 build: no OpenOrbis toolchain (set OO_PS4_TOOLCHAIN or pass --ps4-toolchain)")
        return
    tools = _toolchain_bin(toolchain)
    cc = getattr(sln, "ps4_cc", None) or "clang"
    ld = getattr(sln, "ps4_ld", None) or "ld.lld"
    launcher = getattr(sln, "compiler_launcher", None)
    release = bool(getattr(sln, "port_release", False))
    version = os.environ.get("HALO_BUILD_NUMBER", "")
    log_address = getattr(sln, "ps4_log_address", None) or ""

    host_dir = PORT_DIR / "host"
    obj_dir = BUILD / "host" / "obj"
    gen_dir = BUILD / "host" / "gen"
    elf = BUILD / "host" / "halo_host.elf"
    oelf = BUILD / "host" / "halo_host.oelf"
    eboot = BUILD / "eboot.bin"
    pkg_root = BUILD / "pkg_root"
    pkg_dir = BUILD / "pkg"
    pkg = pkg_dir / f"{CONTENT_ID}.pkg"
    gp4 = pkg_root / "halo.gp4"
    sfo = pkg_root / "sce_sys" / "param.sfo"
    icon = pkg_root / "sce_sys" / "icon0.png"

    n.comment("PS4 build (ninja ps4, ninja ps4_pkg); see port/ps4/README.md")
    n.variable("ps4_toolchain", _quote(toolchain))
    n.variable("ps4_cc", (f"{launcher} " if launcher else "") + _quote(cc))
    n.variable("ps4_ld", _quote(ld))
    n.variable("ps4_tools", _quote(tools))

    # ---------- the host

    # the build's settings, as a header (strings survive no shell quoting)
    config_header = gen_dir / "host_config.h"
    write_config_header(config_header, version or "dev", log_address, release)

    cflags = HOST_CFLAGS + [
        f"-isysroot {_quote(toolchain)}",
        f"-isystem {_quote(toolchain / 'include')}",
        f"-I{PORT_DIR / 'include'}",
        f"-I{host_dir}",
        f"-I{gen_dir}",
        "-O2" if release else "-O1",
        "-g",
    ]
    if release:
        cflags.append("-DNDEBUG")
    n.rule(
        name="ps4_host_cc",
        command="$ps4_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="PS4 HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    objects: List[Path] = []
    for source in sorted(host_dir.glob("*.c")):
        obj = obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="ps4_host_cc", inputs=source, implicit=[config_header],
                variables={"cflags": " ".join(cflags)})
        objects.append(obj)

    n.rule(
        name="ps4_host_link",
        command=(f"$ps4_ld -m elf_x86_64 -pie --script {_quote(toolchain / 'link.x')} --eh-frame-hdr "
                 f"-L{_quote(toolchain / 'lib')} -o $out $in "
                 + " ".join(f"-l{lib}" for lib in HOST_LIBRARIES)
                 + f" {_quote(toolchain / 'lib' / 'crt1.o')}"),
        description="PS4 HOST LINK $out",
    )
    n.build(outputs=elf, rule="ps4_host_link", inputs=objects, implicit=[toolchain / "link.x"])

    n.rule(
        name="ps4_fself",
        command=(f"$ps4_tools/create-fself -in=$in -out={oelf} --eboot $out "
                 f"--paid {PAID} --authinfo {AUTHINFO}"),
        description="PS4 FSELF $out",
    )
    n.build(outputs=eboot, rule="ps4_fself", inputs=elf, implicit_outputs=[oelf])
    n.build(outputs="ps4", rule="phony", inputs=eboot)

    # ---------- the package

    n.rule(name="ps4_copy", command="cp $in $out", description="PS4 STAGE $out")
    staged: List[Path] = []
    staged_eboot = pkg_root / "eboot.bin"
    n.build(outputs=staged_eboot, rule="ps4_copy", inputs=eboot)
    staged.append(staged_eboot)
    n.build(outputs=icon, rule="ps4_copy", inputs=PORT_DIR / "pkg" / "sce_sys" / "icon0.png")
    staged.append(icon)
    for name, folder in (("libc.prx", "sce_module"), ("libSceFios2.prx", "sce_module"), ("right.sprx", "sce_sys/about")):
        source = _support_module(toolchain, name)
        if not source:
            # libc.prx is the process's C library: without it the package
            # does not start. right.sprx only backs the "about" screen.
            level = "warning" if name == "right.sprx" else "error"
            print(f"PS4 build {level}: {name} not found in the toolchain (bin/data/modules or samples/*/{folder}); "
                  "the package goes without it", file=sys.stderr)
            continue
        target = pkg_root / folder / name
        n.build(outputs=target, rule="ps4_copy", inputs=source)
        staged.append(target)

    sfo_command = " && ".join(
        [f"$ps4_tools/PkgTool.Core sfo_new $out"]
        + [f"$ps4_tools/PkgTool.Core sfo_setentry $out " + " ".join(_quote(part) for part in entry)
           for entry in sfo_entries()]
    )
    n.rule(name="ps4_sfo", command=sfo_command, description="PS4 SFO $out")
    n.build(outputs=sfo, rule="ps4_sfo", implicit=[Path(__file__)])
    staged.append(sfo)

    # the project file lists the staged files by their paths in the package
    write_gp4(gp4, sorted(str(path.relative_to(pkg_root)).replace(os.sep, "/") for path in staged))

    n.rule(
        name="ps4_pkg",
        command=f"rm -rf {pkg_dir} && mkdir -p {pkg_dir} && $ps4_tools/PkgTool.Core pkg_build $in {pkg_dir}",
        description="PS4 PKG $out",
    )
    n.build(outputs=pkg, rule="ps4_pkg", inputs=gp4, implicit=staged)
    n.build(outputs="ps4_pkg", rule="phony", inputs=pkg)
    n.newline()


def clean_stage(root: Path = BUILD) -> None:
    """Removes the staging folders (for tests)."""
    if root.exists():
        shutil.rmtree(root)
