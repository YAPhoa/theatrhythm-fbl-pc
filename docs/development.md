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

Guest code runs on host thread stacks, so the bottom of every guest stack is a host frame. IL2CPP
walks the whole stack with libc's unwinder for each managed exception's trace, and that unwinder
follows rbp wherever it finds no frame data. So the game's `_start` and every guest thread are
entered with rbp 0 (`runtime_enter_guest`, `enter_on_stack`), and `sceKernelGetModuleInfoForUnwind`
reports host code as a module without frame data. The walk then ends at the host boundary instead
of reading host locals as frames (issue #17: the start-up picture's path in the runner's `main`).

Guest code follows the SysV ABI, whose leaf functions keep locals in the 128 bytes below rsp (the
red zone). Windows has no red zone: an exception's frame (CONTEXT plus the thread's live XSAVE
state) is written directly below the faulting rsp. The GPU's page tracker write-protects guest
memory, so when a guest leaf function's store or load takes a tracking fault, Windows overwrites
that function's locals. The game then reads a zeroed pointer a few instructions later (issues #1
and #12, FMOD at 0x17433fc; another at 0xdb797e). How much is overwritten depends on the CPU: on
AVX2-only CPUs whenever the upper ymm halves are in use, on AVX-512 CPUs only when zmm16-31 are.
At load the runner therefore runs shadPS4's static red-zone pass
(`gpu/shadps4/core/redzone_patches.cpp`, called from `redzone_protect` in probe.c). It finds
functions from each module's `.eh_frame_hdr`, works out where red-zone data is live, and moves each
memory access made then to a trampoline after the image that runs it with rsp 128 bytes lower.
Code reached only through a `switch` jump table is found by reading the table: a `jmp reg` after
`movsxd reg, [table + index*4]` and `add reg, table`, with the index bounded by `cmp index, imm` and
`ja`/`jae`, or by `and r32, imm` and shifts right. On top of upstream's adjacent pattern the pass
skips unrelated instructions between the load and the add (eboot 0xd34790), follows copies of the
index (`mov`, `movzx`, a spill through a stack slot) and flag-keeping instructions between the
`cmp` and its branch (Il2Cpp, FMOD). The table register is its one reaching definition over the
function's edges, a `lea reg, [rip+d]` (eboot 0x16a9ff0 sets several in earlier blocks); upstream's
nearest `lea` by address must be that one too. With all edges known each table must come out the
same again (its register, no way into the path from the bounds check to the jump, as later tables
can add), or the function is decoded again with that jump left unresolved; a function with an
unresolved jump is decoded without reaching definitions, where only a path the edges cannot settle
falls back to upstream's choice. The table's targets are
the jump's successors in the liveness analysis (upstream gives it none, so data read again in a
case of a loop through the table counted as dead: 0xd34790's `[rsp-0x10]`, whose loss crashed at
0xd34962). A pointer jump (`jmp [reg]`, or `jmp reg` right after `mov reg, [reg+d]`, IL2CPP's
method thunks) is a tail call, not a table, unless a known way in lands between the pointer load and
the jump. A function whose table the pass still cannot read
would keep those blocks unprotected; the log line counts these functions (`with an unresolved
jump table`, 0 in 1.00 and 1.04). Functions the loader's own hooks or the ISA rewrite changed are left alone, and so are calls (a
return address in a trampoline would have no unwind data). The log line
`Red zone: N of M memory accesses ... moved` gives the counts; `THEATRHYTHM_REDZONE=2` lists the
accesses it could not move and the jumps it could not follow, `=3` also every access it moved,
each table with its targets, the tail calls and the patched spans (to check against another
disassembler: no jump may land inside a span), and
`=0` turns the pass off. `THEATRHYTHM_REDZONE_PROBE=<offset>[,...]` (tests, up to 4) puts a hardware
execute breakpoint on guest instructions, on the trampoline copy of a moved one, and prints the
hits: its debug exception writes the exception frame there as a tracking fault would. With
`THEATRHYTHM_DIRTY_ZMM=1` each hit puts zmm31 back in use, so every exception writes the full frame;
it ends at rsp rounded down to 64 (rsp-0x10 is reached when rsp % 64 is 8). On a thread where
zmm31 was not in use yet, that first write into the frame is the probe's own, so list the
function's entry first: its hit puts zmm31 in use before any probed access runs. Never probe a
stack-relative access: those are not moved, so the probe clobbers their red zone in any build. Linux needs none of this: the kernel skips
the red zone when it delivers a signal. To test on an AVX-512 PC the way AVX2-only CPUs behave, set
`THEATRHYTHM_DIRTY_ZMM=1` (zmm31 stays in use on guest threads). `THEATRHYTHM_FAULT_SITES=1` prints
each guest instruction that takes a tracking fault. `tests/test_redzone.cpp` (`redzone-test`, run by
`build.sh --test` on Windows) patches small leaf functions that store into a write-protected page,
one per table shape above and a tail call, and checks that their red-zone locals survive the
fault; an unpatched copy shows the overwrite.

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
  watchdog prints the same thread list before it ends a run. Each thread's stack is copied while
  it is suspended and walked on the copy (a running thread's live stack changes under the walk and
  once made the watchdog fault in ntdll); a read that faults anyway ends that thread's walk.

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
none, sets its `skip_opening` line from the window's checkbox (in the save folder's `settings.ini`
too, when there is one, since the game reads that one first), and writes `paths.ini` with the game
and save folders and, when one is chosen, `dlc=`: an extra folder of add-ons. The launcher passes it
as `--dlc` (`THEATRHYTHM_DLC_DIR` overrides it), and `src/runtime_content.c` lists the folders in
`<save>/addcont/` and then those in the extra folder (its `<TITLE_ID>/` subfolder when it has one, as
shadPS4's `addcont` does) whose label the save folder lacks. The game folder is never an add-on: when
it is the extra folder (DLC kept inside the dump), its own `Media`, `sce_module` and `sce_sys` are
skipped, since mounting `Media` as an add-on would overlay the game's whole StreamingAssets and fill
the mount table. No release contains a save folder, so
unpacking a newer release over the folder keeps it. After that, the game asks for the setup once
more, because `game/` was prepared for another `release.txt`. The command line does the same as the
window: `theatrhythm-setup.exe --help`.

Started with no arguments, `theatrhythm-fbl.exe` reads `paths.ini` and builds the command line
that `run.py` would pass (`src/launch.c`). Without a console its output goes to
`theatrhythm.log` in the save folder's `log/`. `--console` opens a console window instead.

Such a start also watches the game for a crash. The launcher starts the same command line again as
a child and waits for it: on Windows in a job that also takes in a restart's process
(`runtime_restart`) and ends the game if the launcher is killed, on Linux by `fork` (the child gets
`PR_SET_PDEATHSIG`). When the game ends with an exit code other than 0 (closing the window and
F4 end with 0; Ctrl+C on Windows and SIGTERM or SIGINT on Linux are not reported), the launcher adds
`Launcher: the game ended with exit code ...` and what the code means to the log, copies the log to
`log/crash-<date>-<time>.log` (never deleted: players may delete them) and shows a message box
naming that file. This
also catches ends the runtime's fault handler never sees: fast fails (`0xc0000409`), `abort()`,
heap corruption, a driver ending the process. `THEATRHYTHM_CRASH_WATCH=0` turns the watch off; the
child runs with it set to 0.

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

The pad on the Live Info panel and in the tutorial follows the same setting: the Controller atlas's
two base sprites get an Xbox drawing (`assets/controller_pad/xbox.bc7`, put into the game's own atlas
by `scripts/controller_pad.py`; keyboard players keep the DualShock 4), swapped the same way. The
DualShock 4's base sprites have tight meshes that would cut the Xbox drawing, and the red press
highlights sit on its buttons, so `src/runtime_xbox_pad.c` hooks MSGamePad's per-frame calls: while
the atlas shows the Xbox pad it gives the base renderer a FullRect sprite over the same area and
moves the highlight groups onto the Xbox buttons, and puts both back for the DualShock 4.

Display Settings is an eighth Options button (`src/runtime_display_page.c`) on the game's own settings page. Its icon
sits in an unused corner of the Options atlas, one of the game's encrypted bundles: `run.py prepare`
decrypts it with the key from the game's own metadata, adds `assets/config_icon/display.bc7` and
encrypts it again (`scripts/config_icon.py`), and the runtime serves it in place of the game's file.

Its values apply while the game runs: the window thread switches the display mode and resolution
(`gpu/shim/window.cpp`), and the presenter rebuilds the swapchain for a new VSync choice or window
size (`Presenter::UpdateSwapchain`). Every swapchain rebuild holds `Scheduler::submit_mutex`, since
`vkDeviceWaitIdle` and destroying the swapchain must not overlap a GPU thread's `vkQueueSubmit`.
Without that lock, NVIDIA lost the device when VSync changed around a switch into or out of exclusive
fullscreen (issue #10). To reproduce that kind of failure, flip the settings from the window thread
in a test build while the game runs. Set `SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0`: otherwise an
exclusive fullscreen window that loses focus minimizes and the presenter stops presenting.

The same file lets left and right on Options > Language & Menu Volume > Language step through all
nine display languages: the game has Korean and Chinese (Traditional, Simplified) text, graphics and
fonts, but this release's region checks (`RegionLanguage`, also inlined in `ConfigInfo`'s setter, its
save reader and the picker) allow only the Western six. The hook switches the language as the
picker does (`LanguageController.SwitchLanguage` with `FontResourceManager.Instance`, then
`Backup.ReserveSave`), byte patches keep the choice through the setter and a reload, and a stub
gives the picker of six a cursor on English when the language is one of the other three.

These patches, the field walk below, the DLC window's bonus row (`src/runtime_dlc_bonus.c`) and the
logo and opening skip (`src/runtime_skip_opening.c`) work on every game version they know the code of (1.00 and 1.04 so far). The log's first lines name the version (`Game: CUSA32853 version 01.04, ...`: param.sfo's
`APP_VER` and the module's SHA-256 from `link.json`; a dump that mixes two versions' files says so).
`src/runtime_game_code.c` finds each place the patches use by its C# name in the game's own
metadata (`Media/Metadata/global-metadata.dat` and the module's code generation tables, as
`tools/il2cpp_symbols.py` does), then checks the code there against a byte pattern with wildcards
for addresses; code without a name of its own (shared generic code, a spot inside a method) is found
by its pattern, which must match the expected number of times. Where a version's code differs
(1.04's `ConfigSliderBehaviour`), the site lists a pattern per version. When a site is missing, the
patch is skipped as a whole and the log names it (`Display page: <method> not found in game ...`);
without the Options button, a notice on screen says Display Settings is on F10. After a game update,
`python tools/game_code.py check DUMP...` shows every site in each dump (and FAIL for one it
cannot find); `make` writes a pattern for a method.

On field stages the party walks between enemies: `StageCharacter.MyLateUpdate` picks run, walk or
idle from how far the character moved since the last frame, with thresholds made for 60 fps, so
above that (an uncapped `fps_limit`) the party slid along in its idle pose. `src/runtime_field_walk.c`
sends that comparison through a stub that divides the distance by (60 x `Time.deltaTime`)^2, so
any frame rate reads like 60; at 60 fps the result is unchanged.

After the loading screen, the start-up logos (Final Fantasy 35th anniversary, Square Enix,
indieszero) are a timeline in the StartUpLogo scene. Its markers reach
`StartUpLogoMain.OnTimelineEvent`: types 0 and 1 open and close a logo's skip window, in which
`OnUpdate` jumps the director to that logo's skip time when a button is pressed (and sets a
"skipped" flag); type 3 starts the music under the logos (`SoundManager.PlaySE`); type 5 sets the
director's time to the timeline's end, where type 2 stops the sounds with a 1 s fade if a logo was
skipped and picks the next scene: first-time setup when it
is due, else the Opening scene, whose movie can be skipped with Options only once
`UserInfo.LookedOpeningDemo` is set. With `skip_opening=1` in `settings.ini` (the setup's checkbox),
`src/runtime_skip_opening.c` hooks both scenes:
- `StartUpLogoMain.OnUpdate` goes, while the scene is not marked skipped (its first frame, before
  the director has fired a marker), to a stub that marks it and runs type 5's code in
  `OnTimelineEvent`'s frame: the director jumps to the end and the scene ends as it does after a
  button skip, so a new save still gets first-time setup. `OnTimelineEvent` drops type 3 in a
  skipped scene, so the logo music never starts (jumping on the second frame let it play through
  the fade).
- The first `OpeningMain.OnReady` of each start goes to `OpeningMain.endMovie`, the movie's own end:
  it marks the movie seen and goes to the title screen before the movie starts. Later Opening
  scenes in the same session play as before.

F4 asks "Exit the game?" in the game's own Yes/No window (`src/runtime_exit_prompt.c`), so it has
the game's look and sounds. The overlay takes the key (Alt+F4 still closes at once); on the game
thread `ProcessLifecyclePS4::Update` (every frame) calls the Language page's `openResetSettingWindow`
with the lifecycle object in place of the page, which only becomes the target of the window's
reaction callback. The Display page's translation hook swaps its text term
(`Scene/Config/Default/001`), and the callback, `onExecuteResetSettingWindow`, is answered for that
target. The window takes only Left/Right (No on the left, focused first; it does not wrap) and Cross,
so while it is open (`runtime_exit_prompt_open`) the pad turns a press of Circle or Options (Escape,
Start, Tab, P) into Left, then Cross (`exit_prompt_no` in `src/runtime_pad.c`). A scene change can
take the window away unanswered (the title screen's attract movie): once no `WindowMain::Update` has
run for 30 frames, the prompt counts as closed, so F4 can ask again. During a song
(`MusicStageMainLoop::updateState` ran this frame) it first calls
`MusicStageMainLoop.callSystemPause(true)`, the pause the PS4's system menu causes: the song and its
music stand still without the game's pause menu, which would take the keys under the window (a
Cross on No once chose its Retry Song). No ends that pause, and a hook on the synchronous
`MusicStageMainLoop::resume(2)` sets its `countdown` argument, which the end of a system pause passes
as false, so the song goes on after the 3-2-1 of Resume Song (`ResumeCounter`). Yes pushes the same
quit event as closing the window, once no save is mounted, backed up or written
(`runtime_savedata_busy`, at most 10 s). It does not open while another game window updates
(`WindowMain::Update`: a second window of the same scene throws "An item with the same key has
already been added", and an exception through a hook stub ends the game) or when
`WindowManager.IsOpenableSystemWindow()` says no; a notice says why.

With `players=2` (`THEATRHYTHM_PLAYERS`) the runtime has two pad slots: user 2 is logged in by
`src/runtime_services.c` and `scePadOpen` for it returns handle 2, with its own sample, rumble and SDL
gamepad (the first one SDL lists that player 1 does not hold). The keyboard, mouse and the scripted
input below stay with slot 0.

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
| `assets/controller_pad/` | Xbox pad for the Live Info controller (PNG of the two base sprites, and their atlas blocks from `tools/encode_controller_pad.py`) |
| `assets/config_icon/` | Display Settings icon for the Options menu (PNG patch, and its atlas blocks from `tools/encode_config_icon.py`) |
| `tools/` | `package.py` (the release), `name_imports.py` (names NIDs), `got_import.py` (names a GOT slot), `game_code.py` (the patched game code in each dump), Frida and IL2CPP tracing helpers |
| `tests/` | Python tests (`cd tests; python -m unittest`) and C/C++ test targets (`gpu/CMakeLists.txt`) |
| `docs/` | This guide and [building.md](building.md) |
