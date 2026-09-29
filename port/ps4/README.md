# PS4 port (GoldHEN homebrew)

> Experimental: builds, but not yet run on a PS4. Refer to the status
> table in [PORTING.md](PORTING.md).

The game runs as a 32-bit-pointer (x32) guest inside a native PS4
launcher, as on Android. The package holds only the launcher. You copy the
game data (`maps/`) to `/data/halo` on the console.

| Document | Contents |
| --- | --- |
| [BUILDING-PS4.md](BUILDING-PS4.md) | Requirements, build, install, game data, settings, controls |
| [DEBUG.md](DEBUG.md) | klog, log files, crash reports, the Linux test host |
| [PORTING.md](PORTING.md) | Status of the acceptance tests, design, decisions, assumptions |

| Folder or file | Contents |
| --- | --- |
| `guest/` | The guest image's link script and start-up code |
| `host/` | The launcher: loader, system calls, files, threads, memory, GL ES 2 layer, shader translator, PS4 platform layer (`host_orbis.c`) |
| `include/halo_ps4_abi.h` | The guest's memory layout, shared by guest and host |
| `sce_sys/icon0.png` | The package icon (original artwork, no game assets) |
| `CMakeLists.txt` | The CMake build of `eboot.bin` (`build-ps4.sh`) |
| `host_imports.list` | The host functions that the guest imports |
