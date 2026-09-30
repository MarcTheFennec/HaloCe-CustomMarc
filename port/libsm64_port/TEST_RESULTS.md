# Test results

Everything below was run in this workspace, on this machine, and is
reproducible with the commands given. Where a test could not be run the heading
says so.

Summary: **32 tests against a real `libsm64.so`, 31 against the ROM-free test
double, 0 failures either way; the ABI mirror is static-asserted against
libsm64's own header; the module's two units, and `main.c` and `console.c`,
compile in the Linux build's own configuration; the library the game loads is
built for its 32-bit ABI.** The full game executable was not linked here
(`BUILD_NOTES.md` says why), so the tests are the host suite plus per-unit
compilation, not a play session.

## 1. The host suite against the test double

```
cd port/libsm64_port/tests && make test
```

```
unit tests
  ok   conversions: Halo -> SM64 -> Halo is the identity
  ok   conversions: known points
  ok   conversions: directions keep their length
  ok   conversions: yaw round-trips and is a quarter turn
  ok   conversions: a yaw means the same direction in both systems
  ok   clock: 60 Hz frames give one tick every other frame
  ok   clock: 30, 60 and 144 frames a second all produce 30 ticks a second
  ok   clock: no drift over a simulated hour at 60 Hz
  ok   clock: a stalled frame is clamped, not paid back
  ok   clock: a zero or backwards frame changes nothing
  ok   clock: a variable frame rate still averages 30 ticks a second
  ok   input: below the dead zone the stick is centred
  ok   input: the camera look is minus the wanted direction, the stick is forward
  ok   input: the analogue magnitude reaches the stick
  ok   input: invert_y mirrors the forward axis, sensitivity scales it
  ok   input: buttons pass straight through
  ok   surfaces: a triangle is added in SM64 units
  ok   surfaces: the winding follows the normal libsm64 expects
  ok   surfaces: a polygon is fanned, degenerate polygons are refused
  ok   surfaces: the set stops at its capacity instead of overrunning
  ok   surfaces: the test room builds and is loadable
  ok   degradation: a missing library leaves the module inert
  ok   degradation: the ROM is looked for in the search directory, then beside the game
  (23 unit tests, 0 failed)

integration tests
  ok   integration: an actor walks where it is told, in four directions
  ok   integration: a jump leaves the ground and comes back
  skip integration: the double and triple jump (real libsm64 only)
  ok   integration: a wall stops the actor
  ok   integration: the actor walks up the ramp
  ok   integration: a ceiling stops an upward jump
       1 actor(s): 0.3 ms for 300 frames (0.001 ms a frame, 300 ticks)
       5 actor(s): 0.6 ms for 300 frames (0.002 ms a frame, 600 ticks)
      10 actor(s): 1.1 ms for 300 frames (0.004 ms a frame, 900 ticks)
  ok   integration: many actors, and what they cost
  ok   integration: the actor limit is enforced
  ok   integration: initialising and shutting down repeatedly is clean

31 tests, 0 failed
tests/libsm64.h matches the vendored libsm64's
```

## 2. The same suite against a real libsm64, with no ROM

```
HALO_SM64_GEO_CACHE=<cache> python3 port/libsm64_port/tools/build_libsm64.py \
        --out /tmp/libsm64.so
cd port/libsm64_port/tests && SM64_PORT_TEST_LIBRARY=/tmp/libsm64.so ./test_sm64_port
```

```
test_sm64_port: library "/tmp/libsm64.so" (real libsm64: 32 of 32 tests run)
test_sm64_port: 8192 KiB of placeholder data (no ROM: synthetic textures and animations)

... the same 22 unit tests, 0 failed ...

integration tests
  ok   integration: an actor walks where it is told, in four directions
  ok   integration: a jump leaves the ground and comes back
  ok   integration: the second and third jumps go higher (real libsm64)
  ok   integration: a wall stops the actor
  ok   integration: the actor walks up the ramp
  ok   integration: a ceiling stops an upward jump
       1 actor(s): 30.5 ms for 300 frames (0.102 ms a frame, 300 ticks)
       5 actor(s): 158.2 ms for 300 frames (0.527 ms a frame, 600 ticks)
      10 actor(s): 315.3 ms for 300 frames (1.051 ms a frame, 900 ticks)
  ok   integration: many actors, and what they cost
  ok   integration: the actor limit is enforced
  ok   integration: initialising and shutting down repeatedly is clean

32 tests, 0 failed
```

The double/triple-jump test is skipped against the double (whose simplified
physics has only one jump) and runs against the real library.

### Performance, real libsm64, 300 frames each (host, `cc -O1`, 64-bit)

| actors | total | per frame | per actor per tick |
|---|---|---|---|
| 1 | 30.5 ms | 0.102 ms | 0.10 ms |
| 5 | 158.2 ms | 0.527 ms | 0.26 ms |
| 10 | 315.3 ms | 1.051 ms | 0.32 ms |

The port's budget is a 33 ms tick that already runs the whole game; one actor
costs 0.3 % of it, ten cost 3 %. The per-actor cost *falls* with more actors
because the fixed part (the library call, the surface lookup set-up) is shared.
Measured with the game's own 4096-surface cap and libsm64's linear surface
search, which is why the cap is what bounds a step.

### Physics ground truth measured against the real library

| what | value |
|---|---|
| ticks a second | 30 (one Halo tick is one Mario tick) |
| gravity | −4.00 SM64 units/tick² |
| jump velocity | 38.00, peak 96.50, 11 ticks airborne |
| double jump peak | 147.36 |
| forward unit vector | (sin faceAngle, 0, cos faceAngle) |
| state's faceAngle | `faceAngle[1] / 32768.0 * π` |
| full stick speed | 583.23 SM64 units after 300 ticks |
| 8-direction walk error | 0.0°, same distance every direction |
| rest action / health | `0x0C400201` / 2176 (8 wedges of 272) |

## 3. The ABI mirror

```
cd port/libsm64_port/tests && make test_abi && ./test_abi
```

Compares `sizeof` and every field offset of the module's mirrors
(`SM64Surface`, `SM64MarioInputs`, `SM64MarioState`,
`SM64MarioGeometryBuffers`, `SM64ObjectTransform`) with the vendored
`libsm64.h`, and the texture-atlas and triangle-count constants. It is a
compile-time test (negative-size arrays), so a mismatch stops the build rather
than failing at run time: **ABI OK**. `make check-header` also diffs the
vendored header against the copy the tests hold:

```
tests/libsm64.h matches the vendored libsm64's
```

## 4. Compiled in the game's own configuration

The two units of the module were compiled through the port's own `ninja` rules
(its flags, its include path, its generated MSVC-semantics header), with clang
targeting 32-bit x86:

```
python3 configure.py --enable-libsm64 --linux-cc <clang for 32-bit x86>
ninja build/linux/libsm64_port/libsm64_port_sm64_port_core.o \
      build/linux/libsm64_port/libsm64_port_sm64_port_halo.o
```

```
[1/2] LINUX CC build/linux/libsm64_port/libsm64_port_sm64_port_halo.o
[2/2] LINUX CC build/linux/libsm64_port/libsm64_port_sm64_port_core.o
```

and separately with `-Wall -Wextra`, which produced no diagnostic from either
file. This is what found the `log` macro collision (`DECISIONS.md` §14).

## 4b. The library is built for the game's ABI

The game is 32-bit, so the library it opens has to be:

```
python3 configure.py --enable-libsm64
ninja build/linux/libsm64.so
readelf -h build/linux/libsm64.so | grep Class     # ELF32
```

and it is built by the port's own compiler and flags
(`--target=i686-linux-gnu -m32`), not by whatever `cc` happens to be.

## 4c. What CI runs

```
python3 tools/ci_build.py linux debug --enable-libsm64     # the game, linked, with the module
make -C port/libsm64_port/tests test_sm64_port test_abi double
(cd port/libsm64_port/tests && ./test_abi && ./test_sm64_port)                  # 31 tests, 0 failed
python3 port/libsm64_port/tools/build_libsm64.py --out /tmp/libsm64-host.so     # for the runner
(cd port/libsm64_port/tests && SM64_PORT_TEST_LIBRARY=/tmp/libsm64-host.so ./test_sm64_port)   # 32 tests, 0 failed
```

The tests cannot use the game's library: it is 32-bit, and a test binary built
on the runner cannot open it. Both runs were reproduced here (§1 and §2).

## 5. The build graph, flag on and off

```
python3 configure.py                 # grep -c libsm64 build.ninja  ->  0
python3 configure.py --enable-libsm64  # grep -c libsm64 build.ninja  ->  16
ninja -n build/linux/libsm64.so      # [0/1] LIBSM64 build/linux/libsm64.so
```

With the flag off, no port compiles a unit of the module, `main.c` and
`console.c` compile exactly as before, and nothing of libsm64 is built.

## 6. Building the vendored libsm64 offline

```
HALO_SM64_GEO_CACHE=<dir of geo.inc.c and model.inc.c> \
python3 port/libsm64_port/tools/build_libsm64.py --out /tmp/libsm64.so --jobs 8
```

```
offline_import: wrote .../port/third_party/libsm64/src/decomp/mario
build_libsm64: /tmp/libsm64.so       (1,637,600 bytes)
```

and that library then passes the suite above (§2), end to end, with no network
and no ROM.

## 7. Static analysis

`clang-tidy` and `scan-build` are not installed here and could not be
installed (`apt` has no reachable mirror), so no static-analysis report is
included. What stands in for it:

- `-Wall -Wextra` on the suite and on the module's units: clean.
- The game build's own `-w`, so nothing is hidden there that also passes here.
- The ABI static assertions (§3).
- The suite's degradation test (a missing library leaves the module inert) and
  the repeated init/shutdown test, which is the shape of run-time abuse that
  would otherwise show up as a leak.

Valgrind is not installed either; ASan/UBSan runs of the suite are the natural
next step on a machine with them (`make CFLAGS="-std=gnu99 -g -O1 -fsanitize=address,undefined"`).

## 8. What could not be tested here

| what | why | what would close it |
|---|---|---|
| a linked `build/linux/halo` | no `clang -m32`, no 32-bit glibc, no 32-bit SDL3; `apt` unusable | `ninja linux` on a machine with them (see `BUILD_NOTES.md`) |
| a play session (`sm64_spawn`, walking on a real BSP) | needs the above, and the game data | the same, then the console commands |
| the BSP collision adapter against a real level | needs a running game | the same; the adapter's inputs are the only untested surface (it is exercised by the tests through the same `AddPolygon` API) |
| Valgrind / ASan | not installed | one command, §7 |
| the Windows rules | `windows_build.py` only generates them on a Windows host | `configure.py --enable-libsm64 && ninja windows` there |
