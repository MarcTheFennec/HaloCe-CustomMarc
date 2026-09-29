# Debugging the PS4 port

The port does not use Sony's debugging tools. It uses the kernel log
(klog), its own log files and a test host that runs on Linux.

## The logs

| Log | Contents |
| --- | --- |
| klog | Each line of `host.log`, while the launcher runs. |
| `/data/halo/logs/host.log` | The launcher: start, content folders, each file that the game opens and each missing file, the controller, the audio, the display, the frame rate and memory reports, and the output of the game. |
| `/data/halo/logs/debug.txt` | The game's own log (as `debug.txt` on the other platforms). |
| `/data/halo/logs/crash.log` | After a crash or a fatal error: the last 64 KB of the log at all levels. |

Get the log files with FTP (GoldHEN, port 2121), for example:

```sh
curl ftp://<PS4_IP>:2121/data/halo/logs/host.log
```

### klog over the network

GoldHEN's klog server sends the kernel log to TCP port 3232. Enable it in
GoldHEN's settings. Then, on the computer, before you start the game:

```sh
nc <PS4_IP> 3232 | tee klog.txt
```

The launcher's lines start with the time in seconds and the level:

```
[    0.000] info: Halo for PS4 starting (build ...)
[    0.000] info: content root 1: /data/halo (write root)
[    0.340] info: file maps/ui.map: root 1 (/data/halo/maps/ui.map)
[    0.341] warn: file not found: /halo/maps/a10.map (tried /data/halo/maps/a10.map, /mnt/usb0/halo/maps/a10.map, /app0/maps/a10.map)
```

### The log level

Set the level in `/data/halo/config/ps4.toml`:

```toml
[log]
level = "debug"   # debug, info (default), warn, error
```

The level controls klog and `host.log`. `crash.log` always has all
levels.

## Crashes

On a crash, the launcher logs the signal, the fault address, the
registers and the stack frames of the game, then writes `crash.log`. If
the crash is in the game code, the log gives the command that finds the
function:

```
fatal: signal 11, fault address 0x..., pc 00000000880a1234 sp ...
fatal:   in the guest: llvm-addr2line -f -e build/ps4/halo_guest.elf 0x880a1234
fatal:   frame  0: return 880b5678
```

Use the `halo_guest.elf` of the same build (`build/ps4/halo_guest.elf`).
The return addresses of the frames resolve the same way.

The memory report in the log (`memory (...)`: committed, peak and pool)
helps to find leaks in long tests.

## Common problems

| Symptom | Cause and remedy |
| --- | --- |
| Notification "Halo needs Piglet's shader compiler" | The shader compiler modules are missing. Refer to [BUILDING-PS4.md](BUILDING-PS4.md#shader-compiler-modules). `host.log` gives the folders that the launcher examined. |
| "file not found: /halo/maps/ui.map" | The game data is not in `/data/halo/maps` (or `/mnt/usb0/halo/maps`). The log line gives the full paths. |
| The application does not start, or it closes immediately | Make sure that GoldHEN is active (run it again after each restart of the console). Examine klog. |
| `eglGetDisplay` fails ("out of memory") | Piglet refused the application. The eboot must have the PAID and auth info of `EBOOT_AUTHINFO` in `tools/ps4_build.py`. Do not change them. |
| No sound | `host.log` records the result of `sceAudioOutOpen`. |
| The controller does not operate | `host.log` records "controller opened" and "controller connected" (or the `scePadOpen` error). The launcher uses the user who started the application. |

## The Linux test host

`ninja ps4_host_linux` builds `build/ps4/halo_ps4_headless`. It is the PS4
launcher for x86-64 Linux, without a display, sound or controller. It runs
the same game image as the console and uses the same loader, system call
translation, threads, memory layout and file resolution. Use it to test
changes before you copy them to the console.

```sh
python3 configure.py --ps4
ninja ps4_guest ps4_host_linux
mkdir -p /tmp/halo_root/maps     # copy maps/ here for a full test
HALO_PS4_ROOTS=/tmp/halo_root HALO_PS4_QUIT_AFTER=10 \
    build/ps4/halo_ps4_headless build/ps4/halo_guest.elf
```

| Variable | Function |
| --- | --- |
| `HALO_PS4_ROOTS` | The content folders, separated by `:` (instead of `/data/halo:/mnt/usb0/halo:/app0`). The first one is the folder that the game writes to. |
| `HALO_PS4_QUIT_AFTER` | Ask the game to quit after this number of seconds. |
| `HALO_PS4_GUEST` | The game image, if it is not the first argument. |
| `HALO_PS4_LOG_LEVEL` | The log level (`debug`, `info`, `warn`, `error`). |
| `HALO_PS4_TRACE_SYSCALLS` | `1` logs each system call of the game (at the debug level). |

These variables are for the test host only. The console reads its
settings from `ps4.toml`.

## Other checks

- `tools/ps4_shader_check.sh` translates the game's shaders to GLSL ES and
  validates them with glslangValidator.
- `python3 tools/ps4_package.py --dump-sfo build/ps4/cmake/package/pkg_root/sce_sys/param.sfo`
  shows the values in `param.sfo`.
- `readelf --dyn-syms build/ps4/eboot.elf | grep UND` lists the system
  functions that the eboot imports.
