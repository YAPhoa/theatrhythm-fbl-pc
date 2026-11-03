# Development

How the runner works and how to work on it. Building, tests and packaging are in
[building.md](building.md).

## How it works

The PS4 and the PC are both x86-64, so nothing is recompiled. The game's own machine code is
mapped into memory and run directly. What differs is everything that code calls:

| The game calls | Replaced by |
|---|---|
| PS4 system libraries (kernel, POSIX, threads, files, pad, audio, save data, ...) | C reimplementations in `src/runtime_*.c` |
| GNM GPU command buffers | shadPS4's video core in `gpu/`, GCN shaders translated to Vulkan |

The pipeline (`run.py`):

1. `scripts/prepare.py` strips the SELF wrapper from `eboot.bin` (the dump must be decrypted).
2. `scripts/link_modules.py` links the modules the eboot needs at boot into one image:
   `libc.prx`, `libSceFios2.prx` (from `sce_module`), then `PS4Util.prx` and
   `Il2CppUserAssemblies.prx` (from `Media/Modules`; the latter is the game's IL2CPP code).
   Symbols match by NID, never by name.
3. `theatrhythm-fbl` maps the image, resolves every import against the C runtime (by symbol
   name, from `src/import_names.inc`) or the GPU core (by NID), runs each module's initializer
   and enters the game. An import with no implementation becomes a trap that prints
   `STOP: first unsupported PS4 import: <name>` and exits.

The renderer draws at the game's own resolution (2160p in PS4 Pro mode with `render_resolution=4k`,
the default: `sceKernelIsNeoMode` returns 1, the video out is 3840x2160 and probe.c serves
`boot.config` with `platform-ps4-videoout-width=3840`; or 1080p) and scales the picture to the window
(borderless by default, at the desktop resolution). A larger frame is filtered down over each output
pixel's footprint in `post_process.frag`: no upscaler, TAA, motion vectors or post-process hooks. The only overlay is the PS4's system UI (save
list, text entry, trophy notices) and the shader-cache notes (`THEATRHYTHM_SHADER_NOTES=1`).

## Running from the source tree

Set `THEATRHYTHM_GAME_DIR` to your decrypted dump (or pass `--game DIR` each time); `run.py` stops
with a message when it has neither:

```bash
python run.py check
```

```bash
python run.py --timeout 20
```

- `check` lists the imports the runtime cannot resolve yet, grouped by library (full list in
  `out/game/missing-imports.txt`).
- Without a command, `run.py` starts the game. The first STOP or fault it prints is the next thing
  to implement or fix. `--timeout` ends a run after that many seconds.
- `run.py prepare` rebuilds the game image (`out/game/`, or `--out DIR`). Otherwise it is rebuilt
  only when the dump or `scripts/` change.
- Saves go to `user/` (`--user DIR`). Shader caches go to one folder per PC,
  `%LOCALAPPDATA%\Theatrhythm Final Bar Line`, which installed copies of the game share
  (`THEATRHYTHM_GPU_USER_DIR` moves it).
- Every `settings.ini` key has an environment variable that overrides it (named in
  `settings.example.ini`). The file is `THEATRHYTHM_CONFIG` (the launcher: `settings.ini` in the
  save folder; `run.py`: `user/settings.ini`), read from `THEATRHYTHM_CONFIG_DEFAULT` (the one next to
  the exe, or this checkout's) until it exists; the display pages always write `THEATRHYTHM_CONFIG`.
- `THEATRHYTHM_VERBOSE=1` logs every routine event (threads, semaphores, memory maps, module
  loads and lookups). `run.py` and the debug package's `diagnose/start.bat` turn it on; an installed
  release leaves it off, so its log holds faults, stops, errors and the end-of-run summary.
- Hang dump: once a frame has been shown, a picture held for `THEATRHYTHM_HANG_DUMP` seconds
  (default 20, `0` turns it off) logs every thread's name, CPU time and frames (`Hang:` lines, guest
  offsets and host module offsets, innermost first), once until frames move again. The `--timeout`
  watchdog prints the same thread list before it ends a run.

## Development loop

1. `python run.py --timeout 20` and read the STOP line (name, caller offset, module).
2. Implement that function in the matching `src/runtime_*.c` export table (`{"name",function}`),
   following the PS4 contract. Return "not available" for services a PC does not have.
3. `bash build.sh`, run again.

When an import shows up as a bare NID, add candidate names to `tools/ps4_names.txt` and run
`python tools/name_imports.py out/game --write`. It hashes every candidate (and every name the
runtime already implements) and appends the matches to `src/import_names.inc`.

## Release folder and setup

`theatrhythm-setup.exe` (`setup/`) runs `prepare/run.py prepare` into `game/` beside it, rewrites the
game's instructions for this CPU if needed, writes `settings.ini` from the template when there is
none, and writes `paths.ini` with the game and save folders. No release contains a save folder, so
unpacking a newer release over the folder keeps it. After that, the game asks for the setup once
more, because `game/` was prepared for another `release.txt`. The command line does the same as the
window: `theatrhythm-setup.exe --help`.

Started with no arguments, `theatrhythm-fbl.exe` reads `paths.ini` and builds the command line
that `run.py` would pass (`src/launch.c`). Without a console its output goes to
`theatrhythm.log` in the save folder's `log/`. `--console` opens a console window instead.

Preparing the game needs Python 3.10 or newer, using only its standard library; releases bundle
python.org's embeddable Python in `prepare/python`. The CPU rewrite also needs `iced-x86`, which the
bundled Python lacks, but it only runs on CPUs that are missing an instruction set the game uses.
The icon of both exes is `packaging/windows/theatrhythm-fbl.ico` (`tools/make_app_icon.py` builds
it from the app artwork).

## Controls and prompts, inside

Pad and keyboard mapping live in `src/runtime_pad.c`. `pad_map` / `key_map` (or
`THEATRHYTHM_PAD_MAP` / `THEATRHYTHM_KEY_MAP`) take SDL's key names, `Mouse Left/Right/Middle/X1/X2`
and SDL's gamepad button names (`a`, `b`, `x`, `y`, `back`, `leftshoulder`, ...); stick directions
are `lstick_up` ... `rstick_right` (keyboard only). `pad_map=touchpad=back,touchpad=misc1` adds Share
to the touchpad. `theatrhythm-fbl --gamepad` prints the connected pad and its face-button labels.

The game draws every prompt from one icon atlas: `run.py prepare` builds copies of its bundle with
the sheets from `assets/button_icons` (`scripts/button_icons.py`), the runtime serves the current one
in place of the game's, and on a device change it rewrites the atlas already loaded in memory
(`src/runtime_button_icons.c`). `button_icons` (or `THEATRHYTHM_BUTTON_ICONS`) is `auto` by default.

Display Settings is an eighth Options button (`src/runtime_display_page.c`) on the game's own settings page. Its icon
sits in an unused corner of the Options atlas, one of the game's encrypted bundles: `run.py prepare`
decrypts it with the key from the game's own metadata, adds `assets/config_icon/display.bc7` and
encrypts it again (`scripts/config_icon.py`), and the runtime serves it in place of the game's file.

The same file lets left and right on Options > Language & Menu Volume > Language step through all
nine display languages: the game has Korean and Chinese (Traditional, Simplified) text, graphics and
fonts, but this release's region checks (`RegionLanguage`, also inlined in `ConfigInfo`'s setter, its
save reader and the picker) allow only the Western six. The hook switches the language as the
picker does (`LanguageController.SwitchLanguage` with `FontResourceManager.Instance`, then
`Backup.ReserveSave`), byte patches keep the choice through the setter and a reload, and a stub
gives the picker of six a cursor on English when the language is one of the other three.

On field stages the party walks between enemies: `StageCharacter.MyLateUpdate` picks run, walk or
idle from how far the character moved since the last frame, with thresholds made for 60 fps, so
above that (an uncapped `fps_limit`) the party slid along in its idle pose. `src/runtime_field_walk.c`
sends that comparison through a stub that divides the distance by (60 x `Time.deltaTime`)^2, so
any frame rate reads like 60; at 60 fps the result is unchanged.

Scripted input for automated runs: `THEATRHYTHM_PAD_FILE` (held buttons and stick values, re-read
when the file changes), `THEATRHYTHM_PAD_RECORD` (F9 records the pad) and `THEATRHYTHM_PAD_REPLAY`
(see `src/runtime_pad.c`).

## Layout

| Path | What it is |
|---|---|
| `src/` | Loader (`probe.c`), launcher (`launch.c`) and the HLE runtime (`runtime_*.c`) |
| `gpu/` | shadPS4 video core, the shim and overlay around it, CMake build |
| `scripts/` | Image preparation and linking (Python) |
| `setup/` | `theatrhythm-setup.exe`, which sets up a release folder from a player's dump |
| `tools/dlcmaker/` | Custom song packs: the DLC maker (`core/`; its window and command line in `app/`); see its README |
| `packaging/windows/` | Manifest, resources and icon of the game exe; `diagnose/`, the debug package's glitch kit |
| `assets/button_icons/` | Xbox and keyboard icon sheets (PNG, and the game's texture format from `tools/encode_button_icons.py`) |
| `assets/config_icon/` | Display Settings icon for the Options menu (PNG patch, and its atlas blocks from `tools/encode_config_icon.py`) |
| `tools/` | `package.py` (the release), `name_imports.py` (names NIDs), `got_import.py` (names a GOT slot), Frida and IL2CPP tracing helpers |
| `tests/` | Python tests (`cd tests; python -m unittest`) and C/C++ test targets (`gpu/CMakeLists.txt`) |
| `docs/` | This guide and [building.md](building.md) |
