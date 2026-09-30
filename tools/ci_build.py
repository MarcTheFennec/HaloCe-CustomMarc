#!/usr/bin/env python3
"""Builds one native port in one configuration, as the GitHub workflow does
(.github/workflows/build.yml), and collects what it built into dist/:

    python tools/ci_build.py linux debug
    python tools/ci_build.py android release

--enable-libsm64 builds the libsm64 module in too (port/libsm64_port), with
the library it opens (build/linux/libsm64.so, build/windows/sm64.dll) beside
the executable. It is off at run time all the same, until config.toml asks
for it or the player presses F3; Android has no dynamic loader, and is
refused.

Builds are portable (any x86-64 processor), so they run on other
computers. Debug builds skip link-time and profile-guided optimisation,
which only make the build slower; release builds use both, as a local
release build does (profile-guided optimisation needs clang 22 or later,
and is skipped with an older one). CI_COMPILER_LAUNCHER (ccache, say) is
passed on as --compiler-launcher. A build of the main branch gets the run's
number (HALO_BUILD_NUMBER), which its release is named after and the
self-updater compares.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# what each port's build leaves, and what goes into dist/
OUTPUTS = {
    "linux": ["build/linux/halo"],
    "windows": ["build/windows/halo.exe", "build/windows/SDL3.dll"],
    "android": [],  # the APK, below
}
APKS = {
    "debug": "port/android/app/build/outputs/apk/debug/app-debug.apk",
    "release": "port/android/app/build/outputs/apk/release/app-release.apk",
}


def run(command, cwd=ROOT):
    print("+", " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], cwd=cwd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("platform", choices=sorted(OUTPUTS))
    parser.add_argument("config", choices=["debug", "release"])
    parser.add_argument("--enable-libsm64", action="store_true",
                        help="build the libsm64 module in, and libsm64 with it")
    args = parser.parse_args()

    if args.enable_libsm64 and args.platform == "android":
        print("libsm64: the Android guest has no dynamic loader, so the module is not "
              "built for it (port/libsm64_port/DECISIONS.md)", flush=True)
        return 1

    configure = [sys.executable, "configure.py", "--portable"]
    if args.config == "release":
        configure.append("--release")
    else:
        configure += ["--lto=off", "--pgo=off"]
    launcher = os.environ.get("CI_COMPILER_LAUNCHER")
    if launcher:
        configure += ["--compiler-launcher", launcher]
    if args.enable_libsm64:
        configure.append("--enable-libsm64")
    # a build of main knows its number, which names its release (build-<n>),
    # for the self-updater (port/linux/src/updater.c, and the Android app);
    # other builds have none, and never look for updates
    if os.environ.get("GITHUB_REF") == "refs/heads/main" and os.environ.get("GITHUB_RUN_NUMBER", "").isdigit():
        os.environ["HALO_BUILD_NUMBER"] = os.environ["GITHUB_RUN_NUMBER"]
        print(f"build number {os.environ['HALO_BUILD_NUMBER']}", flush=True)
    run(configure)

    if args.platform == "android":
        # the native part, then the app around it (Gradle's variant of the
        # same name: release is signed with the debug key, not debuggable)
        run(["ninja", "android"])
        gradlew = "gradlew.bat" if os.name == "nt" else "./gradlew"
        run([gradlew, "--console=plain", "-q", f"assemble{args.config.capitalize()}"], cwd=ROOT / "port/android")
        outputs = [APKS[args.config]]
    else:
        run(["ninja", args.platform])
        outputs = OUTPUTS[args.platform]

    dist = ROOT / "dist" / f"halo-{args.platform}-{args.config}"
    if dist.exists():
        shutil.rmtree(dist)
    dist.mkdir(parents=True)
    for output in outputs:
        shutil.copy2(ROOT / output, dist)
        print(f"{output} -> {dist.relative_to(ROOT)}", flush=True)
    if args.platform != "android":
        # the desktop builds' disc image reader (port/linux/src/xiso.c)
        # follows extract-xiso, whose license asks binaries to carry its
        # notice
        shutil.copy2(ROOT / "port/third_party/extract-xiso/LICENSE.TXT", dist / "extract-xiso-LICENSE.txt")
    if args.platform == "linux":
        # the self-updater's TLS (port/third_party/mbedtls), whose Apache
        # license asks the same
        shutil.copy2(ROOT / "port/third_party/mbedtls/LICENSE", dist / "mbedtls-LICENSE.txt")
    if args.enable_libsm64:
        # the library the module opens, and libsm64's licence. Mario's textures
        # and animations come from the player's own ROM, which no build ships.
        library = ROOT / ("build/windows/sm64.dll" if args.platform == "windows"
                          else "build/linux/libsm64.so")
        if library.is_file():
            shutil.copy2(library, dist)
            print(f"{library.relative_to(ROOT)} -> {dist.relative_to(ROOT)}", flush=True)
        else:
            print(f"warning: {library.relative_to(ROOT)} was not built", flush=True)
        shutil.copy2(ROOT / "port/third_party/libsm64/LICENSE.md", dist / "libsm64-LICENSE.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
