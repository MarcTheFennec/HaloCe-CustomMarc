# PS4 port: status and plan

## Status

Last update: 2026-09-29. **The port has not run on a PS4 yet.** This work
was done without a console. "Verified" means built or run here. The
"Where" column says on which system.

| # | Acceptance item | Status | Evidence |
| --- | --- | --- | --- |
| A1 | Build the PS4 ELF, fself and PKG; Linux, Windows and Android stay green | **Partly verified** | ELF and fself: verified (`ninja ps4_eboot` and `./build-ps4.sh`; 0 warnings; SELF magic `4F153D1D`, OELF type `0xFE10`, 113 imports; 13 imports checked against ps4libdoc NIDs). PKG: `param.sfo`, `icon0.png` and `halo.gp4` are made and checked. The `.pkg` step calls LibOrbisPkg PkgTool.Core. It was **not run** here: it needs .NET, which the build machine could not download. Other platforms: without `--ps4`, `build.ninja` is identical to the base commit except the reconfigure inputs. The shared source changes are inert off the PS4 (below). `ninja linux` could not link here (no 32-bit glibc on the build machine). |
| A2 | Boot, klog, clean exit | Not run on a PS4 | Linux test host: the game image boots, runs its main loop, quits on request and exits with code 0 (`HALO_PS4_QUIT_AFTER=5`). |
| A3 | 1080p / 720p with vsync | Not run | `video.height` and `video.vsync` in `ps4.toml` are implemented (Piglet window surface, `eglSwapInterval`). |
| A4 | Content root logging | Verified on the Linux test host | Each file that is found logs its root; each missing file logs all paths tried (`file not found: /halo/maps/ui.map (tried ...)`). |
| A5 | Map load | Not run | Needs the game data. The Linux test host had no maps. |
| A6 | Geometry, textures, sky, HUD, first-person weapon at ≥ 30 FPS | Not run | The renderer layer compiles. All 529 generated shaders translate to GLSL ES 1.00 and pass glslangValidator. The error material is in place. Measurement table below. |
| A7 | Menu navigable with the DualShock 4 | Not run | libScePad input is implemented (mapping in BUILDING-PS4.md). |
| A8 | Audio | Not run | libSceAudioOut on its own thread is implemented. |
| A9 | Input | Not run | As A7, plus reconnection and vibration. |
| A10 | ≥ 10 minutes of gameplay | Not run | The frame rate and memory monitor (`monitor.*` in `ps4.toml`) logs what this test needs. |
| A11 | 30 minutes stable | Not run | As A10. |

### Measurements (to fill in on a console)

Take the numbers from `host.log`: `fps` lines (every `monitor.fps_seconds`)
and `memory` lines (every `monitor.seconds`).

| Test | Firmware / GoldHEN | Map | Resolution | FPS avg / min | Memory peak | Result |
| --- | --- | --- | --- | --- | --- | --- |
| A6 render | | | | | | |
| A7 menu | | ui | | | | |
| A10 10 min | | | | | | |
| A11 30 min | | | | | | |

### Blockers

1. **A PS4 with GoldHEN** for A2 to A11. The next step is to install the
   PKG, copy the maps and the two shader compiler modules, and read klog.
2. **PkgTool.Core** (needs .NET) to make the `.pkg`. The staged files and
   `halo.gp4` are ready for it.
3. **The Piglet shader compiler modules** from a 4.74 devkit firmware.
   The user must supply them (BUILDING-PS4.md). There is no public
   alternative for runtime GLSL compilation on retail firmware.
4. **No OpenGL ES implementation on the build machine.** Mesa and ANGLE
   could not be downloaded or built there. The Chromium SwiftShader
   package on npm contains only stub libraries. So the GL ES 2 layer and
   the error material compile but have not drawn a frame. The shaders
   were checked with glslangValidator only. A Linux machine with Mesa
   could run the test host with an EGL context; this is the next step
   without a console.

## Design

### Recon

- The game is the Xbox build 2342 decompilation. It is 32-bit code (ILP32)
  and assumes 32-bit pointers in its data files and structures.
- The Linux and Windows ports run it as a 32-bit x86 program. The Android
  port runs it as a 32-bit guest image inside a 64-bit host
  (`port/android`). The renderer is `port/linux/src/d3d8_gl.c`: Direct3D 8
  over OpenGL (ES 3 on Android), with shaders generated from the NV2A
  programs (`nv2a_vsh.c`, `nv2a_psh.c`).
- The PS4 runs 64-bit FreeBSD-based code only. OpenOrbis provides clang
  and LLD for `x86_64-pc-freebsd12`, musl, headers and create-fself. The
  graphics libraries on retail firmware are GNM (low level, no public
  shader compiler) and Piglet (OpenGL ES 2.0 and EGL 1.4). Retail Piglet
  has no shader compiler; the devkit modules add it back (flatz, 2018).

### Architecture

DECISION: the game runs as an **x32 guest** (`x86_64-linux-gnux32`: 64-bit
instructions, 32-bit pointers) inside a native 64-bit PS4 host. The guest
lives in the low 4 GB of the address space. This is the Android design
with the x86-64 instruction set, so the game code needs no pointer-size
changes.

```
eboot.bin (OpenOrbis ELF -> fself)
 ├─ host (port/ps4/host, LP64, OpenOrbis musl + system libraries)
 │   ├─ host_main.c      start, settings (ps4.toml), monitor
 │   ├─ host_orbis.c     platform: klog, Piglet/EGL, libScePad, libSceAudioOut,
 │   │                   notifications, devkit shader compiler loading
 │   ├─ host_loader.c    loads the embedded guest image at 0x88000000
 │   ├─ host_memory.c    guest memory (Xbox window 0x80000000, pool), crashes
 │   ├─ host_syscall.c   Linux x32 system calls -> FreeBSD/OpenOrbis
 │   ├─ host_files.c     /halo -> /data/halo, /mnt/usb0/halo, /app0
 │   ├─ host_thread.c    guest threads with large stacks, TLS
 │   ├─ host_sdl.c       the SDL subset the game's platform code uses
 │   ├─ host_gl*.c       OpenGL ES 3 (as d3d8_gl.c uses it) over Piglet ES 2
 │   └─ host_shader.c    GLSL ES 3.00 -> GLSL ES 1.00
 └─ guest image (build/ps4/halo_guest.elf, x32, embedded by host_image_blob.S)
     └─ the game + port/linux/src (d3d8_gl.c, nv2a_*.c, posix_*.c) + musl
```

### Dependency table

| Subsystem | Linux port | PS4 implementation |
| --- | --- | --- |
| Toolchain | clang, 32-bit x86 | Guest: clang `x86_64-linux-gnux32`. Host: OpenOrbis clang/LLD `x86_64-pc-freebsd12`, create-fself, ps4libdoc stubs |
| C library | glibc | Guest: musl 1.2.5 (x32). Host: OpenOrbis musl |
| Window, video | SDL3 + OpenGL 4.5 | Piglet EGL window (1920x1080 or 1280x720), OpenGL ES 2 + extensions |
| Renderer | d3d8_gl.c on GL 4.5 | d3d8_gl.c on "GLES 3" (`HALO_ANDROID` path), emulated over ES 2 by host_gl_es2.c |
| Shaders | GLSL 4.5 | GLSL ES 3.00 -> ES 1.00 (host_shader.c), compiled by Piglet with the devkit shader compiler |
| Audio | SDL3 audio | libSceAudioOut, 48 kHz stereo, own thread |
| Input | SDL3 gamepad | libScePad (initial user, standard port) -> SDL gamepad events |
| Files | POSIX | host_files.c content roots, case-insensitive lookup with a cache |
| Threads, time | pthreads | Host pthreads (OpenOrbis), guest stacks in guest memory, `clock_gettime` |
| Memory | mmap | Fixed mappings under 4 GB, `sceKernelReserveVirtualRange` |
| Network | BSD sockets | BSD sockets through OpenOrbis musl/libkernel |
| Logging | stderr + debug.txt | klog (`sceKernelDebugOutText`), host.log, debug.txt, 64 KB crash ring |

### Port order (as done)

1. Guest build: game + musl as x32, link checks (`ninja ps4_guest`).
2. Host core: loader, memory, system calls, threads, files, log. Tested
   on Linux (`ninja ps4_host_linux`).
3. GL layer and shader translator. Validated with glslangValidator.
4. ORBIS platform layer, stubs, eboot link, fself (`ninja ps4_eboot`).
5. CMake toolchain, packaging, scripts, documentation.
6. Next: console tests A2 to A11 and the fixes that they show.

### Decisions

- DECISION: firmware. The port targets all GoldHEN firmwares (5.05, 6.72,
  7.02, 7.55, 9.00, 11.00). It uses only libraries that are the same on
  all of them. The stubs are made from ps4libdoc 5.05 names, and the NIDs
  do not change between firmwares.
- DECISION: renderer. The candidates were:
  - Vulkan: the PS4 has no Vulkan.
  - GNM: needs Sony's shader compiler (proprietary, no public
    replacement) and a command buffer builder that does not exist in
    OpenOrbis.
  - Software rendering: too slow for ≥ 30 FPS on the 1.6 GHz Jaguar
    cores with the game's multi-pass shaders.

  The port uses **Piglet (OpenGL ES 2)** under the existing d3d8_gl.c.
  This needs the devkit shader compiler modules from the user.
- DECISION: x32 guest in a 64-bit host (above). Guest code:
  `-fdirect-access-external-data` and `--no-relax`, because LLD relaxes
  x32 GOTPCRELX incorrectly.
- DECISION: the eboot uses the PAID `0x3800000000000035` and the auth info
  of OpenOrbis' piglet sample. Piglet refuses applications without them.
- DECISION: system library stubs come from the public ps4libdoc symbol
  lists (`tools/ps4_stub_libs.py`). The build uses no Sony code or
  headers.
- DECISION: the canonical build is the repository's `configure.py` and
  ninja (like the other platforms). `build-ps4.sh` and
  `port/ps4/CMakeLists.txt` use the CMake functions of `cmake/ps4.cmake`
  for the eboot and the package, and ninja for the guest image.
- DECISION: the `.pkg` is made by LibOrbisPkg PkgTool.Core (the tool
  that the OpenOrbis samples use). `tools/ps4_package.py` makes
  `param.sfo` itself (TITLE_ID `HCEP02342`, CATEGORY `gde`).
- DECISION: a fragment shader that Piglet rejects is replaced by a
  magenta and black checkerboard (the error material). A program that does
  not link is linked again with it. See host_gl_es2.c.
- DECISION: the host's settings are in `/data/halo/config/ps4.toml`. The
  log level filters klog and host.log only; the crash ring keeps all
  levels.
- DECISION: page size = max(system page size, 16 KB); one controller
  (controller 1).

### Assumptions to check on a console

- ASSUMPTION: GoldHEN allows `PROT_EXEC` mappings for the embedded guest
  image, and fixed mappings in the low 4 GB work for homebrew.
- ASSUMPTION: the FreeBSD amd64 `mcontext` layout (crash reports) and the
  FreeBSD `FIONREAD` value apply.
- ASSUMPTION: Piglet reports `GL_MAX_VERTEX_UNIFORM_VECTORS` ≥ 196 and
  S3TC (DXT) texture compression.
- ASSUMPTION: flatz's Piglet patch offsets for the 4.74 devkit modules
  apply to the modules that the user copies.
- ASSUMPTION: GoldHEN's FTP server accepts curl's passive mode uploads
  with `--ftp-create-dirs` (tested against a standard FTP server only).

### Changes outside port/ps4

| File | Change | Effect on other platforms |
| --- | --- | --- |
| `source/interface/terminal.c` | `va_list` instead of `char *` for `terminal_printf`'s arguments | None: the same type on the Xbox, i386 and Windows. Required on x32. |
| `port/linux/src/msvc_crt.c` | SSE rounding and flags for `_control87`/`_statusfp`/`_clearfp` | Only with `HALO_PS4`. |
| `tools/android_gl_stubs.py` | `--integer-registers N` option | None without the option. |
| `tools/project_x86.py`, `configure.py` | `--ps4` targets | None without `--ps4`. |
