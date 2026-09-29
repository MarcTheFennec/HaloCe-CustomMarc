"""Ninja rules for the PS4 guest image (``ninja ps4_guest``).

The PS4 port (port/ps4/README.md, PORTING.md) follows the Android port's
design: the game needs 32-bit pointers, so it runs as ILP32 code - here x32,
x86-64 instructions with 32-bit pointers - inside an ordinary 64-bit program,
the Orbis executable built with the OpenOrbis toolchain. This graph builds

- the guest image, build/ps4/halo_guest.elf: the game sources, the platform
  layer shared with the Linux and Android ports (port/linux/src), the guest
  runtime shared with Android (port/android/guest/runtime) plus the PS4
  pieces (port/ps4/guest), with a subset of musl as its C library, all
  compiled by clang for x86_64-linux-gnux32 and linked at a fixed address
  below 4 GB;
- the host's generated sources, build/ps4/host_gen: the import table and the
  OpenGL ES entry points the host implements for the guest.

The host itself (port/ps4/host) is built with CMake and the OpenOrbis
toolchain (cmake/toolchain-ps4.cmake, build-ps4.sh), which also embeds the
image in eboot.bin and packages it. Like the Linux and Android builds, this
is independent of the byte-matching graph.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List

from .android_build import (GUEST_CODE_FLAGS, MUSL_DIRECTORIES, MUSL_EXCLUDE, MUSL_FILES, MUSL_THREAD_PREFIXES,
                            VARIADIC_PROTOTYPE_FILES)
from .linux_build import MUSL_MATH_DIR, XDK_INCLUDE, compile_launcher, musl_math_sources, xdk_headers
from .ninja_syntax import Writer

PORT_DIR = Path("port/ps4")
ANDROID_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/ps4")
THIRD_PARTY = BUILD / "third_party"
TOML_DIR = Path("port/third_party/tomlc17")
KCP_DIR = Path("port/third_party/kcp")

MUSL_VERSION = "1.2.5"
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION}"
MUSL_URL = f"https://musl.libc.org/releases/musl-{MUSL_VERSION}.tar.gz"
MUSL_SHA256 = "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"
# where musl.libc.org cannot be reached: a mirror of musl's git repository,
# checked against the release commit
MUSL_GIT_MIRROR = "https://github.com/kraj/musl.git"
MUSL_RELEASE_COMMIT = "0784374d561435f7c787a555aeab8ede699ed298"
SDL_TAG = "release-3.4.16"
SDL_DIR = THIRD_PARTY / "SDL3"
SDL_URL = "https://github.com/libsdl-org/SDL.git"
# the Khronos OpenGL ES and EGL headers (the guest compiles the renderer
# against GLES3/gl32.h; the host's emulation needs the ES 3 enumerants too)
KHRONOS_GL_URL = "https://github.com/KhronosGroup/OpenGL-Registry.git"
KHRONOS_GL_COMMIT = "1cdd228e34966dd6b95bd203e9f84faba0f371a1"
KHRONOS_EGL_URL = "https://github.com/KhronosGroup/EGL-Registry.git"
KHRONOS_EGL_COMMIT = "db3425b8246136faccb5e2782b5694960bd6edf1"
KHRONOS_DIR = THIRD_PARTY / "khronos"

# The guest ABI: x32. The platform layer's "HALO_ANDROID" code paths are the
# ones for a guest-hosted OpenGL ES build (gamepad input, fullscreen, the
# host's services), which the PS4 guest shares; HALO_PS4 marks the few places
# where the two differ (PORTING.md lists them).
GUEST_ABI_FLAGS = [
    "--target=x86_64-linux-gnux32",
    "-DHALO_ANDROID=1",
    "-DHALO_PS4=1",
    # the PS4's Jaguar cores: SSE4.2 and AVX, no AVX2
    "-march=btver2",
    "-nostdinc",
    "-fshort-wchar",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    # %fs belongs to the host's libkernel
    "-femulated-tls",
    # position-independent code, though the image is linked at a fixed
    # address: x32's absolute addressing sign-extends 32-bit displacements,
    # which only reaches the lowest 2 GB, and the image lives above it;
    # RIP-relative addressing works at any address
    "-fpie",
    # no fused multiply-add (see tools/android_build.py)
    "-ffp-contract=off",
    "-O2",
]

# beyond port/android/guest/runtime: arch-specific pieces of musl
MUSL_EXTRA_FILES = [
    "fenv/fegetexceptflag.c", "fenv/feholdexcept.c", "fenv/fesetexceptflag.c",
    "fenv/fesetround.c", "fenv/feupdateenv.c", "fenv/x32/fenv.s",
    "setjmp/x32/setjmp.s", "setjmp/x32/longjmp.s",
]

# the Android runtime files the PS4 guest shares, and its own
SHARED_RUNTIME = ["guest_memory_watch.c", "guest_sdl.c", "guest_syscall.c", "guest_thread.c"]

GL_STUBS_INTEGER_REGISTERS = 6


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _run(command: List[str], cwd: Any = None) -> None:
    subprocess.run(command, check=True, cwd=cwd)


def _fetch_musl() -> None:
    if MUSL_DIR.is_dir():
        return
    archive = THIRD_PARTY / f"musl-{MUSL_VERSION}.tar.gz"
    try:
        print(f"Downloading {MUSL_URL}")
        _run(["curl", "-sSfL", "--connect-timeout", "20", "-o", str(archive), MUSL_URL])
        import hashlib
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != MUSL_SHA256:
            raise OSError(f"musl archive checksum mismatch ({digest})")
        _run(["tar", "xzf", archive.name], cwd=THIRD_PARTY)
        archive.unlink()
        return
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"cannot download musl ({error}); cloning {MUSL_GIT_MIRROR}", file=sys.stderr)
        if archive.exists():
            archive.unlink()
    staging = THIRD_PARTY / "musl-git"
    if staging.exists():
        shutil.rmtree(staging)
    _run(["git", "clone", "-q", "--depth", "1", "--branch", f"v{MUSL_VERSION}", MUSL_GIT_MIRROR, str(staging)])
    commit = subprocess.run(["git", "-C", str(staging), "rev-parse", "HEAD"], check=True,
                            capture_output=True, text=True).stdout.strip()
    if commit != MUSL_RELEASE_COMMIT:
        raise OSError(f"musl mirror's v{MUSL_VERSION} is {commit}, not the release commit {MUSL_RELEASE_COMMIT}")
    shutil.rmtree(staging / ".git")
    staging.rename(MUSL_DIR)


def _sparse_clone(url: str, commit: str, paths: List[str], destination: Path) -> None:
    if destination.is_dir():
        return
    staging = destination.with_name(destination.name + "-staging")
    if staging.exists():
        shutil.rmtree(staging)
    _run(["git", "init", "-q", str(staging)])
    _run(["git", "-C", str(staging), "remote", "add", "origin", url])
    _run(["git", "-C", str(staging), "sparse-checkout", "set", "--no-cone", *paths])
    _run(["git", "-C", str(staging), "fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", commit])
    _run(["git", "-C", str(staging), "checkout", "-q", "FETCH_HEAD"])
    staging.rename(destination)


def fetch_third_party() -> None:
    """Download musl, SDL3 (headers only) and the Khronos headers (configure time, once)."""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    _fetch_musl()
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG}")
        _run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)])
    KHRONOS_DIR.mkdir(parents=True, exist_ok=True)
    _sparse_clone(KHRONOS_GL_URL, KHRONOS_GL_COMMIT, ["/api/GLES2/", "/api/GLES3/"], KHRONOS_DIR / "OpenGL-Registry")
    _sparse_clone(KHRONOS_EGL_URL, KHRONOS_EGL_COMMIT, ["/api/KHR/", "/api/EGL/"], KHRONOS_DIR / "EGL-Registry")


def khronos_include_dirs() -> List[Path]:
    return [KHRONOS_DIR / "OpenGL-Registry" / "api", KHRONOS_DIR / "EGL-Registry" / "api"]


def _musl_sources() -> List[Path]:
    src = MUSL_DIR / "src"
    result = set()
    for directory in MUSL_DIRECTORIES:
        for path in (src / directory).glob("*.c"):
            result.add(path)
    for name in MUSL_FILES + MUSL_EXTRA_FILES:
        result.add(src / name)
    for path in (src / "thread").glob("*.c"):
        if path.name.startswith(MUSL_THREAD_PREFIXES):
            result.add(path)
    sources = []
    for path in sorted(result):
        relative = path.relative_to(src).as_posix()
        if relative in MUSL_EXCLUDE or any(relative.startswith(e + "/") for e in MUSL_EXCLUDE):
            continue
        sources.append(path)
    return sources


def ps4_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "guest", ANDROID_DIR / "guest" / "runtime", LINUX_DIR / "src"]


def generate_ps4_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not (PORT_DIR / "guest").is_dir():
        return
    if not getattr(sln, "ps4", False):
        n.comment("PS4 guest build: disabled (pass --ps4 to configure.py)")
        return
    try:
        fetch_third_party()
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"PS4 build disabled: cannot fetch musl/SDL3/Khronos headers ({error})", file=sys.stderr)
        return
    import json
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))
    guest_cc = getattr(sln, "ps4_guest_cc", None) or "clang"
    guest_ld = getattr(sln, "ps4_guest_ld", None) or "ld.lld"
    guest_ar = getattr(sln, "ps4_guest_ar", None) or "llvm-ar"

    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    host_gen = BUILD / "host_gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    arch_override = PORT_DIR / "guest" / "libc" / "arch" / "x32"
    musl_arch = MUSL_DIR / "arch" / "x32"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = BUILD / "halo_guest.elf"
    gl2ext = KHRONOS_DIR / "OpenGL-Registry" / "api" / "GLES2" / "gl2ext.h"
    gl32 = KHRONOS_DIR / "OpenGL-Registry" / "api" / "GLES3" / "gl32.h"
    python = "$python"

    n.comment("PS4 guest build (ninja ps4_guest); see port/ps4/README.md")
    n.variable("ps4_guest_cc", guest_cc)
    n.variable("ps4_guest_ld", guest_ld)
    n.variable("ps4_guest_ar", guest_ar)

    # ---------- generated headers and sources

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name="ps4_alltypes",
        command=f"mkdir -p $$(dirname $out) && sed -f {MUSL_DIR}/tools/mkalltypes.sed $in > $out",
        description="PS4 MUSL $out",
    )
    n.build(outputs=alltypes, rule="ps4_alltypes",
            inputs=[arch_override / "bits" / "alltypes.h.in", MUSL_DIR / "include" / "alltypes.h.in"])
    n.rule(
        name="ps4_syscall_h",
        command="mkdir -p $$(dirname $out) && cp $in $out && sed -n -e s/__NR_/SYS_/p < $in >> $out",
        description="PS4 MUSL $out",
    )
    n.build(outputs=syscall_h, rule="ps4_syscall_h", inputs=musl_arch / "bits" / "syscall.h.in")
    n.rule(
        name="ps4_version_h",
        command=f"mkdir -p $$(dirname $out) && echo '#define VERSION \"{MUSL_VERSION}\"' > $out",
        description="PS4 MUSL $out",
    )
    n.build(outputs=version_h, rule="ps4_version_h")

    guest_gl_c = gen_dir / "guest_gl.c"
    gl_imports = gen_dir / "gl_imports.list"
    n.rule(
        name="ps4_gl_stubs",
        command=(f"mkdir -p {gen_dir} && {python} tools/android_gl_stubs.py --integer-registers "
                 f"{GL_STUBS_INTEGER_REGISTERS} {LINUX_DIR}/src/gl.h {gl32} {gl2ext} {guest_gl_c} {gl_imports}"),
        description="PS4 GL STUBS",
    )
    n.build(outputs=[guest_gl_c, gl_imports], rule="ps4_gl_stubs",
            implicit=[Path("tools/android_gl_stubs.py"), LINUX_DIR / "src" / "gl.h"])

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.rule(
        name="ps4_posix_stubs",
        command=(f"mkdir -p {gen_dir} && {python} tools/android_posix_stubs.py {LINUX_DIR}/src/posix.h "
                 f"{guest_posix_c} {posix_imports}"),
        description="PS4 POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, posix_imports], rule="ps4_posix_stubs",
            implicit=[Path("tools/android_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"])

    imports_s = gen_dir / "imports.s"
    host_table_c = host_gen / "host_import_table.c"
    n.rule(
        name="ps4_imports",
        command=f"mkdir -p {host_gen} && {python} tools/ps4_imports.py --host-table {host_table_c} {imports_s} $in",
        description="PS4 IMPORTS",
    )
    n.build(outputs=[imports_s, host_table_c], rule="ps4_imports",
            inputs=[PORT_DIR / "host_imports.list", posix_imports, gl_imports],
            implicit=[Path("tools/ps4_imports.py")])

    host_gl_c = host_gen / "host_gl_dispatch.c"
    n.rule(
        name="ps4_gl_host",
        command=(f"mkdir -p {host_gen} && {python} tools/ps4_gl_host.py {LINUX_DIR}/src/gl.h {gl32} {gl2ext} "
                 f"{host_gl_c}"),
        description="PS4 HOST GL DISPATCH",
    )
    n.build(outputs=host_gl_c, rule="ps4_gl_host",
            implicit=[Path("tools/ps4_gl_host.py"), LINUX_DIR / "src" / "gl.h"])

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, semantics_header,
                         platform_semantics_header]

    # ---------- guest compilation

    n.rule(
        name="ps4_guest_cc",
        command=f"{compile_launcher(sln)}$ps4_guest_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="PS4 CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="ps4_guest_as",
        command="$ps4_guest_cc --target=x86_64-linux-gnux32 -c $in -o $out",
        description="PS4 AS $out",
    )

    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch_override}", f"-isystem {musl_arch}",
        f"-isystem {MUSL_DIR}/arch/generic", f"-isystem {MUSL_DIR}/include",
    ]
    gl_includes = [f"-I{d}" for d in khronos_include_dirs()]
    guest_abi = " ".join(GUEST_ABI_FLAGS + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []))
    guest_code = " ".join(GUEST_CODE_FLAGS)

    def guest_object(source: Path, cflags: str, prefix: str = "") -> Path:
        obj = obj_dir / prefix / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(BUILD)):
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        rule = "ps4_guest_as" if source.suffix == ".s" else "ps4_guest_cc"
        n.build(outputs=obj, rule=rule, inputs=source, implicit=generated_headers,
                variables={"cflags": cflags})
        return obj

    # musl
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch_override}", f"-I{musl_arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{MUSL_DIR}/src/include", f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources()]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name="ps4_ar",
        command="rm -f $out && $ps4_guest_ar rcs $out @$out.rsp",
        description="PS4 AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule="ps4_ar", inputs=musl_objects)

    # the game
    objects: List[Path] = []
    excluded = set(config.get("exclude_sources", []))
    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    for proj in sln.projects:
        if proj.name not in config["projects"]:
            continue
        options = proj.options
        defines = " ".join(f"-D{d}" for d in options.get("defines") or [])
        includes = " ".join(
            f"-I{_quote(d)}" for d in options.get("include_dirs") or [] if Path(d) != Path("xbox/include")
        )
        game_cflags = " ".join([
            guest_abi, guest_code, " ".join(game_flags),
            f"-include {prefix_header}", f"-include {semantics_header}", defines,
            f"-I{LINUX_DIR}/include", includes, *libc_includes, f"-idirafter {XDK_INCLUDE}",
        ])
        for obj in proj.objects:
            name = str(obj.file_path).replace(os.sep, "/")
            if obj.status.name == "Missing" or name in excluded or obj.file_path.suffix.lower() != ".c":
                continue
            cflags = game_cflags
            if name in VARIADIC_PROTOTYPE_FILES:
                # x86-64 variadic calls need a prototype too (%al counts the
                # vector registers used)
                cflags += f" -include {ANDROID_DIR}/include/halo_android_variadic_prototypes.h"
            objects.append(guest_object(obj.file_path, cflags))
        for source in sorted(Path(config["game_sources"]).glob("*.c")):
            objects.append(guest_object(source, game_cflags))

    # the platform layer shared with Linux and Android, and the guest runtime
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w",
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{ANDROID_DIR}/guest/runtime",
        f"-I{PORT_DIR}/include", f"-I{TOML_DIR}", f"-I{KCP_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", *gl_includes, *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    guest_host_only = {"memory_watch.c"}  # replaced by guest_memory_watch.c
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_host_only:
            continue
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", *libc_includes, f"-I{MUSL_MATH_DIR}/include",
        f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        objects.append(guest_object(source, musl_math_cflags))
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        f"-I{ANDROID_DIR}/guest/runtime", f"-I{PORT_DIR}/include",
        f"-I{arch_override}", f"-I{musl_arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{MUSL_DIR}/src/include", f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{ANDROID_DIR}/guest/runtime", f"-I{PORT_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{SDL_DIR}/include", *gl_includes, *libc_includes,
    ])
    for name in SHARED_RUNTIME:
        source = ANDROID_DIR / "guest" / "runtime" / name
        if name == "guest_thread.c":
            objects.append(guest_object(source, runtime_internal_cflags))
        elif name == "guest_memory_watch.c":
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    objects.append(guest_object(PORT_DIR / "guest" / "runtime" / "guest_start.c", runtime_internal_cflags))
    objects.append(guest_object(PORT_DIR / "guest" / "runtime" / "guest_misc.c", runtime_cflags))
    objects.append(guest_object(guest_gl_c, runtime_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="ps4_guest_as", inputs=imports_s)
    objects.append(imports_o)

    # ---------- the guest image

    linker_script = PORT_DIR / "guest" / "guest.ld"
    n.rule(
        name="ps4_guest_link",
        command=(f"$ps4_guest_ld -m elf32_x86_64 -static -nostdlib -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc} && "
                 f"{python} tools/ps4_image_check.py $out"),
        description="PS4 LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=image, rule="ps4_guest_link", inputs=objects,
            implicit=[libguestc, linker_script, Path("tools/ps4_image_check.py")])
    n.build(outputs="ps4_guest", rule="phony", inputs=[image, host_table_c, host_gl_c])
