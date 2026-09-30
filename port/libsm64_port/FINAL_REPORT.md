# Final report: libsm64 in Halo CE Universal

**What was asked for:** port [libsm64](https://github.com/libsm64/libsm64) into
Halo CE Universal, autonomously and end to end: a working, tested, documented,
feature-flagged integration, or an exhaustive account of why a part of it is
impossible and everything else delivered anyway.

**What was delivered:** a module (`port/libsm64_port`) that puts a real,
simulated Mario into a Halo level, standing on the level's own collision and
driven by the player's own controls — off by default, built only with
`configure.py --enable-libsm64`, tested by 31 tests that run against a real
libsm64 with no ROM, and documented in five files. Two things are not finished
and are documented rather than faked: **Mario is not drawn** (he is a capsule)
and **he is silent**; **Android is not supported** at all, because its guest
cannot open a shared library. `DECISIONS.md` explains all three.

## The commits

Eleven atomic commits on top of `dddaffd`, in the order they make sense to
read, exported as a `git format-patch` series (`patches/`, 11 patches):

| # | commit | what |
|---|---|---|
| 1 | `port: vendor libsm64 (CC0 1.0) under port/third_party` | the vendored library, and the ignores that keep Nintendo's data out |
| 2 | `port: add the libsm64 module's portable core` | loader, clock, conversions, actors, surfaces, search paths |
| 3 | `port: add the libsm64 module's host test suite` | 23 unit + 9 integration tests, the double, the synthetic blob, the ABI test |
| 4 | `port: add the libsm64 module's Halo side` | settings, console commands, F3, the frame hook, the BSP adapter, the debug drawing |
| 5 | `port: build the libsm64 module, and libsm64 itself` | `--enable-libsm64`, the ninja rules, `build_libsm64.py` |
| 6 | `game: call the libsm64 module from the main loop and the console` | the three hooks in `main.c` and `console.c` |
| 7 | `port: add the libsm64 module's settings` | the `sm64.*` settings |
| 8 | `ci: let the CI builds include the libsm64 module` | `tools/ci_build.py --enable-libsm64`, and the suite |
| 9 | `docs: the libsm64 module's five documents` | README, design, decisions, build notes, test results |
| 10 | `docs: the libsm64 module's final report` | this file |
| 11 | `ci: build the libsm64 module in the Linux and Windows debug builds` | the workflow itself |

Commit 11 could not be pushed from the machine this was written on: the
GitHub App it authenticates with has no `workflows` permission, and GitHub
refuses to create or update `.github/workflows/*` without it. It is in the
local branch and as `patches/0011-ci-build-the-libsm64-module-in-the-Linux-and-Windows.patch`;
`git am` applies it, or push it from a machine with that permission. The
other ten are pushed.

**On the branch name.** The task asked for `feature/libsm64-port`; this session
is pinned to `arena/01a0ef64-haloce-custommarc` and cannot check another branch
out, so the work is committed there instead. Every commit is atomic and
self-contained, so `git checkout -b feature/libsm64-port dddaffd && git cherry-pick
dddaffd..HEAD` (or `git am patches/*.patch`) moves it to that name unchanged.

## What works

| | |
|---|---|
| **Simulated Mario in a level** | walks, runs, jumps, double- and triple-jumps, climbs ramps, is stopped by walls and ceilings — verified against a real libsm64 |
| **Collision from the level** | the BSP, converted: surfaces within 40 Halo units of the actor, fanned into triangles, wound from the BSP planes |
| **The player's controls** | `player_control`'s throttle and flags, camera-relative: Halo's own bindings work |
| **30 Hz, no drift** | one Halo tick is one Mario tick; the accumulator survives a jittery clock for a simulated hour |
| **Console** | `sm64_spawn`, `sm64_reset`, `sm64_enable`, `sm64_debug`, `sm64_scale`, `sm64_show_collision`, `sm64_status`, `sm64_help` |
| **Off means off** | without the flag, `configure.py` emits no rule for the module and `main.c`/`console.c` compile as they were |
| **Missing pieces degrade** | no library, no ROM, no floor: one line in the log, the game plays normally |

## What was verified, and how

| check | result |
|---|---|
| host suite, ROM-free double | **30 tests, 0 failed** |
| host suite, real libsm64 (no ROM) | **31 tests, 0 failed** |
| ABI mirror vs `libsm64.h` | every size and offset static-asserted: **OK** |
| vendored header vs the tests' copy | identical |
| the module's two units, in the Linux build's own configuration (clang, 32-bit x86) | compile clean, and clean under `-Wall -Wextra` |
| `main.c`, `console.c` with `--enable-libsm64` | compile clean |
| the same two, without it | compile clean (no trace of the module in `build.ninja`) |
| performance, real libsm64 | 1 actor 0.097 ms/frame, 5 actors 0.506, 10 actors 0.999 (of a 33 ms tick) |
| the repository's own tests | 359 passed; the 4 failures and 9 collection errors also fail on a clean checkout (missing `capstone`/`unicorn`, Windows-only tests) |
| building the vendored libsm64 offline, then running the suite against it | 31 tests, 0 failed |

Everything above was run here; `TEST_RESULTS.md` has the commands and the
output, `BUILD_NOTES.md` the exact build steps.

## What could not be done here

**The game executable was never linked.** `ninja linux` needs `clang -m32`,
32-bit glibc and 32-bit SDL3; this sandbox has none of them and `apt` has no
reachable mirror. What stands in its place is per-unit compilation through the
port's own ninja rules with a clang that targets 32-bit x86 — which is how the
three real bugs that only a game-configuration compile can show were found (the
`log` macro, the module's need for the game's full include path, and the
include directories that hold a space). A play session, and the BSP adapter
against a real level, are the two things still untested: on a machine with the
32-bit toolchain, `python3 configure.py --enable-libsm64 && ninja linux`, then
`sm64_status` and `sm64_spawn`, closes both.

**Static analysis** (`clang-tidy`, `scan-build`, Valgrind, ASan) is not
installed and could not be. `-Wall -Wextra`, the ABI assertions and the suite's
degradation and repeated-init tests stand in for it; `TEST_RESULTS.md` §7 says
what to run.

**The Windows rules were not generated**: `windows_build.py` only emits them on
a Windows host. They are written to the same shape as the Linux ones.

## What is not finished

1. **Mario is not drawn.** The module fills libsm64's geometry buffers; drawing
   them needs a model tag built at run time, and a wrong guess in the renderer
   costs a crash rather than a missing hat. `sm64.draw_mesh` is the switch.
2. **He is silent.** libsm64 synthesizes audio the host must mix, and the
   port's mixer has no door for a second source.
3. **He walks through moving objects** (vehicles, crates, doors): only the
   level's static collision is converted.
4. **Android**: no dynamic loader in the guest, so nothing could open the
   library.

Each is one piece of work with a clear shape, and `DECISIONS.md` says what it
would take.

## Licensing and what is not in this repository

Both projects are CC0 1.0. Nothing of Nintendo's is committed: the ROM, which
libsm64 reads at run time, is the user's own (`sm64.rom`, `*.z64` ignored); and
Mario's geometry, which libsm64's build fetches, is generated at build time
(`port/third_party/libsm64/src/decomp/mario/` ignored). The tests run against a
real libsm64 using synthetic, copyright-free asset data, so even they use
nothing of the game.

## Where things are

```
port/libsm64_port/
    README_LIBSM64_PORT.md   what it is, how to use it, the settings
    INTEGRATION_DESIGN.md    the design
    DECISIONS.md             every assumption, and what is not done and why
    BUILD_NOTES.md           the commands, and what could not be run here
    TEST_RESULTS.md          the numbers
    include/                 the module's API, its ABI mirror, the game's door
    src/                     the portable core and the Halo side
    tests/                   the host suite
    tools/                   builds libsm64, offline
port/third_party/libsm64/    the vendored libsm64
tools/libsm64_build.py       the ninja rules the ports share
source/main/{main,console}.c three hooks, behind the flag
port/linux/src/port_config.c the sm64.* settings
```
