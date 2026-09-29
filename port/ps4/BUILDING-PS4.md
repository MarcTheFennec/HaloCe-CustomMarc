# Halo: Combat Evolved on the PS4 (GoldHEN)

> **Status: experimental, not yet run on a PS4.** The launcher builds,
> links and is fake-signed. The same host code runs the game on Linux
> without a display. Tests on a console are still to do. Refer to the
> status table in [PORTING.md](PORTING.md).

The PS4 port is a homebrew application for jailbroken consoles with
[GoldHEN](https://github.com/GoldHEN/GoldHEN). The package (PKG) contains
only the launcher. It does not contain game data. You copy your own game
data to the console.

## Requirements

On the console:

- A PS4 or PS4 Pro with a firmware that GoldHEN supports: 5.05, 6.72,
  7.02, 7.55, 9.00 or 11.00. Use GoldHEN 2.3 or newer.
- The game data (the `maps/` folder). Refer to [Game data](#game-data).
- The two shader compiler modules. Refer to
  [Shader compiler modules](#shader-compiler-modules).

DECISION: the port targets all GoldHEN firmwares. The launcher uses only
libkernel, libScePad, libSceAudioOut, libSceUserService,
libSceSystemService, libSceNet and Piglet (the system OpenGL ES library).
These libraries are the same on all of those firmwares. PORTING.md records
the firmware of each test.

On the computer that builds the port (Linux, or Windows with WSL):

- clang 15 or newer and ld.lld, python 3, CMake 3.16 or newer, ninja and
  git.
- The OpenOrbis PS4 toolchain. Use one of these:
  - An [OpenOrbis-PS4-Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
    release. Set `OO_PS4_TOOLCHAIN` to its folder.
  - `tools/ps4_setup_toolchain.sh [prefix]`. The script builds the
    toolchain from source into `prefix` (default `/opt`). This also
    builds create-fself (it needs Go) and OpenOrbis musl. The link stubs
    for the system libraries are made from the public
    [ps4libdoc](https://github.com/idc/ps4libdoc) symbol lists. The build
    does not use Sony code.
- For the `.pkg` file only:
  [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg)'s `PkgTool.Core`
  (it needs .NET). Without it, the build stops after the staged package.
  You can then build the `.pkg` on another computer.

## Build

In the root folder of the repository:

```sh
./build-ps4.sh
```

The script does the steps configure, compile, link, create-fself and
create-pkg:

1. CMake configures `port/ps4` with `cmake/toolchain-ps4.cmake`.
2. The repository's ninja build makes the game image
   (`build/ps4/halo_guest.elf`). The game is 32-bit (x32) code that runs
   inside the native launcher.
3. CMake compiles the launcher with the game image embedded and links it
   with OpenOrbis' `link.x` (`ps4_add_executable` in `cmake/ps4.cmake`).
4. create-fself makes `build/ps4/cmake/eboot.bin` (`ps4_create_fself`).
5. `tools/ps4_package.py` puts `eboot.bin`, `sce_sys/param.sfo` and
   `sce_sys/icon0.png` in `build/ps4/cmake/package/pkg_root` and writes
   `halo.gp4`. If PkgTool.Core is present, it makes
   `build/ps4/cmake/package/IV0000-HCEP02342_00-HALOCECUSTOMMARC.pkg`
   (`ps4_create_pkg`).

| Option | Function |
| --- | --- |
| `--toolchain DIR` | The OpenOrbis toolchain folder (default: `$OO_PS4_TOOLCHAIN`, `$PS4_OPENORBIS` or `/opt/oo`). |
| `--no-pkg` | Stop after `eboot.bin`. |
| `--require-pkg` | Fail if the `.pkg` cannot be made. |

Set `PKGTOOL` to the PkgTool.Core command if it is not on the `PATH`, for
example `PKGTOOL="dotnet /opt/PkgTool.Core/PkgTool.Core.dll"`.

To make only the package from an existing `eboot.bin`:

```sh
./package-ps4.sh                      # build/ps4/package
PkgTool.Core pkg_build build/ps4/package/halo.gp4 build/ps4/package
```

`package-ps4.sh` never adds game data. `--assets-dir DIR` is off by
default. It adds only extra launcher files and refuses Halo data files
(`.map`, `.xbe` and similar).

The ninja build can also make `eboot.bin` alone:

```sh
python3 configure.py --ps4 [--ps4-openorbis DIR]
ninja ps4_eboot          # build/ps4/eboot.bin
ninja ps4_host_linux     # build/ps4/halo_ps4_headless, the test host (DEBUG.md)
```

The Linux, Windows and Android builds do not change. `--ps4` only adds
targets.

## Install the package

1. Copy the `.pkg` file to a USB drive (exFAT or FAT32) or to the
   console over FTP.
2. On the console, open GoldHEN's package installer (or
   **Settings > Debug Settings > Game > Package Installer**) and install
   the package.
3. The home screen shows "Halo CE CustomMarc".

## Shader compiler modules

The PS4 system software does not include the OpenGL ES shader compiler.
Only the firmware of development kits has it. The launcher cannot compile
shaders without two modules from a 4.74 development firmware:

- `libScePigletv2VSH.sprx`
- `libSceShaccVSH.sprx`

These modules are Sony software. This project does not include them and
cannot give them to you. Get them from a 4.74 devkit or testkit firmware
that you have. Copy them to one of these folders:

- `/data/halo/lib/` (recommended)
- `/data/self/system/common/lib/`

If the modules are missing, the launcher shows a notification and stops.
`host.log` then gives the folders that it examined.

## Game data

The package does not include game data. Get the `maps/` folder as the
[main README](../../README.md#game-data) tells: the Linux or Windows
version extracts it from your Xbox disc image.

The launcher looks for each file in these folders, in this sequence:

1. `/data/halo/<file>` (the internal storage, recommended)
2. `/mnt/usb0/halo/<file>` (a USB drive)
3. `/app0/<file>` (the package; it has no game data)

`host.log` records the folder that supplied each file. It also records
each missing file with the full paths that the launcher examined. File
names do not need the correct uppercase and lowercase letters. `\` in
paths operates as `/`.

The folders on the console:

| Folder | Contents |
| --- | --- |
| `/data/halo/maps/` | The game data: `ui.map`, `a10.map`, `bloodgulch.map`, ... (you copy them) |
| `/data/halo/bitmaps/`, `sounds/` | Optional extra data (you copy them) |
| `/data/halo/lib/` | The shader compiler modules (you copy them) |
| `/data/halo/config/` | `config.toml` (the game's settings) and `ps4.toml` (the launcher's settings) |
| `/data/halo/save/` | Saved games and player profiles |
| `/data/halo/logs/` | `host.log`, `debug.txt` and `crash.log` |

The launcher makes the folders that it writes to.

### Copy the data with FTP

1. On the console, enable the FTP server in GoldHEN's settings. The
   server uses port 2121.
2. Find the IP address of the console: **Settings > Network > View
   Connection Status**.
3. On the computer:

   ```sh
   scripts/upload-maps.sh <PS4_IP> path/to/maps
   ```

   The script copies the folder to `/data/halo/maps` with curl. To copy a
   different folder, give the remote folder name:
   `scripts/upload-maps.sh <PS4_IP> path/to/sounds sounds`.

An FTP program (for example FileZilla) also operates: connect to
`<PS4_IP>` port 2121 without a password and copy `maps/` to `/data/halo/`.

### Copy the data with PS4 Xplorer

1. Put the `halo/maps/` folder on a USB drive.
2. In PS4 Xplorer, copy `/mnt/usb0/halo` to `/data/`.

You can also keep the data on the USB drive (`/mnt/usb0/halo/maps/`). The
launcher reads it there, but the internal storage is faster.

## Settings

The game's settings are in `/data/halo/config/config.toml`, as on the
other platforms (refer to [port/linux/README.md](../linux/README.md)).

The launcher's settings are in `/data/halo/config/ps4.toml`:

```toml
[log]
level = "info"          # debug, info, warn, error
[monitor]
seconds = 60            # memory report interval in seconds, 0 = off
fps_seconds = 10        # frame rate report interval in seconds, 0 = off
[video]
height = 1080           # 1080 or 720
vsync = true
```

## Controls

The DualShock 4 operates controller 1:

| DualShock 4 | Xbox controller | Function in the game |
| --- | --- | --- |
| left stick | left stick | move |
| right stick | right stick | aim |
| Cross | A | jump, accept |
| Circle | B | melee, back |
| Square | X | action, reload |
| Triangle | Y | change the weapon |
| R2 | right trigger | fire |
| L2 | left trigger | throw a grenade |
| L1 | white | flashlight |
| R1 | black | change the grenade |
| L3 | left stick click | crouch |
| R3 | right stick click | zoom |
| D-pad | D-pad | |
| Options | start | pause menu |
| touchpad click | back | scoreboard (multiplayer) |

If the controller disconnects, the game continues without input. The
controller operates again when it connects again (`host.log` records
both events). The controller vibrates when the game uses the Xbox
controller's motors.

## Problems

Refer to [DEBUG.md](DEBUG.md).
