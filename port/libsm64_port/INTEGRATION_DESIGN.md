# How libsm64 is fitted into Halo CE Universal

This is the design the module in `port/libsm64_port` implements. It was written
before the code, and the code was then written to it; where the two disagree,
the code wins and `DECISIONS.md` says why.

## 1. Shape of the integration

```
        Halo CE                          the module                    libsm64
  ┌──────────────────────┐        ┌───────────────────────┐      ┌──────────────┐
  │ main_loop()          │        │ sm64_port_halo.c      │      │ libsm64.so   │
  │  sm64_port_halo_     │  C     │  settings, console,   │ C    │ (or sm64.dll)│
  │   update(dt)         ├───────►│  input, collision,    ├─────►│              │
  │ console_process_     │        │  debug drawing        │      │ loaded with  │
  │  command()           │        └──────────┬────────────┘      │ dlopen at run│
  └──────────────────────┘                   │                   │ time, never  │
                                  ┌──────────▼────────────┐      │ linked       │
                                  │ sm64_port_core.c      │      └──────────────┘
                                  │  portable: clock,     │
                                  │  conversion, actors,  │
                                  │  surfaces, loader     │
                                  └───────────────────────┘
```

Two rules shape everything:

1. **The module is a guest.** It is compiled only with
   `configure.py --enable-libsm64`, it is off unless `config.toml` says
   `sm64.enable = true`, it does nothing until someone types `sm64_spawn`, and
   when libsm64 or a ROM is missing it logs one line and stays out of the way.
   With the flag off, no port compiles a line of it and `main.c`/`console.c`
   are byte-for-byte what they were.
2. **The portable half knows nothing about Halo.** `sm64_port_core.c` talks to
   the game through a host structure of four function pointers (log, read file,
   free file) and is compiled and tested on its own
   (`port/libsm64_port/tests`): 31 tests against a real `libsm64.so` and
   against a ROM-free stand-in, on any machine, with no Halo build.

## 2. Coordinates, units, and which way is up

| | Halo CE | libsm64 |
|---|---|---|
| handedness | right, **Z up** | right, **Y up** |
| axes | +X east, +Y north, +Z up | +X/+Z ground, +Y up |
| unit | ~1 unit ≈ 1 metre (a player is ~1.6 tall) | 1 unit ≈ 1 cm at Mario's own scale |
| forward | (−sin yaw, cos yaw, 0) for yaw 0 = +Y | (sin θ, 0, cos θ) for θ = faceAngle |
| yaw | 0 = +Y, grows towards +X | 0 = +Z, grows towards +X |

The conversion (`sm64_port_core.c`) is a rotation and a scale:

```
halo → sm64:   x =  X / s      y =  Z / s      z = −Y / s
sm64 → halo:   X =  x * s      Y = −z * s      Z =  y * s
s = halo_units_per_sm64_unit (0.01 by default)
```

The sign of `sm64.z` is what makes Halo's +Y (north) point along SM64's −Z.
This is a right-handed rotation of −90° about the shared vertical, and it
preserves winding order, so a triangle that faces up in Halo faces up in SM64.

Yaw: `halo_yaw = sm64_yaw − π/2`, wrapped to (−π, π]. Verified by sweeping all
eight compass directions against a real libsm64: the error is 0.0° and the
walked distance identical (583.23 SM64 units from rest at full stick).

**Scale.** Mario's collision capsule is 160 SM64 units tall
(`decomp/game/object_stuff.c`), so at 0.01 Halo units per SM64 unit he is 1.6
Halo units tall: the same height as a standing player, and so able to use the
same doors, ramps and ledges. The scale is a setting (`sm64.scale`).

## 3. Time

Halo ticks 30 times a second (`TICKS_PER_SECOND`, `cseries.h`) and libsm64
expects exactly 30 ticks a second, so **one Halo tick is one Mario tick** and
no resampling is needed. The module still runs its own accumulator, because the
frame length varies (and with `display.interpolation` the port draws more
frames than ticks):

```
accumulator += dt                      (dt = halt_time_scale * seconds_elapsed)
while accumulator >= 1/30 and steps < 4:
        for each actor: sm64_mario_tick(...)
        accumulator -= 1/30
        steps += 1
```

The cap of four steps per frame is the anti-death-spiral: a frame longer than
133 ms (a stall, a debugger, a slow level load) makes Mario lose time rather
than freeze the game trying to catch up. The leftover stays in the accumulator,
so there is no drift: the tests run 3600 simulated seconds of a jittery clock
and find 107,000–109,000 ticks, exactly the 108,000 they should.

The accumulator is per module, not per actor: all actors step together, which
is what makes their interactions (`sm64_mario_interact_cap`, surfaces) behave.
`halt_time_scale` (0 when the game is paused or the console is open) simply
stops feeding it.

## 4. Input

The module takes the player's own movement intent, not raw keys, so it follows
whatever the player has bound:

| Mario | source |
|---|---|
| stick | `player_control_get(0)->throttle` (`.i` forward, `.j` strafe), turned into a world-space direction with the camera's facing (`player_control_get_facing_direction`), then into SM64 axes |
| A (jump) | `_player_control_jump_bit` of `control_flags` |
| B | `_player_control_action_bit` (the "use" key) |
| Z | `_player_control_back_bit` (crouch: Halo's player-control flags have no crouch of their own) |
| camera | `observer_get_camera(0)->forward`, flattened; the module tells libsm64 where the camera looks (`camLookX/camLookZ`), which is what makes "up" on the stick mean "away from the camera" |

The stick is built by rotating the world direction into camera space and
writing `camLook = −direction`, `stick = (0, magnitude)`: that pair is exact for
all eight directions (measured against a real libsm64). The magnitude is the
length of the throttle, deadzoned (`sm64.deadzone`, 0.08) and clamped to 1; it
maps to forward speed through libsm64's own curve (0.5 → 240 units, 1.0 → 583).

## 5. Collision

Halo's static collision is a BSP of surfaces, each a polygon of up to eight
points plus a plane; libsm64's is a flat array of triangles, searched **linearly
on every step** (`find_floor_from_list` walks every loaded surface). A Halo BSP
has tens of thousands of surfaces, so the whole level cannot be handed over:
at 4096 surfaces a step already costs ~1 ms for one actor.

The module therefore keeps a **window** around the actor:

- `SM64Port_BeginSurfaceUpdate()` / `AddPolygon()` / `EndSurfaceUpdate()`
  accumulate triangles; `SetStaticCollision()` swaps them in atomically.
- The game side walks the BSP (`global_collision_bsp_get()`), keeping every
  surface whose polygon's centre is within `sm64.collision_radius` (40 Halo
  units by default) of the actor, fans it into triangles, and fixes each
  triangle's winding from the BSP plane's normal, which decides which side
  libsm64 lets Mario stand on.
- It rebuilds when the actor has moved a quarter of the radius, or when
  `global_structure_bsp_index_get()` changes (a BSP switch). Rebuilding walks
  the whole BSP once — a few milliseconds — which is why it is rare.
- The set is capped at `sm64.max_surfaces` (4096). In a dense level the window
  simply stops at the cap; Mario still walks on what arrived first.
- With no level loaded (the main menu) the module builds its own room at the
  world origin (`SM64Port_BuildTestRoom`), so `sm64_spawn` works there too.

Dynamic collision (Halo's objects) is not converted: libsm64's surfaces are
global and rebuilding them each tick is exactly what the window exists to
avoid. Mario passes through vehicles, crates and doors that move.

## 6. Rendering

- **The actor** is drawn with the game's debug primitives
  (`render_debug.h`): a capsule 1.6 units tall and 0.74 wide
  (`render_debug_pill`), a line for the facing, a vector for the velocity, and
  a line of text with his action, speed and health
  (`render_debug_string_at_point`). This is what `sm64_debug` switches.
- **The collision** is drawn as wireframe from the same Halo-space triangles
  the adapter built (`sm64_show_collision`), capped at 512 triangles so that a
  dense window stays readable.
- **Mario himself** is not drawn yet. The module owns
  `SM64MarioGeometryBuffers` (positions, normals, colours, uvs) and fills them
  from libsm64 (`sm64_mario_geometry`), but Halo's renderer has no "draw this
  mesh now" door that does not go through a model tag, so showing him needs a
  real model-tag builder — see `DECISIONS.md`. `sm64.draw_mesh` is the switch
  for it and is off.

## 7. Audio

libsm64 synthesizes its own audio (`sm64_audio_tick`) through a buffer the host
must mix. The port's audio is SDL3, driven by the game's sound system, and has
no door for a second source that would not either fight the game's mixer or
need a mixing thread of its own. **Not wired.** Mario is silent; the module
does not call the audio entry points at all. `DECISIONS.md` says what it would
take.

## 8. State and the game's entities

`SM64PortState` (position, velocity, yaw, forward velocity, health, action,
animation id/frame, flags) is what the game reads. Three modes:

1. **Drawn only** (the default): the actor is a capsule in the world; the
   player's biped is untouched. Safe in a network game, because nothing the
   netcode simulates is modified.
2. **Possession** (`sm64.possess_player`): `object_set_position()` moves the
   player's own unit to Mario's position each frame, so the camera follows him.
   Single player only in spirit — it writes a simulated object, so in a network
   game the host would correct it.
3. **Query**: `sm64_status` prints ticks, surfaces and the last frame's cost.

## 9. Threading and memory

Everything runs on the game's main thread, between `observer_update()` and
`game_engine_update_non_deterministic()`. No locks are needed: libsm64's own
state is global and not thread-safe, and the module is only ever called from
there. Allocations happen at start-up (the actor pool, the surface set) and on
`sm64_spawn`/collision rebuild; a frame itself does not allocate.

## 10. Errors

| what | what happens |
|---|---|
| library not found | one log line at start-up, module off, game normal |
| ROM not found or not an SM64 US ROM | one log line, module off |
| `sm64_global_init` fails | module off; `sm64_status` says why |
| no floor under a spawn point | `sm64_mario_create` returns −1: the console says so, no actor |
| surface set over the cap | the extra surfaces are dropped, a line is logged once |
| frame longer than 133 ms | steps are capped at 4: Mario loses time, the game does not stall |

Every failure is a message and a no-op, never a fatal error: the port's own
rule is that a missing extra must not stop the game from running.

## 11. Feature flag

- `configure.py --enable-libsm64` → `-DHALO_CE_ENABLE_LIBSM64=1` for every unit
  plus the module's two units and the library rule. Off by default; without it,
  `main.c` and `console.c` compile exactly as before.
- `config.toml` `sm64.enable` (default `false`) → the module starts at all.
- Console `sm64_enable 0|1` → start/stop at run time.
- Android: not built. Its guest is a static binary with a bundled libc that has
  no dynamic loader, so nothing could open `libsm64.so`.

## 12. Files

| file | what |
|---|---|
| `include/sm64_port.h` | the module's public API (no Halo types) |
| `include/sm64_port_abi.h` | mirrors of libsm64's structs, static-asserted against the real header |
| `include/sm64_port_halo.h` | the four functions the game calls |
| `src/sm64_port_core.c` | portable core |
| `src/sm64_port_halo.c` | settings, console, frame, BSP adapter, debug drawing |
| `tests/` | host test suite (see `TEST_RESULTS.md`) |
| `tools/build_libsm64.py` | builds the vendored libsm64 |
| `tools/offline_import_geo.py` | its geometry import without network access |
| `../../third_party/libsm64` | the vendored libsm64 (CC0 1.0) |
