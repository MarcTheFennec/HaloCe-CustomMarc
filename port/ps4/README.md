# PS4 (homebrew)

`ninja ps4` builds the game's PS4 host, `build/ps4/eboot.bin`, and
`ninja ps4_pkg` makes a package from it:
`build/ps4/pkg/IV0000-HALO00001_00-HALOCE0000000000.pkg`.

The port is for a jailbroken console with **GoldHEN on firmware 9.00 or
later**. It is in development; [PLAN.md](PLAN.md) gives the design, the
milestones and the state of the work. **Milestone 0** is what builds now:
the host starts, finds the game data, opens the display, reads the
controllers and writes a log. The game itself arrives with the next
milestones.

The PS4 port follows the Android port: the game is 32-bit-pointer code
(x32: x86-64 instructions with 32-bit pointers) in a *guest image* that a
64-bit *host* application loads. The host is in `host/`. It is built with
the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).

## Requirements

You do not need Sony's SDK. You need the tools of the Linux build (Python,
ninja) and:

- The OpenOrbis PS4 Toolchain, v0.5.4 or later, unpacked somewhere.
  `configure.py` looks for it in `OO_PS4_TOOLCHAIN`; the option
  `--ps4-toolchain` names a different folder.
- clang and ld.lld (LLVM 15 or later). The options `--ps4-cc` and
  `--ps4-ld` select different ones.

## Build the package

1. Go to the root folder of the repository.
2. Enter `export OO_PS4_TOOLCHAIN=/path/to/toolchain`.
3. Enter `python configure.py`.
4. Enter `ninja ps4_pkg`.

`tools/ci_build.py ps4 release` makes the same build as GitHub Actions and
puts the package in `dist/halo-ps4-release/`.

Give `configure.py --ps4-log-address 192.168.1.10:9999` to make a build
that also sends its log to that computer over UDP (`nc -ulk 9999`).

## Install the game

1. Install the package with the package installer of GoldHEN, or from a
   USB stick.
2. Copy the game data and the two Sony modules to the console (below).
3. Start "Halo: Combat Evolved" from the home screen.

### Game data

**The package does not contain the game data, and the console does not
extract disc images.** Extract the `maps/` folder from an Xbox disc image
(`.xiso` or `.iso`) of any version of the game with the Linux or Windows
version of this port (refer to the main [README](../../README.md#game-data)).
Then copy it to the console with the FTP server of GoldHEN (port 2121) or
with PS4Xplorer:

| Item | Path on the console |
| --- | --- |
| Game data (approximately 1.8 GB) | `/data/halo/maps/` |
| Saved games (`z:\` and `u:\`) | `/data/halo/save/` |
| Settings | `/data/halo/config.toml` |
| Log | `/data/halo/debug.txt` |
| Sony modules (below) | `/data/halo/modules/` |

The game also looks for `maps/` in `/mnt/usb0/halo/` to `/mnt/usb3/halo/`,
so a USB stick with a `halo/maps/` folder operates too. The first folder
with `maps/` in it becomes the data root; the saves, the settings and the
log go there.

The game makes `/data/halo` and its subfolders at the first start, so
you can start the game one time to get the folders, then copy the files.

### Sony modules

The game draws with OpenGL ES 2 through Piglet, Sony's implementation.
Retail firmware has Piglet (`libScePigletv2VSH.sprx`) but not the shader
compiler that goes with it (`libSceShaccVSH.sprx`). As for RetroArch,
LÖVE and the Super Mario 64 port on PS4, copy both files to
`/data/halo/modules/`. They are in the 4.74 development-kit firmware and
in `RetroArch_PS4_r4.pkg`; this project cannot distribute them.

Without them, the game shows a message at start-up and stops. The log
says which module it did not find.

## Controls

The first controller is player 1; the controllers of the other logged-in
users are players 2 to 4 (split screen). To add a player, pair a
controller and push its PS button; select a guest user. The buttons agree
with the positions on the Xbox controller:

| DualShock 4 / DualSense | Xbox | Function in the game |
| --- | --- | --- |
| left stick, right stick | left stick, right stick | move, look |
| R2 | right trigger | fire |
| L2 | left trigger | throw a grenade |
| Cross | A | jump, accept |
| Circle | B | melee, back |
| Square | X | action, reload |
| Triangle | Y | change the weapon |
| L1 | white | flashlight |
| R1 | black | change the grenade |
| L3, R3 | left and right stick clicks | crouch, zoom |
| D-pad | D-pad | |
| Options | start | pause menu |
| Share / Create | back | |

Hold Options and Circle together for one second to leave the game
(milestone 0: the only way out of the test screen).

## Settings

The settings are the settings of Linux, without the window, the mouse
and the paths. Refer to [port/linux/README.md](../linux/README.md#settings).
The file is `/data/halo/config.toml`; the game writes it with the default
values at the first start. (Milestone 2 and later.)

## Find problems

The host writes its log to `/data/halo/debug.txt` and to the kernel log,
which GoldHEN shows on TCP port 3232 (`nc <console> 3232`). At start-up the
log gives:

- the data root and whether `maps/` was found;
- the result of the memory probe: whether the fixed addresses the game
  needs (`0x80000000`, `0x88000000`) can be mapped, and how much flexible
  memory the process has;
- the Piglet version, renderer and extensions, and whether the shader
  compiler is present;
- each controller and, in milestone 0, every change of its buttons.

## Files

| Path | Contents |
| --- | --- |
| `PLAN.md` | The design, the decisions and the milestones. |
| `include/halo_ps4_abi.h` | The contract between the host and the guest: addresses, the image header, the storage paths. |
| `host/` | The host application: `host_main.c` (start-up, data root, main loop), `host_video.c` (Piglet), `host_pad.c` (scePad), `host_debug.c` (log, message dialog), `host_memory.c` (memory probe, later the guest allocator). |
| `pkg/sce_sys/icon0.png` | The icon of the package. |
| `../../tools/ps4_build.py` | The ninja rules: host, fake-signed ELF, `param.sfo`, project file, package. |
| `../../tools/test_ps4_port.py` | Tests of the build rules and the ABI header. |
