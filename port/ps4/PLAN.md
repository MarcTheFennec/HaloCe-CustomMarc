# PS4 (homebrew) port — plan

This document is the plan for a PlayStation 4 homebrew port of the game. It
follows the structure of the existing ports (`port/linux`, `port/windows`,
`port/android`) and reuses as much of them as possible. Nothing in this
document is built yet; it exists so that the work can be reviewed and
sequenced before code is written.

## 1. What we are porting

The game is Xbox build 2342 compiled from the decompilation. The native
ports keep it as **32-bit-pointer code**, because the game's data (tags,
cache files, saved games) contains 32-bit pointers, and the tag cache is
linked to fixed Xbox addresses (`0x80000000`–`0x88000000`, see
`port/linux/src/platform.h` and `port/linux/include/halo_port_capacity.h`).

The existing ports are:

| Port | Game code | Platform layer | How it runs |
| --- | --- | --- | --- |
| Linux | i686 (`-m32`) | `port/linux/src` on SDL3 + OpenGL 4.5 | Ordinary 32-bit process |
| Windows | i686 | `port/linux/src` + `port/windows/src` | Ordinary 32-bit process |
| Android | **arm64_32** (ILP32 on AArch64) | `port/linux/src` on GLES 3 | **Guest image** loaded by a 64-bit **host** app (`port/android/host`) that provides syscalls, threads, SDL and GL through an import table (`port/android/host_imports.list`) |

The PS4 is an x86-64 FreeBSD-derived system (Orbis OS). Homebrew runs as
64-bit ELF processes built with the OpenOrbis toolchain. There is no
32-bit process mode available to homebrew. So the PS4 port **must follow
the Android model**: a 32-bit-pointer guest image plus a 64-bit host.

## 2. Key architectural decisions

### 2.1 Guest ABI: x32 (ILP32 on x86-64)

Compile the game and the platform layer with clang for
`--target=x86_64-unknown-linux-gnux32`. This is the same idea as Android's
`arm64_32`, but far cleaner on x86:

- clang and lld support x32 natively (`ld.lld -m elf32_x86_64`). No Mach-O
  conversion step (`tools/android_asm_convert.py`) is needed.
- musl has an official `arch/x32`, so the custom `arch/arm64_32` headers in
  `port/android/guest/libc` are not needed; we use musl's own.
- The x32 psABI guarantees that pointers passed or returned in registers
  are zero-extended to 64 bits, and the register calling convention is the
  SysV AMD64 one. The 64-bit host can therefore call guest functions and
  the guest can call host functions **directly**, with pointer arguments
  passing through unchanged, as long as the host is careful about `long`
  (32-bit in the guest) and structures whose layout differs. The rules in
  `port/android/include/halo_android_abi.h` (32-bit values as `int`,
  64-bit values as `long long`, pointers as-is) apply unchanged.
- The game's MSVC inline assembly is already guarded by `#ifdef HALO_LINUX`
  with C fallbacks (`cseries.h`, `matrix_math.c`, `decals.c`, `hud_draw.c`),
  so nothing i386-specific is compiled into the guest.
- `-ffp-contract=off` and the other flags of `GUEST_ABI_FLAGS` in
  `tools/android_build.py` carry over; x87/SSE arithmetic on x86-64 is
  actually closer to the Xbox than ARM was.

The guest is linked as a static ELF32 (`e_machine = EM_X86_64`,
`ELFCLASS32`) at a fixed base below 4 GB, with the same
`struct halo_guest_header` + import table scheme as Android. The image base
`0x88000000` and the Xbox window `0x80000000`–`0x88000000` are kept.

### 2.2 Host: a 64-bit OpenOrbis application, **without SDL**

On Android the host is a thin layer over SDL3. On PS4 there is no SDL3
(only an SDL2 fork, `orbis-ports/SDL2`), and the guest's SDL usage already
goes through a narrow, handle-based interface of about 40 functions
(`host_sdl_*` in `port/android/guest/runtime/guest_host.h`). The cheapest,
most robust option is to implement those functions **directly on the PS4
system libraries**, and synthesize the few SDL3 events the game needs
(gamepad added/removed, quit):

| Host service | PS4 implementation |
| --- | --- |
| Window, GL context, swap | Piglet (`libScePigletv2VSH`) EGL surface on `sceVideoOut` |
| Shader compilation | `libSceShaccVSH` (runtime GLSL compiler used by Piglet) |
| Gamepads, rumble | `sceUserService` + `scePad` (DualShock 4 / DualSense). Up to 4 pads → controllers 1–4, split screen as on Android |
| Audio stream | `sceAudioOut` (48 kHz stereo S16; a ring buffer fed by `host_sdl_put_audio_stream_data`) |
| Clipboard, toasts, message boxes | Stubs / `sceMsgDialog` where useful |
| Threads | `scePthread` (FreeBSD pthreads); stacks allocated in guest memory below 4 GB, as `host_thread_create` does today |
| Logging | `sceKernelDebugOutText` + `debug.txt` in the data root; optional UDP log sink (klog-style) for development |

### 2.3 Syscall translation: Linux → Orbis

The guest's musl issues Linux syscalls through `host_syscall`
(`port/android/guest/runtime/guest_syscall.c`). On Android the host mostly
passes them to the kernel. On PS4 the host must **translate each syscall
to libkernel/libc calls**, converting structures (`stat`, `dirent`,
`timespec`, `iovec`, `sockaddr`, `msghdr`). The list of syscalls actually
used is already known — it is the `case` list in
`port/android/host/host_syscall.c` (about 80 syscalls). Groups:

- **Files**: `openat`, `read/write/pread64/pwrite64/readv/writev`, `lseek`,
  `fstat/newfstatat/statx`, `getdents64`, `mkdirat`, `unlinkat`,
  `renameat`, `ftruncate`, `fsync`, `faccessat`, `getcwd`, `readlinkat` →
  OpenOrbis libc / `sceKernel*`. Case-insensitive lookup is already handled
  in the guest (`xbox_files.c`).
- **Memory**: `mmap/munmap/mprotect/madvise/mremap/brk` → `sceKernelMmap`
  / `sceKernelMapFlexibleMemory` with the fixed-address flag. The guest
  needs all its memory below 4 GB; the host reserves the ranges once at
  start-up (`host_memory.c`), and `brk`/`mmap` are served from that pool.
- **Time**: `clock_gettime`, `nanosleep`, `gettimeofday` → `sceKernelClockGettime`,
  `sceKernelNanosleep`. Linux clock IDs are mapped.
- **Threads**: `clone/clone3` → never reach the kernel (musl's
  `pthread_create` is replaced by `guest_thread.c` → `host_thread_create`,
  as on Android). `set_tid_address`, `gettid`, `tkill/tgkill` → mapped
  to `scePthread*`.
- **futex** — the one syscall with **no PS4 equivalent**. musl's locks,
  mutexes, condition variables and semaphores all sit on it. Two options;
  we take (a) first and keep (b) as a fallback:
  (a) a **futex emulation** in the host: a hash table of wait queues keyed
      by address, over `scePthreadMutex`/`scePthreadCond`, supporting
      `FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_REQUEUE` and the private/bitset
      variants musl uses. This is a well-known ~300-line construct.
  (b) replace musl's threading primitives in the guest with host imports
      (`host_mutex_*`, `host_cond_*`), which is more invasive.
- **Signals**: `rt_sigaction`, `rt_sigprocmask`, `sigaltstack` → FreeBSD
  `sigaction` in libkernel. Needed for `memory_watch.c` (see 2.5).
- **Network**: `socket`, `bind`, `sendmsg/recvmsg`, `ppoll/pselect6`,
  `epoll_pwait`, `ioctl(FIONBIO)`, `fcntl(O_NONBLOCK)` → `sceNet*` (BSD
  sockets; `sockaddr` layout differs from Linux by the `sa_len` byte, so it
  is converted). `epoll` is emulated over `poll`, which the guest already
  supports as a fallback path.
- **Process**: `exit/exit_group`, `getpid`, `uname`, `sysinfo`,
  `getrandom` (→ `sceRandomGetRandomNumber`), `getrlimit/prlimit64`
  (fixed answers), `execve/wait4/pipe2` (used only by the self-updater →
  return `ENOSYS`).

### 2.4 Graphics: GLES 2.0 on Piglet is the constraint

This is the **largest risk** in the port. The renderer
(`port/linux/src/d3d8_gl.c`, `nv2a_vsh.c`, `nv2a_psh.c`, ~4,700 lines)
targets OpenGL 4.5 on desktop and **OpenGL ES 3.2** on Android. Piglet is
**OpenGL ES 2.0** with a number of extensions. Features the renderer uses
that GLES 2 lacks, and how to handle them:

| GLES 3 feature used | GLES 2 replacement on Piglet |
| --- | --- |
| Vertex array objects | `GL_OES_vertex_array_object`, or re-bind attributes per draw |
| Uniform buffer objects (`glBindBufferRange`) for the 192 NV2A vertex constants | plain `uniform vec4 c[N]` arrays set with `glUniform4fv`; batch by dirty range |
| Sampler objects | set sampler state on the texture object; cache per (texture, state) |
| `glTexImage3D` (volume textures, rare) | `GL_OES_texture_3D` if present, otherwise the largest slice |
| `glMapBufferRange` | `glBufferSubData` with orphaning, or `GL_OES_mapbuffer` |
| Fence syncs (`host_gl_fence_frame`) | `glFinish`/`glFlush` with a 1-frame ring of buffers |
| Multiple render targets, `glDrawBuffers` | Not needed for the NV2A path (single target); verify |
| `glBlitFramebuffer` (final 640×480 → display scale) | Draw a textured quad |
| `glClipControl`, `glDepthRange` tweaks | Handled in the generated vertex shader (Android already does the reversed-Z / viewport maths in GLSL, `nv2a_vsh.c` ~L289) |
| Occlusion queries (`glGenQueries`) | `GL_EXT_occlusion_query_boolean` if present, else always-visible |
| Integer/float textures, texture swizzle | Convert in `xbox_textures.c` at upload (it already converts most Xbox swizzled formats on the CPU) |
| DXT compression | `GL_EXT_texture_compression_s3tc` / `dxt1/3/5` extensions (PS4 GPU supports BC1–3 natively) |
| GLSL `#version 300 es` shaders | Emit `#version 100`: `attribute/varying` instead of `in/out`, `gl_FragColor`, no integer ops / bit ops, no `texture()` overloads, precision qualifiers. The two shader generators already branch on `HALO_ANDROID`; add a third profile, `HALO_PS4` |

Plan: introduce a **GL profile abstraction** (`xgpu_capabilities` already
exists) with a `gles2` profile, implement it on the **Linux build first**
using ANGLE or Mesa's GLES 2 context (so it can be debugged with
RenderDoc), then bring it up on Piglet. Piglet-specific quirks (the
`SCE` extensions, precision, max uniforms — check `GL_MAX_VERTEX_UNIFORM_VECTORS`
which must be ≥ 256 for the NV2A constant array) get handled last.

`libScePigletv2VSH.sprx` and `libSceShaccVSH.sprx` are **not on retail
firmware**. As with RetroArch, SM64 and LÖVE on PS4, the user has to copy
them to the console themselves (e.g. `/data/halo/modules/`). This fits
the manual-data approach of section 3; the host loads them with
`sceKernelLoadStartModule` from that folder and shows a clear message if
they are missing.

### 2.5 Memory write tracking (`memory_watch.c`)

The texture cache makes guest pages read-only and catches the first write
with a `SIGSEGV` handler. On PS4, homebrew can install `sigaction`
handlers via libkernel (used by several homebrew debuggers). We keep the
mechanism, but implement it in the host (`host_memory_watch_*`, as
Android does), and provide a **fallback mode without protection** (hash
the texture on every upload / every N frames) behind a config flag, in
case the fault handler proves unreliable on some firmware.

### 2.6 Game data: manual placement, no ISO extraction on the console

Per the project owner's decision, **the PKG contains only the game**. The
extract-xiso path (`port/linux/src/xiso.c`, the file picker and the
progress UI) is compiled out on PS4 (`HALO_PS4`). The user extracts
`maps/` on a PC with the Linux/Windows build (exactly as the Android
README already instructs) and copies it to the console with FTP
(GoldHEN) or PS4Xplorer.

Data root search order on PS4:

1. `/data/halo/` (primary; writable on all jailbroken firmwares)
2. `/mnt/usb0/halo/` … `/mnt/usb3/halo/` (USB stick with `maps/`)
3. `/user/app/HALO00001/` (the app's own folder, for completeness)

If no `maps/` folder is found, the game shows a message dialog with the
expected path and exits. Layout inside the data root, following Android:

| Item | Path under `/data/halo` |
| --- | --- |
| Game data | `maps/` |
| Saved games (`z:\`, `u:\`) | `save/z`, `save/u` |
| Settings | `config.toml` |
| Log | `debug.txt` |
| Piglet + shader compiler modules | `modules/libScePigletv2VSH.sprx`, `modules/libSceShaccVSH.sprx` |

The self-updater (`updater.c`, `posix_update.c`) is disabled: a new
version is a new PKG the user installs. The update check at start-up is
compiled out too, so the game never touches GitHub from the console.

### 2.7 Networking

- **System link** on the LAN: works through the translated socket calls.
  Broadcast to `255.255.255.255` needs `SO_BROADCAST`, which `sceNet`
  supports.
- **Internet play** (`p2p.c`, mbedtls, MQTT signalling, STUN, KCP): all
  userland code over UDP sockets; should work unchanged. DNS resolution
  goes through `sceNetResolver` instead of `getaddrinfo`.
- **Discord** integration is compiled out (as on Android).
- Cross-platform play with Linux/Windows/Android is expected to work,
  since the guest simulates the same 32-bit game with the same
  `halo_port_capacity.h` limits (this is exactly why the guest must be
  ILP32).

### 2.8 Input

- `scePad` state → the game's Xbox gamepad model. Mapping is the
  DualSense table in `port/android/README.md`.
- No keyboard/mouse in the first version (USB keyboards are possible later
  through `sceKeyboard`, but the Android port shows the game is complete
  on gamepad only).
- The developer console can be opened with a pad combination
  (e.g. L3+R3+Options) and typed with an on-screen keyboard (`sceImeDialog`)
  in a later milestone.

## 3. Repository layout

```
port/ps4/
  README.md                 user instructions (install PKG, copy maps/, modules)
  PLAN.md                   this document
  port.json                 sources/excludes for the PS4 guest, like port/linux/port.json
  include/
    halo_ps4_abi.h          guest/host ABI constants (image base, magic, ABI version)
  guest/
    runtime/                guest_start.c, guest_syscall.c, guest_thread.c,
                            guest_sdl.c, guest_memory_watch.c, guest_misc.c
                            (mostly copied from port/android/guest/runtime,
                            with the AArch64 bits replaced by x86-64)
    guest.ld                fixed-address link script (from port/android/guest/guest.ld)
  host/
    host_main.c             entry point, module loading, data root discovery
    host_loader.c           ELF32 x86-64 loader (Android's, adapted to Elf32_*)
    host_memory.c           low-4GB reservation and allocator
    host_syscall.c          Linux → Orbis syscall translation
    host_futex.c            futex emulation
    host_thread.c           guest threads on scePthread
    host_video.c            sceVideoOut + Piglet EGL, swap, vsync
    host_gl.c               GL entry point table for the guest
    host_pad.c              sceUserService/scePad → host_sdl_* gamepad API
    host_audio.c            sceAudioOut ring buffer → host_sdl_* audio API
    host_files.c            path mapping, dirent/stat conversion
    host_net.c              sceNet mapping
    host_debug.c            logging, crash dump with guest backtrace
  pkg/
    sce_sys/                icon0.png, param.sfo template, pic1.png
    halo.gp4                PkgTool project
  host_imports.list         host functions the guest imports (as on Android)
tools/
  ps4_build.py              ninja rules: guest (x32) + host (OpenOrbis) + PKG
  test_ps4_port.py          syscall-table and ABI-contract tests
```

`port/linux/src` stays the shared platform layer; PS4-specific behaviour
is added under `#ifdef HALO_PS4` next to the existing `HALO_ANDROID`
branches (about 100 sites today). Where Android and PS4 want the same
thing (no updater, no xiso, GLES shaders, guest/host SDL), a common
`HALO_GUEST` macro is introduced so the branches are not duplicated.

## 4. Build and CI

- **Toolchain**: OpenOrbis PS4 Toolchain (latest release; clang 15+ with
  `x86_64-pc-freebsd12-elf`, `create-fself`, `PkgTool.Core`). The guest
  needs a clang that can target `x86_64-linux-gnux32` — any stock clang.
- `python configure.py --ps4-toolchain=<path>` then `ninja ps4` →
  `build/ps4/eboot.bin`; `ninja ps4_pkg` → `dist/halo-ps4/IV0000-HALO00001_00-HALOCE0000000000.pkg`.
- `tools/ci_build.py ps4 {debug,release}` for GitHub Actions: a Linux job
  that downloads and caches the OpenOrbis tarball; artifacts and releases
  as for the other platforms. The PKG (game only, no data) is small, so
  it fits the existing release flow.
- PGO: the guest can reuse `pgo/halo_linux.profdata` if the profile
  matches; otherwise build without PGO at first.

## 5. Milestones

Each milestone ends in something that can be run and checked.

| # | Milestone | Exit criterion |
| --- | --- | --- |
| **M0** | **Toolchain and skeleton** — `tools/ps4_build.py`, `port/ps4/` layout, OpenOrbis host that boots, logs, and shows a clear-colour frame on Piglet. PKG builds in CI. | PKG installs and runs on a jailbroken PS4; log visible over UDP/FTP. |
| **M1** | **x32 guest compiles and links** — game + `port/linux/src` + musl (x32) + guest runtime linked at `0x88000000`. Verified on Linux first: a small Linux 64-bit test host (or the Android host recompiled for x86-64) loads it and runs to the main menu. This de-risks the ILP32 ABI independently of the PS4. | `halo_guest.elf` reaches the main menu under the Linux test host. |
| **M2** | **Host services on PS4** — loader (ELF32), memory below 4 GB, syscall translation, futex emulation, threads, files. Game boots headless on PS4 (`debug.null_renderer`), loads `maps/ui.map`, logs "main menu". | `debug.txt` on the console shows the menu loop running without assertion failures. |
| **M3** | **GLES 2 renderer** — `gles2` GL profile developed on Linux (ANGLE/Mesa), then on Piglet. Main menu, then campaign level a10 render correctly. | Screenshots match the Linux build on a10 and b30 (`debug.screenshot_*`). |
| **M4** | **Input and audio** — `scePad` (4 pads, rumble), `sceAudioOut`. Split screen. | Playable start to finish of a10 with a DualSense; sound and music. |
| **M5** | **Networking** — system link on LAN with a Linux machine; internet invite links. | 2-machine PS4↔Linux system link game runs 10 minutes without desync. |
| **M6** | **Polish and release** — memory-watch fallback, performance (target 60 fps interpolated on base PS4), `README.md`, error dialogs, release in CI, main README/Download table updated. | Tagged release with `halo-ps4-release.zip` containing the PKG. |

Rough effort: M0–M2 is mostly mechanical adaptation of existing Android
code (~3,000 lines host, ~700 lines guest runtime); M3 is new work and
the schedule risk; M4–M5 are small.

## 6. Risks and open questions

| Risk | Mitigation |
| --- | --- |
| Piglet's GLES 2 cannot express something the NV2A emulation needs (e.g. > 256 uniform vectors, dependent texture reads in the combiner emulation) | Prototype the pixel-shader path in M3 first on Piglet with the most complex `nv2a_psh` outputs; fall back to multi-pass for the rare shaders. |
| Fixed mappings at `0x80000000`/`0x88000000` are refused by the Orbis kernel | Test in M0 with a probe. Fallback: make the guest position-independent at build time and choose any free low range (the Android host already searches `LOW_START`..`LOW_LIMIT` for its pool). |
| `SIGSEGV`-based write tracking is unreliable | Hashing fallback (2.5). |
| Flexible-memory limit for homebrew (default ~448 MB) is too small for the 128 MB window + heaps + GL | Request more via `param.sfo`/`sceKernelSetFlexibleMemory`-equivalents, or map the guest window from direct memory. Measure real usage on Linux first (`/proc/self/status`). |
| The runtime shader compiler is slow (first-frame hitches) | Cache compiled program binaries under `/data/halo/cache/` (`GL_OES_get_program_binary`). |
| Users must find two Sony `.sprx` modules | Document clearly; detect and show a dialog with the exact path. Same requirement as RetroArch/LÖVE on PS4, so the community knows it. |

## 7. Progress

| Milestone | State | Notes |
| --- | --- | --- |
| M0 | **Code complete, awaiting hardware test** | `tools/ps4_build.py` (host, fself, sfo, gp4, pkg), `port/ps4/host` (log, data root, memory probe, Piglet context, pads, exit combo), CI job `ps4`, `tools/test_ps4_port.py`. The host units compile against the OpenOrbis headers; the link and the package are exercised by CI, which has the toolchain's library stubs. First run on a console: read `debug.txt` for the memory probe result (decides M2's layout) and the GL strings. |
| M1 | Not started | |
| M2–M6 | Not started | |

## 8. Decisions taken (2026-09-29)

| Question | Decision |
| --- | --- |
| Target firmware / jailbreak | **GoldHEN, firmware 9.00 and later.** Older firmwares (5.05 Mira) are not a target. |
| Renderer sequencing | **Develop the `gles2` GL profile on Linux first** (ANGLE/Mesa GLES 2 context, RenderDoc), then bring it up on Piglet. |
| Order of work | **M0 first** (toolchain, `port/ps4` skeleton, ninja/CI rules, a host that boots and clears the screen), then M1. |
| Title ID and data path | **`HALO00001`**, data root **`/data/halo`**. Icon from `port/android/art`. |
| PS4 Pro / 4K | First version renders 480 lines at the display's aspect ratio scaled to 1080p, as on Android. Higher internal resolution later. |
| ISO extraction on the console | **No.** `xiso.c` stays for the PC builds only. |
