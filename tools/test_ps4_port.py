"""Tests for the PS4 build tooling (port/ps4, tools/ps4_build.py)."""

import io
import re
import xml.etree.ElementTree as ElementTree
from pathlib import Path

import pytest

from tools import ps4_build
from tools.ninja_syntax import Writer

ROOT = Path(__file__).resolve().parent.parent


# ---------- the ABI header


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def defines(text: str) -> dict:
    return dict(re.findall(r"^#define (HALO_GUEST_[A-Z_]+) (0x[0-9a-fA-F]+u?|\d+u?)", text, re.M))


def test_ps4_abi_keeps_the_addresses_of_the_android_guest():
    """The image base and the Xbox window are the game's addresses; the two
    guest ports must agree on them."""
    ps4 = defines(read("port/ps4/include/halo_ps4_abi.h"))
    android = defines(read("port/android/include/halo_android_abi.h"))
    for name in ("HALO_GUEST_IMAGE_BASE", "HALO_GUEST_WINDOW_BASE", "HALO_GUEST_WINDOW_SIZE", "HALO_GUEST_MAGIC"):
        assert ps4[name] == android[name], name


def test_ps4_abi_window_lies_below_the_image_and_4gb():
    ps4 = defines(read("port/ps4/include/halo_ps4_abi.h"))
    window = int(ps4["HALO_GUEST_WINDOW_BASE"].rstrip("u"), 16)
    size = int(ps4["HALO_GUEST_WINDOW_SIZE"].rstrip("u"), 16)
    image = int(ps4["HALO_GUEST_IMAGE_BASE"].rstrip("u"), 16)
    assert window + size <= image < 0x100000000


def test_ps4_data_roots_start_with_the_default():
    text = read("port/ps4/include/halo_ps4_abi.h")
    roots = re.search(r"#define HALO_PS4_DATA_ROOTS \\\n((?:\s*\"[^\"]+\",? \\?\n?)+)", text)
    assert roots, "HALO_PS4_DATA_ROOTS"
    first = re.findall(r"\"([^\"]+)\"", roots.group(1))[0]
    assert first == "/data/halo"
    assert '#define HALO_PS4_DEFAULT_DATA_ROOT "/data/halo"' in text


# ---------- the package metadata


def test_content_id_has_the_ps4_shape():
    assert re.fullmatch(r"[A-Z]{2}\d{4}-[A-Z]{4}\d{5}_00-[A-Z0-9]{16}", ps4_build.CONTENT_ID)
    assert ps4_build.TITLE_ID in ps4_build.CONTENT_ID
    assert ps4_build.TITLE_ID == "HALO00001"


def test_sfo_entries_name_the_title_and_content():
    entries = {entry[0]: entry for entry in ps4_build.sfo_entries()}
    assert entries["TITLE_ID"][-1] == ps4_build.TITLE_ID
    assert entries["CONTENT_ID"][-1] == ps4_build.CONTENT_ID
    assert entries["CATEGORY"][-1] == "gd"
    assert re.fullmatch(r"\d\d\.\d\d", entries["APP_VER"][-1])
    for entry in entries.values():
        assert entry[1:6:2] == ["--type", "--maxsize", "--value"]


def test_gp4_lists_files_and_their_folders():
    text = ps4_build.gp4_text(["eboot.bin", "sce_sys/param.sfo", "sce_sys/about/right.sprx", "sce_module/libc.prx"])
    root = ElementTree.fromstring(text)
    package = root.find("volume/package")
    assert package is not None and package.get("content_id") == ps4_build.CONTENT_ID
    files = [file.get("targ_path") for file in root.find("files")]
    assert files == ["eboot.bin", "sce_sys/param.sfo", "sce_sys/about/right.sprx", "sce_module/libc.prx"]
    for file in root.find("files"):
        assert file.get("orig_path") == file.get("targ_path")
    directories = {directory.get("targ_name") for directory in root.iter("dir")}
    assert directories == {"sce_sys", "about", "sce_module"}
    about = root.find("rootdir/dir[@targ_name='sce_sys']/dir[@targ_name='about']")
    assert about is not None


def test_config_header_quotes_strings():
    text = ps4_build.config_header_text('1"2\\3', "10.0.0.1:9999", True)
    assert '#define HALO_PS4_VERSION "1\\"2\\\\3"' in text
    assert '#define HALO_PS4_LOG_ADDRESS "10.0.0.1:9999"' in text
    assert "#define HALO_PS4_RELEASE 1" in text


def test_config_header_is_rewritten_only_on_change(tmp_path):
    header = tmp_path / "host_config.h"
    ps4_build.write_config_header(header, "7", "", False)
    first = header.stat().st_mtime_ns
    ps4_build.write_config_header(header, "7", "", False)
    assert header.stat().st_mtime_ns == first
    ps4_build.write_config_header(header, "8", "", False)
    assert '"8"' in header.read_text()


# ---------- the build graph


class Solution:
    ps4_toolchain = None
    ps4_cc = None
    ps4_ld = None
    ps4_log_address = None
    compiler_launcher = None
    port_release = False


def fake_toolchain(root: Path) -> Path:
    (root / "lib").mkdir(parents=True)
    (root / "lib" / "crt1.o").write_bytes(b"")
    (root / "link.x").write_text("SECTIONS {}")
    (root / "bin" / "linux").mkdir(parents=True)
    for name in ("create-fself", "PkgTool.Core"):
        (root / "bin" / "linux" / name).write_bytes(b"")
    sample = root / "samples" / "hello_world"
    (sample / "sce_module").mkdir(parents=True)
    (sample / "sce_sys" / "about").mkdir(parents=True)
    (sample / "sce_module" / "libc.prx").write_bytes(b"")
    (sample / "sce_module" / "libSceFios2.prx").write_bytes(b"")
    (sample / "sce_sys" / "about" / "right.sprx").write_bytes(b"")
    return root


def generate(monkeypatch, tmp_path, **attributes) -> str:
    monkeypatch.chdir(ROOT)
    monkeypatch.setattr(ps4_build, "BUILD", tmp_path / "build")
    solution = Solution()
    for name, value in attributes.items():
        setattr(solution, name, value)
    out = io.StringIO()
    # a wide writer: the assertions look for whole build lines
    ps4_build.generate_ps4_build(Writer(out, width=10000), solution)
    return out.getvalue()


def test_no_toolchain_means_no_rules(monkeypatch, tmp_path):
    monkeypatch.delenv("OO_PS4_TOOLCHAIN", raising=False)
    text = generate(monkeypatch, tmp_path)
    assert "no OpenOrbis toolchain" in text
    assert "rule ps4_host_cc" not in text


def test_toolchain_from_the_environment(monkeypatch, tmp_path):
    toolchain = fake_toolchain(tmp_path / "toolchain")
    monkeypatch.setenv("OO_PS4_TOOLCHAIN", str(toolchain))
    text = generate(monkeypatch, tmp_path)
    assert "rule ps4_host_cc" in text
    assert "build ps4: phony" in text
    assert "build ps4_pkg: phony" in text


def test_host_graph_compiles_every_host_source_and_packages(monkeypatch, tmp_path):
    toolchain = fake_toolchain(tmp_path / "toolchain")
    monkeypatch.delenv("OO_PS4_TOOLCHAIN", raising=False)
    text = generate(monkeypatch, tmp_path, ps4_toolchain=str(toolchain), ps4_log_address="10.0.0.2:9999")
    for source in sorted((ROOT / "port/ps4/host").glob("*.c")):
        assert f"{source.name}.o: ps4_host_cc" in text, source.name
    assert "--target=x86_64-pc-freebsd12-elf" in text
    assert "-m elf_x86_64 -pie --script" in text
    assert "create-fself" in text and f"--paid {ps4_build.PAID}" in text
    assert "PkgTool.Core sfo_new" in text and "PkgTool.Core pkg_build" in text
    for library in ps4_build.HOST_LIBRARIES:
        assert f"-l{library}" in text
    # the support modules from the toolchain, staged for the package
    assert "sce_module/libc.prx: ps4_copy" in text
    assert "sce_sys/about/right.sprx: ps4_copy" in text
    # the generated header and project file exist after generation
    header = tmp_path / "build" / "host" / "gen" / "host_config.h"
    assert '"10.0.0.2:9999"' in header.read_text()
    gp4 = tmp_path / "build" / "pkg_root" / "halo.gp4"
    files = [file.get("targ_path") for file in ElementTree.parse(gp4).getroot().find("files")]
    assert "eboot.bin" in files and "sce_sys/param.sfo" in files and "sce_sys/icon0.png" in files


def test_support_modules_come_from_bin_data_first(tmp_path):
    toolchain = fake_toolchain(tmp_path / "toolchain")
    assert ps4_build._support_module(toolchain, "libc.prx") == toolchain / "samples/hello_world/sce_module/libc.prx"
    (toolchain / "bin" / "data" / "modules").mkdir(parents=True)
    (toolchain / "bin" / "data" / "modules" / "libc.prx").write_bytes(b"")
    assert ps4_build._support_module(toolchain, "libc.prx") == toolchain / "bin/data/modules/libc.prx"
    assert ps4_build._support_module(toolchain, "nothing.prx") is None


def test_release_build_defines_ndebug_and_optimises(monkeypatch, tmp_path):
    toolchain = fake_toolchain(tmp_path / "toolchain")
    text = generate(monkeypatch, tmp_path, ps4_toolchain=str(toolchain), port_release=True)
    assert "-DNDEBUG" in text and "-O2" in text
    header = tmp_path / "build" / "host" / "gen" / "host_config.h"
    assert "#define HALO_PS4_RELEASE 1" in header.read_text()


def test_compiler_launcher_wraps_the_host_compiler(monkeypatch, tmp_path):
    toolchain = fake_toolchain(tmp_path / "toolchain")
    text = generate(monkeypatch, tmp_path, ps4_toolchain=str(toolchain), compiler_launcher="ccache")
    assert re.search(r"^ps4_cc = ccache clang$", text, re.M)


# ---------- the host sources


def test_host_never_links_piglet_as_an_import():
    """A missing import stops the process before main(), without a message;
    Piglet must be loaded by path and resolved with sceKernelDlsym."""
    assert "ScePigletv2VSH" not in ps4_build.HOST_LIBRARIES
    assert "SceShaccVSH" not in ps4_build.HOST_LIBRARIES
    video = read("port/ps4/host/host_video.c")
    assert "sceKernelDlsym" in video and "sceKernelLoadStartModule" in video


def test_host_sources_include_the_ps4_icon():
    icon = ROOT / "port/ps4/pkg/sce_sys/icon0.png"
    data = icon.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    width, height = int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")
    assert (width, height) == (512, 512)


@pytest.mark.parametrize("name", ["host_main.c", "host_video.c", "host_pad.c", "host_debug.c", "host_memory.c"])
def test_host_sources_have_a_header_comment(name):
    text = read(f"port/ps4/host/{name}")
    assert text.startswith("/*\n" + name.upper())
