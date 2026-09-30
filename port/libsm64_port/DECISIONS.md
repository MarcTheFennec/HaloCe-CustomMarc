# Decisions, assumptions and what is not done

Every choice here was made without asking anyone, as the task requires. Where a
choice could reasonably have gone the other way, the reason is given and so is
what changing it would cost.

## The task as read

"Port libsm64 into Halo CE Universal" — libsm64 turns *Super Mario 64*'s Mario
into a C library: you feed it inputs and surfaces, it gives you his state and
his triangles. The port, therefore, is not "make Halo look like Mario" but
**"let a real, simulated Mario exist inside a Halo level, controlled by the
player, standing on the level's own collision"** — built, tested, documented
and behind a flag, without disturbing the byte-matching decompilation work that
is the repository's actual purpose.

Sandbox limits (no `apt`, no 32-bit clang, no 32-bit SDL3, no
`raw.githubusercontent.com`) shaped *how much could be verified*, not the
design. `BUILD_NOTES.md` says exactly what was and was not run, and
`TEST_RESULTS.md` gives the numbers.

## Decisions

### 1. Feature flag: `HALO_CE_ENABLE_LIBSM64`, off by default

A define for the compiler (`configure.py --enable-libsm64`) *and* a setting for
the player (`config.toml sm64.enable`, default false) *and* a console switch
(`sm64_enable 0|1`). Three levels because they answer three questions: was the
build made with it, does this installation want it, does this session want it.
With the flag off, `ninja` compiles no unit of the module at all, and the two
lines added to `main.c` and `console.c` are inside `#ifdef`s: the game is
unchanged. Verified: `python3 configure.py` (no module rules in `build.ninja`),
`python3 configure.py --enable-libsm64` (module rules present).

### 2. libsm64 is loaded at run time, not linked

`dlopen`/`LoadLibrary` in `sm64_port_core.c`; `build_libsm64.py` builds the
vendored source into `build/linux/libsm64.so` beside the executable.

Why: the module must survive a missing library (no ROM, no build, a user who
did not want it) without the game failing to start, and a hard link would make
`libsm64.so` a dependency of every build. It also means a user can drop in
their own build.

Cost: an ABI mirror (`sm64_port_abi.h`) that has to stay true to
`libsm64.h` — which is why `tests/test_abi.c` static-asserts every field and
`make check-header` diffs the vendored header against the copy the tests use.

### 3. Vendored copy, not a submodule

`port/third_party/libsm64` (3.4 MB, CC0 1.0 like this port, upstream commit
`fd11813`, `PORT_NOTES.md` inside). A submodule would need network at clone
time and would break in-tree builds offline; the repository's own convention
(`port/third_party/mbedtls`, `musl-math`, `tomlc17`) is vendored too.

### 4. Nothing of Nintendo's is committed

- libsm64 needs Mario's geometry (`geo.inc.c`, `model.inc.c`) at build time:
  upstream fetches it, the offline importer rewrites it from a local cache, and
  **neither the cache nor the generated files are in the repository**
  (`.gitignore`: `port/third_party/libsm64/src/decomp/mario/`).
- libsm64 needs an SM64 US ROM (`baserom.us.z64`) at run time for textures and
  animation data. It is never committed; the README tells the user to supply
  their own. `*.z64` is ignored too.
- The tests run against a **real** libsm64 with **no ROM at all**: a synthetic,
  copyright-free blob satisfies both things libsm64 reads from a ROM (an MIO0
  texture block at 1132368 and the animation table at 0x4EC000). That is how
  the physics tests are honest about the physics without touching Nintendo's
  data.

### 5. Scale: 0.01 Halo units per SM64 unit

Mario's capsule is 160 SM64 units, so he stands 1.6 Halo units tall — the
height of a Halo player. One world, one set of doors and ramps. A setting
(`sm64.scale`) because a "tiny Mario on the Chief's shoulder" build is a
legitimate thing to want.

### 6. Time: one Halo tick is one Mario tick, with an accumulator

Both are 30 Hz, so no resampling. The accumulator still exists, because frame
lengths vary and the port draws interpolated frames; it is capped at four
steps per frame (133 ms) so that a stall costs Mario time, not the game its
responsiveness.

### 7. Collision: a window, not the level

libsm64 searches its surfaces linearly on every step. A Halo BSP has tens of
thousands. So the module converts a 40-unit window around the actor
(configurable), rebuilds it when he has walked a quarter of it or the BSP
changed, and caps it at 4096 surfaces. Measured cost with a real libsm64: 0.10
ms per frame for one actor, 1.07 ms for ten.

Consequence, accepted: Mario walks through Halo's *dynamic* objects (vehicles,
crates, moving doors). Converting those would mean rebuilding global surfaces
every tick, which is precisely what the window avoids. A future version could
regenerate only the window each tick (it is small) — the API already supports
it (`BeginSurfaceUpdate`/`EndSurfaceUpdate` are per-frame capable).

### 8. Input comes from the player's intent, not from keys

`player_control`'s throttle and control flags, with the camera's facing: so the
player's own bindings (keyboard or gamepad, whatever they set in Halo) drive
Mario, and "forward" means away from the camera. The alternative — reading keys
and XInput directly — would duplicate Halo's binding system and ignore the
player's settings.

Z (crouch/slide) is mapped to `_player_control_back_bit`: Halo's player-control
flags have no crouch bit of their own, and the alternative (reaching into the
biped's unit-control flags) is more coupling for one button.

### 9. Mario is not drawn

The module fills `SM64MarioGeometryBuffers` from `sm64_mario_geometry`, but
drawing them needs the port's renderer to draw an arbitrary mesh, and its
renderer draws model tags. Building a model tag at run time from a vertex
buffer is a real piece of work (tag memory, shader selection, the texture atlas
as a bitmap), and a wrong guess there costs a crash in the renderer. Shipping
the debug capsule, which is honest about the physics and cannot crash the
renderer, is the better deal; `sm64.draw_mesh` is the switch for finishing it.
**Not done, documented, and the only visible corner of the port that is.**

### 10. Audio is not wired

libsm64 wants a host to mix its synthesized audio. The port's audio is SDL3
feeding the game's own sound system; adding a second source means either a
mixing thread or a hook in the game's mixer, and a mistake there is audible
noise over the whole game, not a missing footstep. Silence is the conservative
choice. **Not done.**

### 11. Possession is off by default

`sm64.possess_player` moves the player's *own unit* to Mario's position. It is
off because it writes an object the netcode simulates: in a system-link game
the host would fight it. With it off, the module is purely additive — the
player's biped is untouched, and Mario is a drawn capsule.

### 12. Android is not supported

The Android guest is a static binary with a bundled libc that has no
`dlopen`. Supporting it would mean either implementing a loader in the guest or
link`libsm64` into the guest statically (which CC0 permits, but which changes
the guest's build, size and licensing story). Documented rather than faked: on
Android the module is not built, and the console reports it as unavailable.

### 13. C89 for the module, C99 for its tests

The game's units are `-std=gnu89`; the module is compiled with the game's
flags, so its sources are C89 (declarations first, no `//`, no compound
literals). The test suite is plain C99 on the host and never sees a Halo
header.

`tests/sm64_test_double.c` is a stand-in for libsm64 with simplified physics,
so that the module's own logic can be tested on a machine with no libsm64 and
no ROM; the same suite then runs unchanged against a real build.

### 14. F3 switches the module on and off

The task asked for a key, and F3 is the one. Pressing it starts the module and
spawns an actor in front of the player; pressing it again stops it. It acts on
the press, not while the key is held, and it is read every frame whether the
module is on or not, so it works with the module off — which is the point: a
build with the module in is a normal Halo until you press F3.

F3 is also one of the editor's debug keys ("Select Next Encounter",
`source/main/debug_keys.c`). Those keys are editor facilities and do nothing in
a game build, so the two do not collide in practice; the overlap is noted here
because it is the sort of thing that is annoying to rediscover.

### 15. The ROM and the library are looked for in the game's data folder

`sm64.rom` first, then the data folder (`platform_data_root()`, the folder that
holds `maps/`), then the working directory. The middle one is what makes "put
`baserom.us.z64` next to `assets/`" work wherever the game is started from,
which is where a player would look for it and where the game already finds its
own data. A test covers the order.

### 16. CI builds the module in the Linux and Windows debug builds

`tools/ci_build.py --enable-libsm64`, used by both desktop jobs' debug
configuration. Debug rather than release because it is the cheaper of the two
(no link-time or profile-guided optimisation), and the point of building it in
CI is that a commit which breaks the module fails there: it proves the module
compiles *and links* against the game on both platforms, which no amount of
host-side testing can.

The module is off at run time in those builds all the same, so nothing about
the artifacts changes for a player who does not want it — except that
`libsm64.so` (or `sm64.dll`) now sits beside the executable, and libsm64's
licence beside that.

The Linux job also runs the module's own suite, against a libsm64 built for the
runner rather than the game's: the game's library is 32-bit, and a test binary
built on the runner cannot open it.

### 17. The host callback is called `log_message`, not `log`

Because `port/include/halo_math.h` defines `log(x)` as a macro, and the game's
prefix header includes it. Found by compiling the module's units in the Linux
build's own configuration.

## Assumptions

- A US SM64 ROM, and only the US one: libsm64 is built for `VERSION_US`.
- The player's ROM is theirs to use; the port never reads it except through
  libsm64, and never copies any of it into the repository or into a save.
- Halo's tick rate stays 30 (it is a decompilation: it will).
- Windows and macOS behave like Linux here: `LoadLibrary`/`dlopen` are the only
  platform-specific part of the module, and both are implemented. The Windows
  build rules are written but were not run: `windows_build.py` only generates
  its rules on a Windows host, which this sandbox is not.
- At most 16 actors (`SM64_PORT_MAX_ACTORS`); the limit exists to bound the
  cost of a frame, and it is enforced.
