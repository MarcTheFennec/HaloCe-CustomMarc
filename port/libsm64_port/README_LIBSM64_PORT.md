# The libsm64 module

libsm64 is [libsm64](https://github.com/libsm64/libsm64) — *Super Mario 64*'s
Mario as a C library — running inside Halo CE Universal: a real, simulated
Mario, controlled by the player, standing on the level's own collision.

It is **off by default** and **additive**: it is not compiled unless the build
is configured with `configure.py --enable-libsm64`, it does nothing unless
`config.toml` says `sm64.enable = true`, and it does not touch the game until
someone types `sm64_spawn`. When libsm64 or a ROM is missing, it logs one line
and stays out of the way.

- [How to use it](#using-it) — the console commands and the settings
- [INTEGRATION_DESIGN.md](INTEGRATION_DESIGN.md) — the design: coordinates,
  units, timestep, input, collision, rendering, audio, errors
- [DECISIONS.md](DECISIONS.md) — every assumption, and what is *not* done and
  why (Mario is not drawn; audio is not wired; Android is not supported)
- [BUILD_NOTES.md](BUILD_NOTES.md) — the exact commands, and what could not be
  run in the sandbox this was written in
- [TEST_RESULTS.md](TEST_RESULTS.md) — 31 tests against a real libsm64, 0
  failures; performance numbers

## What you need

1. **A build with it.** `python3 configure.py --enable-libsm64` (then
   `ninja linux`, or `ninja windows` on Windows). This builds the module's two
   units and `libsm64.so` (or `sm64.dll`) from the vendored source in
   `port/third_party/libsm64`. The CI builds (`tools/ci_build.py ...
   --enable-libsm64`) ship the library beside the executable, so a downloaded
   build already has this.
2. **Your own Super Mario 64 US ROM** (`baserom.us.z64`). libsm64 reads Mario's
   textures and animation data out of it at start-up. **It is not part of this
   project and is never committed**: supply your own copy. Nothing is copied
   out of it, and nothing of it is stored anywhere.

With either missing, the game plays normally and the console says which one it
was.

### Where to put the ROM

In the order the module looks, first match wins:

1. **`sm64.rom` in `config.toml`** (or the `HALO_SM64_ROM` environment
   variable) — a path to anywhere you like:
   ```toml
   sm64.rom = "/home/you/games/baserom.us.z64"
   ```
2. **The game's data folder** — the one that holds `maps/`, whatever
   `paths.data` points at (by default `assets/` next to the executable). This
   is the recommended place: the game finds it wherever it is started from.
3. **The working directory** — so, if you start the game with `./halo` from
   `build/linux`, right next to `halo` and `libsm64.so`.

In 2 and 3 the name is one of `baserom.us.z64`, `sm64.us.z64`,
`baserom.us.n64`, `baserom.z64`, `sm64.z64`. `sm64_status` in the console says
plainly whether a ROM was found, and if not, where it looked.

The library is looked for the same way (`sm64.library`, then the data folder,
then beside the executable), under the name `libsm64.so` or `sm64.dll`.

## Using it

**Press F3** to switch the module on: it loads libsm64, reads the ROM, and puts
an actor in front of the player. Press F3 again to switch it off. That is all
that is needed — every command below is optional. (`sm64.enable = true` in
`config.toml` turns it on as the game starts instead. F3 is also one of the
editor's debug keys, which do nothing outside the editor.)

In the console (`~`):

| command | what |
|---|---|
| `sm64_spawn` | put an actor a few units in front of the player |
| `sm64_spawn x y z` | put one at a position, in Halo units |
| `sm64_reset` | remove it |
| `sm64_enable 0` / `1` | stop and start the module (and libsm64 with it) |
| `sm64_debug 0` / `1` | draw the actor: capsule, facing, velocity, what he is doing |
| `sm64_show_collision 0` / `1` | draw the level surfaces he is standing on |
| `sm64_scale 0.01` | Halo units per SM64 unit (takes effect on the next start) |
| `sm64_status` | actors, surfaces, ticks, and the last frame's cost |
| `sm64_help` | the list |

`sm64_status` is the one to try first: it says plainly whether the module is
running, and if it is not, why (`no library`, `no ROM`, `no floor to stand
on`).

### Settings (`config.toml`)

| setting | default | what |
|---|---|---|
| `sm64.enable` | `false` | start the module at all |
| `sm64.rom` | `""` | where the US ROM is; empty looks in the game's data folder and then the working directory, for `baserom.us.z64` (then `sm64.us.z64`, `baserom.us.n64`, `baserom.z64`, `sm64.z64`) |
| `sm64.library` | `""` | where libsm64's shared library is; empty looks in the game's data folder and then beside the executable, for `libsm64.so` (`sm64.dll` on Windows), then in the system's library paths |
| `sm64.scale` | `0.01` | Halo units per SM64 unit: 0.01 makes Mario 1.6 units tall, the height of a player |
| `sm64.tick_rate` | `30.0` | his steps a second; 30 is the game's own tick rate |
| `sm64.deadzone` | `0.08` | movement below this is not sent to him |
| `sm64.sensitivity` | `1.0` | how far he turns for what the player asks |
| `sm64.invert_y` | `false` | backwards means forwards |
| `sm64.collision_radius` | `40.0` | how much of the level around him becomes his collision, in Halo units |
| `sm64.max_surfaces` | `4096` | the most surfaces at once: libsm64 searches them one after another, so this is what bounds the cost of a step |
| `sm64.debug` | `true` | draw him |
| `sm64.show_collision` | `false` | draw his collision |
| `sm64.draw_mesh` | `false` | reserved for drawing Mario himself (not finished) |
| `sm64.possess_player` | `false` | move the player's own unit to where he is, instead of only drawing him |

The game writes the file with every setting at its default the first time it
runs, and adds any setting that is new to a build.

## How it fits together

```
main_loop()  ──►  sm64_port_halo_update(dt)   [src/sm64_port_halo.c]
                     │  settings, console commands, the player's input,
                     │  the level's collision BSP, debug drawing
                     ▼
                  sm64_port_core.c            (portable: knows no Halo)
                     │  fixed 30 Hz accumulator, coordinate conversion,
                     │  surface sets, actor life cycle, dlopen
                     ▼
                  libsm64.so                  (loaded at run time)
```

- **Time:** Halo ticks 30 times a second and libsm64 wants 30, so one Halo
  tick is one Mario tick; an accumulator smooths variable frame lengths and
  gives up after four steps in a frame (133 ms) rather than stalling the game.
- **Collision:** a window of the level around the actor (40 units by default,
  at most 4096 surfaces), rebuilt when he walks a quarter of it or the BSP
  changes, because libsm64 searches its surfaces linearly on every step and a
  whole Halo BSP has tens of thousands.
- **Input:** the player's own movement intent (`player_control`'s throttle and
  control flags) turned into camera-relative stick and buttons, so Halo's own
  bindings work: jump is A, "use" is B, "back" is Z.
- **Scale:** 0.01 Halo units per SM64 unit, which makes him 1.6 units tall.

## What is not finished

- **Mario is not drawn.** He is a capsule with a facing line and a line of
  text. The module fills libsm64's geometry buffers, but showing them needs a
  model tag built at run time; see `DECISIONS.md` §9.
- **He is silent.** libsm64 synthesizes its own audio and the port's mixer has
  no door for a second source; see `DECISIONS.md` §10.
- **He walks through moving objects.** Only the level's static collision is
  converted; see `DECISIONS.md` §7.
- **Not on Android.** Its guest has no dynamic loader, so nothing could open
  the library; see `DECISIONS.md` §12.

## Licences

Both libsm64 and this port are **CC0 1.0 Universal**
(`port/third_party/libsm64/LICENSE.md`).

The module contains none of Nintendo's data. libsm64 reads Mario's textures and
animations from **your** ROM at run time, and fetches (or, offline, rewrites
from a local copy) Mario's geometry at build time; neither is committed, and
both are gitignored (`*.z64`,
`port/third_party/libsm64/src/decomp/mario/`). The tests run against a real
libsm64 using a synthetic, copyright-free stand-in for the parts of the ROM it
reads, so even they use nothing of the game.
