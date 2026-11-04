# Building from source

Everything builds on Windows 10/11 x64 with MSYS2's CLANG64 toolchain. The same steps run in CI
(`.github/workflows/build.yml`), so a green run there is a working recipe.

## 1. Toolchain

Install [MSYS2](https://www.msys2.org) to `C:\msys64` (`winget install MSYS2.MSYS2`; another place
works with `MSYS2_ROOT` set to it), open the **MSYS2 CLANG64** shell and install the toolchain and
libraries:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross} \
  mingw-w64-clang-x86_64-{libvorbis,libogg}
```

You also need Python 3.10 or newer on Windows itself (python.org) for `run.py`, the tests and the
packaging, and [Git for Windows](https://git-scm.com) for Git Bash.

## 2. Source

```bash
git clone --recurse-submodules https://github.com/YAPhoa/theatrhythm-fbl-pc.git
```

imgui, magic_enum, miniz, xbyak and LibAtrac9 are submodules; `build.sh` fetches them if the clone
was made without `--recurse-submodules`.

## 3. Build

From Git Bash (or the MSYS2 shell) at the repository root:

```bash
bash build.sh
```

It writes `out/theatrhythm-fbl.exe` (loader, runtime and GPU library in one exe) and
`out/theatrhythm-setup.exe`. Both are RelWithDebInfo builds: they carry their debug info (about
120 MB of the game exe) for debuggers and `llvm-addr2line`. A failed build prints its last lines;
the full log is `out/gpu-build.log`.

Tests:

```bash
bash build.sh --test
```

```bash
cd tests && python -m unittest discover -p "test_*.py"
```

Tests that need the game dump or `iced-x86` skip themselves when those are missing.

### Linux (not released yet)

On Linux x86-64 the same commands build `out/theatrhythm-fbl` (linked without PIE: host pointers
handed to the game must stay below 1 TiB) and `out/theatrhythm-setup`, and run the same tests.
On Ubuntu 24.04 (also in WSL), `packaging/linux/ci-deps.sh` installs the dependencies as root,
building Vulkan-Headers, VMA and SDL3, which 24.04 lacks or ships too old. Then:

```bash
CC=clang-18 CXX=clang++-18 bash build.sh --test
```

The DLC maker (`tools/dlcmaker/app/out/theatrhythm-dlcmaker`: its window, Dear ImGui on SDL3, and
its command line) builds with its own script:

```bash
CXX=clang++-18 bash tools/dlcmaker/app/build.sh
```

`python3 tools/package.py --zip` writes `out/<release>-linux.tar.gz` (also `-symbols`, and
`-debug` with `--debug`): the game, the setup, the libraries they need in `lib/` less the host's
(glibc, libstdc++, Vulkan, X11/Wayland, audio) and `prepare/`. The setup runs the system's python3.

The DLC maker is a download of its own, `out/theatrhythm-dlcmaker.AppImage`:

```bash
bash tools/dlcmaker/app/build.sh
bash tools/dlcmaker/build-ffmpeg.sh out/dlcmaker-ffmpeg
python3 tools/package.py --dlcmaker --ffmpeg out/dlcmaker-ffmpeg/ffmpeg --zip \
    --appimagetool <appimagetool-x86_64.AppImage> --appimage-runtime <runtime-x86_64>
```

It holds the DLC maker's program (the window and the command line; libvorbis and libogg linked in),
a minimal ffmpeg built from a pinned FFmpeg release (`build-ffmpeg.sh`: only the formats, filters and
encoders the DLC maker uses, FFmpeg's own libraries linked statically) and, in `lib/`, SDL3, libx264
and libdav1d (also in `dist-dlcmaker/`, which works as it is). appimagetool 1.9.1 and the type2 runtime
are pinned with their SHA-256 in `.github/workflows/build.yml`.

`packaging/linux/smoke-test.sh <unpacked release> <theatrhythm-dlcmaker.AppImage>` checks both as root on a
fresh distro (libraries, the setup on non-dumps, the start from `paths.ini`, the DLC maker's Check
on a new project); the `linux-smoke` CI job runs it on Ubuntu 24.04, Debian 13, Fedora 42 and Arch.
In WSL the game runs on Mesa's software Vulkan (llvmpipe): slow, but enough for a boot check.

## 4. Run from the source tree

Point `run.py` at your decrypted dump (PS4, version 1.00: `eboot.bin`, `sce_module`, `sce_sys`,
`Media`) with `--game DIR` or `THEATRHYTHM_GAME_DIR`:

```bash
python run.py --game <dump> --timeout 20
```

`run.py` prepares `out/game/` from the dump when needed and starts the game with the MSYS2 DLLs on
`PATH`. [development.md](development.md) covers its other commands.

## 5. Package

There are two kinds of package. Both hold what a player needs without this repo or MSYS2: the game
and setup exes, the DLLs they load (found by walking their imports in `C:\msys64\clang64\bin`),
`prepare/` for the setup, a README and the license. Neither ever contains game files.

| | Release (for players) | Debug (for chasing a bug on someone's PC) |
|---|---|---|
| Command | `python tools/package.py --python <embed.zip> --zip` | `python tools/package.py --python <embed.zip> --zip --debug --out dist-debug` |
| Exes | debug info stripped (game exe ~7 MB) | as built, with debug info (~126 MB) |
| `diagnose/` | not included | glitch kit from `packaging/windows/diagnose` |
| Zip | `out/<release>.zip` | `out/<release>-debug.zip` |

`<embed.zip>` is the python.org "Windows embeddable package (64-bit)", e.g.
`https://www.python.org/ftp/python/3.15.1/python-3.15.1-embed-amd64.zip`; check its SHA-256
against the python.org release page. It becomes `prepare/python`, so players need no Python of
their own. Without `--python` the setup uses the player's Python 3.10+.

The DLC maker is a separate download. Build its window, then package it:

```bash
bash tools/dlcmaker/app/build.sh
bash tools/dlcmaker/build-ffmpeg.sh out/dlcmaker-ffmpeg
python tools/package.py --dlcmaker --ffmpeg out/dlcmaker-ffmpeg/ffmpeg.exe --zip
```

`build-ffmpeg.sh` builds a minimal static `ffmpeg.exe` from a pinned FFmpeg release (only what the
DLC maker uses, with libx264, libdav1d and zlib linked in: `pacman -S make
mingw-w64-clang-x86_64-{nasm,x264,dav1d,zlib}`). Run it from an MSYS2 CLANG64 shell; it takes a few
minutes, and CI caches it. Without `--ffmpeg`, `package.py` ships MSYS2's `ffmpeg.exe` with its
~110 DLLs instead.

That fills `dist-dlcmaker/` (which works as it is) and writes `out/theatrhythm-dlcmaker.exe`: the
DLC maker's program (the window and the command line, linked statically) with `ffmpeg.exe` and the
text files appended as a zip, which it unpacks into `%LOCALAPPDATA%\theatrhythm-dlcmaker\<release>`
on its first start of a release (with Windows' `tar.exe`). Players put the exe in the game's release
folder, where the window takes the game and save folders from `paths.ini`, or anywhere else.

The folder (`dist/` by default, `--out DIR` otherwise) is also a working install: run its
`theatrhythm-setup.exe` once and it plays. Re-packaging into it keeps what the setup and the
diagnose kit wrote there (`game/`, `paths.ini`, `settings.ini`, captures, logs).

### Symbols for release crashes

A release `--zip` also writes `out/<release>-symbols.zip` with the unstripped exes of that build.
It is not for players. A crash in a player's `theatrhythm.log` reads like
`theatrhythm-fbl.exe+0x1234ab`; resolve it with the symbols of the same release (the name is in the
player's `release.txt`), adding the image base 0x140000000:

```bash
llvm-addr2line -fCe theatrhythm-fbl.exe 0x1401234ab
```

### The debug kit

`diagnose/start.bat` starts the game with the frame capture, the renderer optimization toggles and
the cache reload armed (they stay idle until used) and logs to `diagnose/theatrhythm.log`. While a
glitch is on screen the player runs `1-capture.bat`, `2-optimizations-off.bat` and `3-reload.bat`,
then sends the `diagnose` folder. The debug README.txt tells them so.

## 6. CI and releases

Every push and pull request runs `.github/workflows/build.yml` on `windows-latest`: build, both test
suites, then both packages (the game's with the pinned embeddable Python). The `linux` and `linux-smoke` jobs do
the same for Linux on `ubuntu-24.04`; their tarballs are artifacts and are not published yet. The release, debug and symbols zips
are the run's artifacts (Actions tab, kept 90 days). Pushing an annotated tag `v*` also publishes
the release zip as a GitHub release, named after the tag:

```bash
git tag -a v0.1.0 -m "v0.1.0"
```

```bash
git push origin v0.1.0
```
