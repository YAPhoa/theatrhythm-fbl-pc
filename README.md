<p align="center">
  <img src="packaging/windows/theatrhythm-fbl.ico" alt="" width="128">
</p>

<h1 align="center">Theatrhythm Final Bar Line on PC</h1>

<p align="center">
  An unofficial native PC runner for the PS4 version of <b>Theatrhythm Final Bar Line</b>.<br>
  <a href="https://github.com/YAPhoa/theatrhythm-fbl-pc/releases/latest"><b>Download the latest release</b></a> ·
  <a href="https://ko-fi.com/yohialfredo">Support me on Ko-fi</a>
</p>

It runs the game's own code natively on Windows and Linux, with a Vulkan renderer, keyboard and
gamepad controls, and display settings of its own. **No game files are included:** you need a decrypted
dump of your own copy of the game (PS4, version 1.00).

## Features

- **PC display settings:** windowed, borderless or fullscreen, at your desktop's resolution.
- **Keyboard and controller:** Xbox-style mapping for controllers, with the game's button hints
  and its Live Info controller redrawn for keyboard and Xbox. DualSense works too.
- **Easy setup:** point the installer at your dump, and it can add a desktop shortcut (on Linux,
  an application menu entry).
- **Custom songs:** [dlcmaker](tools/dlcmaker/README.md) turns your own music, charts and art into
  add-on packs that show up in Music Stages next to the game's DLC, on battle, event or field stages.
  It's a separate download on the releases page: `theatrhythm-dlcmaker.exe` (Windows) or
  `theatrhythm-dlcmaker.AppImage` (Linux).

## Supported languages

The game asks which language to use on first boot and offers six. English is the one played most
with this runner so far.

- ✅ English
- ✅ Japanese
- ✅ French
- ✅ German
- ✅ Italian
- ✅ Spanish
- ✅ Korean, Chinese (Traditional) and Chinese (Simplified), available via the in-game options

The game's data has Korean and Chinese text, graphics and fonts too, but the release this runner
targets (CUSA32853, built for the West) leaves them out of its language picker. The runner adds
them in the game: in **Options > Language & Menu Volume**, press left or right on **Language** to
step through all nine. The choice is saved with your game. Pressing the row still opens the game's
own picker of six. These three have been checked less than the others: the menus and the tutorial
display fully.

## Download

From the [latest release](https://github.com/YAPhoa/theatrhythm-fbl-pc/releases/latest):

- **Windows:** `theatrhythm-fbl-pc-<version>.zip`
- **Linux:** `theatrhythm-fbl-pc-<version>-linux.tar.gz`

## Requirements

- **Windows:** Windows 10 or 11, 64-bit. Python and other tools come with the release: you don't
  need to install anything else.
- **Linux:** x86-64 with glibc 2.39 or newer (SteamOS 3.7, Ubuntu 24.04, Debian 13, Fedora 40 or
  newer, Arch) and Python 3.10 or newer (`python3`, which most distributions have) for the setup.
- A CPU with AVX
- A GPU with Vulkan 1.3 and an up-to-date driver (on Linux, Mesa or the NVIDIA driver)
- A decrypted dump of your own copy of the game: the folder with `eboot.bin`, `sce_module`,
  `sce_sys` and `Media`

### Tested on

| Part | Model |
|---|---|
| CPU | AMD Ryzen 7 9800X3D |
| GPU | NVIDIA GeForce RTX 5080 |
| RAM | 64 GB |
| OS | Windows 11 Pro |

That's far more than the game needs. It was made for a PS4, and the runner adds little on top, so
a much more modest PC should play it fine: any reasonably recent CPU with AVX and a GPU with a
current Vulkan 1.3 driver. Lower-end machines haven't been tested yet, so if yours works (or
doesn't), please say so in an [issue](https://github.com/YAPhoa/theatrhythm-fbl-pc/issues).

The Linux version is new: it has been run on Ubuntu 24.04 in WSL (software rendering, from setup
to the game's first screen) and checked on Ubuntu, Debian, Fedora and Arch, but not yet played on a
Linux PC with its own GPU or on a Steam Deck. If you try it there, please report how it goes.

## Install and play

1. Unzip the release anywhere, for example `C:\Games\Theatrhythm`.
2. Run `theatrhythm-setup.exe`.
3. Choose your **game folder** (the dump) and a **save folder** (by default `save` inside the
   release folder), then press **Install**. Setup prepares the game from your dump in a few
   seconds and offers a desktop shortcut. Your dump is only read, never changed.
   Tick **Skip the opening movie** to go from the start-up logo straight to the title screen.
4. Press **Play Game**. Later, start `theatrhythm-fbl.exe` or the shortcut.

Run `theatrhythm-setup.exe` again any time to change the game or save folder. It offers to copy your
saves when the save folder moves. To get the opening movie back, untick the box and press
**Install** again, or set `skip_opening=0` in `settings.ini`. Even with the movie on, **Options**
skips it once you have watched it through one time.

**DLC you already keep elsewhere**, for example for shadPS4, doesn't need a second copy: choose that
folder as the **extra DLC folder** in setup (optional; on Linux setup asks, or pass `--dlc <folder>`).
shadPS4's own `addcont` folder works as it is, with its `CUSA32853` folder inside. The game reads
DLC from there as well as from `addcont` in your save folder, and never writes to it. If the same
DLC is in both, the save folder's copy is used. Custom songs from dlcmaker still go into the save
folder's `addcont`.

### On Linux

1. Extract the release anywhere you can write, for example `~/Games/Theatrhythm`:
   `tar -xzf theatrhythm-fbl-pc-<version>-linux.tar.gz -C ~/Games`
2. Run `theatrhythm-setup`: open it from your file manager and it asks with dialogs, or run
   `./theatrhythm-setup` in a terminal and it asks there. It also takes
   `--game <dump> [--save <folder>]` on the command line.
3. Pick your **game folder** (the dump), keep or change the **save folder**, and choose whether to
   skip the opening movie. Setup prepares the game in a few seconds and offers an entry in your
   application menu.
4. Start the game from the menu entry, or run `./theatrhythm-fbl`.

On a Steam Deck, do this in Desktop Mode; then *Add a Non-Steam Game* in Steam lists the menu entry.
Shader caches are kept in `~/.cache/theatrhythm-fbl`.

### Updating

Unzip (on Linux, extract) the new release over the old folder, then run the setup once more (the
game asks you to). Your folders, saves and settings are kept.

Setup also remembers your folders outside the release, in `%APPDATA%\Theatrhythm Final Bar Line\paths.ini`
(Linux: `~/.config/theatrhythm-fbl/paths.ini`). A release unzipped into a new folder starts from
them, so setup only needs **Install**. To use another save folder without running setup, set the
environment variable `THEATRHYTHM_SAVE_DIR` to it; it wins over the one setup chose.

## Adding DLC

The game's own DLC works from a decrypted dump of DLC you own. Each add-on is one folder in
`addcont` inside your save folder:

```
<save folder>/
  addcont/
    <label>/
      dlcparam.json
      catalog.json
      ...
```

1. Close the game.
2. Make a folder `addcont` in your save folder (the one you chose in setup; by default `save` inside
   the release folder).
3. Copy each DLC into it as a folder of its own, with `dlcparam.json` directly inside. Name the
   folder after the DLC's label, the part of its content ID after the last `-`. Any name of up to
   16 characters works; longer ones are skipped.
4. Start the game: it finds the DLC at boot.

To remove a DLC, delete its folder. Custom song packs from [dlcmaker](tools/dlcmaker/README.md) go in
the same place (its **Install** puts them there). Setup copies only your saves when the save folder
moves, so move `addcont` yourself.

## Settings

Your settings are in `settings.ini` in your save folder, so they move with your saves. It holds the
window mode and size, monitor, vsync, tearing, frame limit, frames queued, anisotropic filtering,
render resolution, shader compilation, audio volume and speaker layout, vibration, the dialog
confirm button, control remapping and button icons, each with a comment explaining it. Setup writes
a default `settings.ini` next to the game, and the game copies it into a save folder that has none
when it starts. From then on the game reads and writes only the save folder's file: edit that one.

In the game, **Options > Display Settings** (an extra button below Streaming Rules) changes the
window mode, resolution, vsync, frame rate limit and render resolution. Changes apply at once and
are saved to `settings.ini` in your save folder when you leave the page. **F10** opens the same
settings in a panel with a few more (monitor, frames queued, anisotropic filtering, and the
advanced choices below).

**Render resolution** is 4K by default (`render_resolution=4k` in `settings.ini`): the game renders at
3840x2160, as on a PS4 Pro, and the picture is filtered down to your screen (the desktop resolution,
as the game opens borderless by default), so it's sharper on any screen. On a weaker GPU set it to
1080p, as on a PS4. It applies the next time you start the game.

### Advanced settings

The defaults are the steady ones: VSync on, one frame per refresh, no tearing. These change that
trade-off; set them in `settings.ini` or the F10 panel.

| Setting | Values | What it does |
| --- | --- | --- |
| `vsync` | `1` (default), `0` | Off shows the newest finished frame at each refresh: less input delay, still no tearing. Some drivers (NVIDIA) hold it to twice the refresh rate. |
| `tearing` | `0` (default), `1` | With `vsync=0`, shows each frame the moment it is done (Vulkan Immediate). The least input delay and no frame rate ceiling, but the picture can tear. F10 shows it as VSync "Off, tearing allowed". |
| `fps_limit` | `0` (default), `30` to `240`, `unlimited` | `0` follows the display: one frame per refresh (120 a second on a 120 Hz monitor). A number caps the frame rate (with `vsync=0`). `unlimited` removes the cap: hundreds of frames a second with `tearing=1`, so the GPU runs flat out. |
| `frames_queued` | `1` (default), `2`, `0` | Frames the CPU may prepare ahead of the GPU: 1 is the lowest latency, more can smooth out a slow GPU (`0` unbounded). |
| `anisotropy` | `0` (default), `2` to `16` | Sharper textures seen at an angle. Applies at the next start. |

**Uncapped** is `vsync=0`, `tearing=1` and `fps_limit=unlimited`. A middle ground is the same with
`fps_limit=240`: most of the latency gain, with the GPU far from flat out. On a monitor at 60 Hz you
still see 60 pictures a second; what you gain is input delay.

## Controls

The game sees one DualShock 4. Any gamepad works, mapped by button position: on an Xbox pad A B X Y
are Cross, Circle, Square, Triangle; LB/RB are L1/R1, LT/RT are L2/R2, Menu is Options and View
clicks the touchpad. The keyboard and mouse work alongside a gamepad. In rhythm play any button
taps a trigger and either stick slides, so each hand gets a stick and taps:

| Keys | PS4 |
|---|---|
| W A S D / I J K L | left stick / right stick |
| arrow keys | d-pad (menus) |
| Space, Enter | Cross |
| Escape, Backspace, `;` | Circle |
| F / H | Square / Triangle |
| Q, Page Up / O, Page Down | L1 / R1 |
| E / U | L2 / R2 |
| X / M | L3 / R3 |
| Tab, P | Options |
| T | touchpad click |

**F4** asks "Exit the game?" in the game's own Yes/No window, with its sounds (No is selected
first; Circle, Esc and Start answer No too). During a song it pauses the song first, and No carries on from the same moment after a
3-2-1 countdown. Yes closes the game, after any save in progress is written. It doesn't open over
another of the game's windows or during a scene change: a note says so. Alt+F4 still closes the
game at once.
**F10** opens the Display Settings panel.

Keys are physical positions, so they're the same on AZERTY or QWERTZ keyboards. To change them,
edit `key_map` and `pad_map` in `settings.ini`, e.g. `key_map=cross=G,cross=Mouse Left`: the first
mention of a button replaces its defaults, the next ones add more, and `=None` unbinds it.

On-screen button prompts follow the device you last used: PlayStation icons for a DualShock or
DualSense, Xbox icons for other gamepads, and key names for the keyboard. The controller on the
Live Info panel and in the tutorial follows too: an Xbox pad for other gamepads, the DualShock 4
otherwise. Set `button_icons` in `settings.ini` to `ps4`, `xbox` or `keyboard` to keep one set.

## Issues and suggestions

Found a problem? [Open an issue](https://github.com/YAPhoa/theatrhythm-fbl-pc/issues) and attach
`theatrhythm.log` from the `log` folder in your save folder. Any suggestion, just open an issue too.
If the picture freezes, leave the game running for half a minute before you close it: after 20
seconds without a new frame it writes what every thread is doing to that log.

## Building from source

See [docs/building.md](docs/building.md) for the toolchain, building, tests and packaging, and
[docs/development.md](docs/development.md) for how the runner works and how to work on it. Every
push is built and tested by GitHub Actions, and tagging a version publishes a release.

## Credits and license

GPL-2.0-or-later, see [LICENSE](LICENSE). The loader, runtime and GPU shim are derived from
SpartanPort by Felipe Prado and contributors (GPL-2.0). The video core and parts of the runtime
follow [shadPS4](https://github.com/shadps4-emu/shadPS4). Theatrhythm Final Bar Line is a trademark
of Square Enix. This project is not affiliated with or endorsed by Square Enix or Sony Interactive
Entertainment.
