# dlcmaker

Custom songs for Theatrhythm Final Bar Line on PC. You give dlcmaker a song (an audio file or a
video), its charts and some art. It turns them into an add-on pack that the runner loads like the
game's own DLC. Your songs show up in Music Stages under a series of their own.

- [Project, DLCs and packs](#project-dlcs-and-packs)
- [What you need to create a DLC](#what-you-need-to-create-a-dlc)
- [Using the app (recommended)](#using-the-app-recommended)
- [Using it from source](#using-it-from-source)
- [The project folder](#the-project-folder)
- [Stages: battle, event, field](#stages-battle-event-field)
- [Charts](#charts)
- [The window](#the-window)
- [Command line](#command-line)
- [Troubleshooting](#troubleshooting)
- [How a build maps onto the game](#how-a-build-maps-onto-the-game)
- [Not done yet](#not-done-yet)

Packs are built from your own decrypted dump: base tables, template bundles and the catalog are read
from it at build time. Your project holds only what you made, so share the project (or one DLC folder from it), never a
built pack; anyone with their own dump can build it.

## Project, DLCs and packs

- Your **project** is your workspace: one folder, anywhere you like, holding all your DLCs. You pick
  it as **Project folder** at the top of the window.
- A **DLC** is a folder in the project holding `dlc.json` and its song folders (your audio, charts,
  art). It holds nothing from the game, so it's what you share.
- A **pack** is what dlcmaker builds from a DLC, for the game to load. **Install** writes each
  DLC's pack to `<save folder>/addcont/<label>/`, where the game finds it. To uninstall a pack,
  delete its folder from `addcont/`. Packs contain parts of the game, so keep them to yourself.
- **Export packs...** builds the same packs into a folder you choose, without installing them. Use
  it to play your songs on another copy of the game, such as a Steam Deck or another PC: copy the
  exported folders into that save folder's `addcont/`. It also works while the game is running,
  which Install doesn't.

## What you need to create a DLC

Before you start: your decrypted game dump and the game's save folder (both set up by
`theatrhythm-setup`).

**The DLC** (one pack, one series in Music Stages):

| What | Needed | Default when you leave it out |
|---|---|---|
| Name | required | the DLC folder's name |
| Series name | optional | the DLC's name |
| Label (the add-on folder) | optional | the name with spaces as `_`, other characters dropped, cut to 16 characters (`CustomDLC` if nothing is left) |
| Series logo (`logo.png`, `logo_wide.png`) | optional | the FINAL FANTASY VII REMAKE logo (base series 13, `series.logo_from`); `logo_wide.png` falls back to `logo.png` |
| Songs | required, at least one | |

**Each song**:

| What | Needed | Default when you leave it out |
|---|---|---|
| Title | optional | the song folder's name |
| Sort name | optional | the title in lower case, keeping only a-z, 0-9 and spaces (set it for Japanese titles: romaji) |
| Stage | optional | `battle` |
| Audio | required | none: mp3, ogg, flac, wav, m4a, ... An event song with a video and no audio file uses the video's sound |
| Video | event only, required | none: mp4, mkv, webm or mov |
| Charts | required | none: Basic, Expert and Ultimate. Supreme is optional; without it, Supreme isn't offered. See [Charts](#charts) |
| Chart levels | optional | 0 (Check warns) |
| Monsters | battle and field | provided by default; for a battle song you can pick your own in the Stage editor |
| Background | optional | battle: the `stage_from` song's, else *One-Winged Angel - Rebirth*'s. Field: the `stage_from` field song's path, camera and sky, else *FFVII REMAKE: Midgar Expressway*'s |
| Jacket | optional | the template song's jacket: *One-Winged Angel - Rebirth* (battle), *Hollow* (event), *Midgar Expressway* (field) |
| Thumbnail | event only, optional | *Hollow*'s |
| Cut | optional | the whole file: `start` 0, `end` at its end |
| Fade out | optional | none (0 s) |
| Preview | optional | 15 s, starting a third of the way in (at most 30 s in) |
| Chart offset | optional | 0 ms |
| Copyright | optional | empty |
| Colour | optional | `#26574C`, the template songs' colour |

The template songs are 481 (battle), 508 (event) and 482 (field); `template` in `song.json` picks
another.

## Using the app (recommended)

The easiest way is the app, a separate download on the
[releases page](https://github.com/YAPhoa/theatrhythm-fbl-pc/releases/latest): one file,
`theatrhythm-dlcmaker.exe` for Windows or `theatrhythm-dlcmaker.AppImage` for Linux. It needs
nothing else installed: it brings its own ffmpeg. Set the game up with the game's
release first (`theatrhythm-setup.exe`, `theatrhythm-setup`), then put the DLC maker **in the game's
release folder**: there it uses the same game folder (your decrypted dump) and save folder.
Anywhere else, it works too; you pick the two folders yourself. Its settings (`dlcmaker.ini`) are
kept next to it.

On Windows, the first start of each release unpacks its ffmpeg into
`%LOCALAPPDATA%\theatrhythm-dlcmaker` (a second, shown in the log). On Linux, allow the
AppImage to run as a program first (its file properties, or `chmod +x`); it needs FUSE, which
desktop distros and SteamOS have.

1. Start `theatrhythm-dlcmaker.exe` (Linux: `theatrhythm-dlcmaker.AppImage`, from a file manager or
   a terminal). **Game folder** and **Save folder** are already filled in from the setup; change
   them if you need to.
2. **Project folder**: pick an empty folder (**Browse...**). This is your workspace, where your DLCs
   live.
3. Press **New DLC...** and give it a name. A DLC is one pack and one series in Music Stages.
4. Select the DLC and press **New song...**. Set the song's **Stage**: `battle`, `event` or `field`.
5. Next to **Audio**, press **Choose...** and pick the music (mp3, ogg, flac, wav, m4a, ...). For an
   event song, choose a **Video** too. Set **Start**/**End** to cut the part you want.
6. Add the charts: put `charts.json` (or `basic.json`, `expert.json`, `ultimate.json`) in the song
   folder, or use **Choose...** on the chart fields. See [Charts](#charts) for the format.
7. Add a **Jacket** (square cover art) if you like, and press **Save**.
8. Press **Check**. **Problems** lists anything to fix. Errors block a build; warnings don't.
9. Close the game, then press **Install**. Start the game: the first time it shows the new songs
   as added DLC, and they're under their series in Music Stages.

If the timing is off once you play it, set **Chart offset** (`chart_offset_ms`, positive = notes
later) and install again.

The app is on the releases page from v1.0.4; with an older release of the game, use it from source
as below. On Linux, **Browse...** and **Choose...** open the desktop's
file dialog (its portal, or zenity); where there is neither, the app asks you to type the path.

## Using it from source

To run it from this repository instead, build it as the runner is built
([docs/building.md](../../docs/building.md)): from Git Bash with MSYS2 CLANG64 on Windows, with clang,
SDL3 and libvorbis on Linux (`CXX=clang++-18` on Ubuntu 24.04):

```
bash tools/dlcmaker/app/build.sh
```

That writes `tools/dlcmaker/app/out/theatrhythm-dlcmaker.exe` (linked statically) on Windows, or
`tools/dlcmaker/app/out/theatrhythm-dlcmaker` on Linux (Dear ImGui on SDL3). It is the window and the
command line in one program: with no arguments it opens the window, with a command it runs it. Set its
**Game folder** and **Save folder** yourself. It also needs:

- **ffmpeg**, found as `THEATRHYTHM_FFMPEG` or `--ffmpeg PATH` names it, next to the program, or on
  `PATH` (MSYS2's on Windows, or the system's). It decodes the audio and re-encodes event videos.
  `bash tools/dlcmaker/build-ffmpeg.sh DIR` builds the small static one releases ship.
- **The decrypted game dump**: the folder holding `eboot.bin` and
  `Media/StreamingAssets/aa/catalog.json` (`--game DUMP` or `THEATRHYTHM_GAME_DIR`).
- **The runner's save folder**: packs are installed into its `addcont/` folder. When the game also reads
  an **extra DLC folder** (setup's optional folder, `dlc=` in `paths.ini`, e.g. shadPS4's `addcont`),
  the window passes it on as `--dlc DIR`: packs there are never changed, but their IDs are skipped.

From a terminal:

```
theatrhythm-dlcmaker new "My Songs/My Pack"
theatrhythm-dlcmaker check "My Songs"
theatrhythm-dlcmaker install "My Songs" --user SAVE --game DUMP
```

`new` writes a DLC folder with an example song to start from. [Command line](#command-line) lists
every command. On Windows the program is a window app: `cmd` does not wait for it, so run it from
PowerShell, Git Bash or `start /wait`, where its output shows as it runs.

## The project folder

```
My Songs/                     your project folder: any folder of DLCs
  My Pack/                    one DLC = one pack = one series in Music Stages
    dlc.json
    logo.png                  series logo (optional; a base series' logo otherwise)
    logo_wide.png             wide logo for the large banner (optional; logo.png otherwise)
    dlc.lock.json             written by the build: the IDs given out. Keep it.
    First Song/               one song
      song.json
      audio.mp3               the music (any format ffmpeg reads)
      movie.mp4               event stage: the video (its sound is the music if there is no audio.*)
      jacket.png              square cover art (optional)
      thumbnail.png           event stage list picture, 16:9 (optional)
      charts.json             all difficulties in one file, or:
      basic.json expert.json ultimate.json [supreme.json]
    .dlcmaker-cache/          decoded audio and encoded video; safe to delete
```

Files are found by name: `audio.*`, `movie.*`, `jacket.*`, `thumbnail.*`, `logo.*` and
`logo_wide.*`. Without `audio.*` or `movie.*`, the first file with a matching extension is used.
`song.json` can also name each file. Song folders are read in name order, unless `dlc.json` lists
`songs`.

`dlc.lock.json` holds the pack, series and song numbers given out on the first build. Keeping it
keeps your scores and saves attached to the songs across rebuilds.

### dlc.json

```json
{
  "name": "My Pack",
  "label": "MyPack",
  "series": {"name": "MY SONGS"}
}
```

| key | meaning |
|---|---|
| `name` | pack name, shown in the DLC list |
| `label` | add-on folder name: 1-16 letters, digits or `_` (default: made from `name`) |
| `series.name` | series title in Music Stages (default: `name`) |
| `series.logo`, `series.logo_wide` | logo file names (default: `logo.*`, `logo_wide.*`) |
| `series.logo_from` | base series whose logo bundles are cloned as the template (default 13) |
| `series.base` | put the songs in an existing base series instead of a new one (series number) |
| `songs` | song folder names, in the order they appear (optional) |
| `package`, `series.id` | fix the pack and series numbers (optional; otherwise given out and locked) |

### song.json

```json
{
  "title": "First Song",
  "sort_name": "first song",
  "stage": "battle",
  "start": 0.0, "end": 90.5, "fade_out": 0.9,
  "preview": [65.0, 80.0],
  "copyright": "The Band"
}
```

| key | meaning |
|---|---|
| `title` | song title: a string, or `{"default": ..., "ja": ...}` per language |
| `sort_name` | Music Stages index text; must start with a-z or 0-9 (use romaji for Japanese titles) |
| `stage` | `battle`, `event` or `field` (see [Stages](#stages-battle-event-field)) |
| `start`, `end` | the cut of the source, in seconds (`end` null: to the end) |
| `fade_out` | fade at the end of the cut, in seconds |
| `preview` | the Music Stages preview, `[start, end]` in seconds (default: 15 s from a third of the way in, at most 30 s in) |
| `chart_offset_ms` | shifts every note by this many ms (to fix the sync after playing it) |
| `copyright` | text shown with the song |
| `color` | Music Stages background colour code (the template's otherwise) |
| `stage_from` | battle: the game song whose background and enemies the stage starts from. Field: the game field song whose path, camera and sky it uses |
| `background` | battle: a background ID (`"190000095"`) in place of the `stage_from` song's |
| `enemy_waves` | battle: its own enemies, `[{"boss": false, "monsters": [83, 84]}, ...]`: 1-10 waves of 1-3 monster numbers (MonsterTable ID less 250000000) |
| `enemies` | battle: a game monster pack ID (older projects; `stage_from` replaces it) |
| `template` | base song whose stage settings and bundles are cloned (default 481 battle, 508 event, 482 field) |
| `audio`, `movie`, `jacket`, `thumbnail` | file names, when they don't follow the naming above |
| `audio_from_video` | `true`: the music is the video's sound; `false`: the audio file (default: the video's when there is no audio file) |
| `id` | fix the song number (600-9999; otherwise given out and locked) |
| `clone` | a game song's number: reuse its audio and charts from your dump at build time |

## Stages: battle, event, field

- **Battle**: the party fights waves of enemies over a scrolling background. Notes run in 4 lanes. A
  battle stage starts from a game stage (`stage_from`), and you can swap its background and write your
  own enemy waves. The **Stage editor** in the window does both.
- **Event**: your video plays behind the 4 lanes. The video is re-encoded to the game's format
  (H.264, 1080p30). This is slow the first time; after that it comes from the cache while the source
  and cut stay the same.
- **Field**: the party walks a path. Notes sit at 9 heights, and holds can curve between heights. A
  field song takes its path, camera and sky from a game field song (`stage_from`; the Stage editor
  picks one).

## Charts

Times are in ms from the first sample of the cut audio (`start`), or in beats on a BPM grid. You can
mix the two. Directions are in degrees: 0 = up, clockwise, in steps of 45.

One file per difficulty (`basic.json` etc.):

```json
{"level": 2, "bpm": 120, "offset_ms": 0, "notes": [
  {"beat": 4, "type": "tap", "lane": 1},
  {"beat": 6, "type": "hold", "lane": 1, "end_beat": 8, "end_dir": 90},
  {"t": 5000, "type": "slide", "lane": 2, "dir": 270},
  {"beat": 12, "type": "twin", "lane": 0, "dir": 270, "dir2": 90, "critical": true}
]}
```

Or one `charts.json` with `bpm`, `beat0_ms`, `levels: {difficulty: level}`, optionally
`preview: {start_s, end_s}` and `stage`, and `difficulties: {difficulty: [note, ...]}`.

| note key | meaning |
|---|---|
| `t` / `beat` | time in ms, or in beats after `offset_ms`/`beat0_ms` (needs `bpm`) |
| `type` | `tap`, `slide` (needs `dir`), `twin` (two-hand slide; `dir` and `dir2`), `hold` (needs `end`/`end_beat`), `curve` (field only) |
| `lane` | 0-3, left to right (field: 0-8, the height, 0 = bottom) |
| `end_dir` | a hold's release slide direction (leave it out for a plain release) |
| `critical` | gold note (on a hold: its start) |
| `end_critical` | a hold's gold release |

Basic, Expert and Ultimate are required. Supreme is optional; without one it isn't offered. `level`
is the number shown in Music Stages (1-20).

**Field charts** put notes at 9 heights and add the curved hold, whose height moves through its path
points:

```json
{"beat": 8, "type": "curve", "lane": 4, "path": [{"beat": 9, "lane": 4}, {"beat": 10, "lane": 6}],
 "end_beat": 11, "end_lane": 7, "end_dir": 0}
```

A `hold` keeps its height. It may list `ticks` (the game's hold pass points), which change nothing we
know of.

### What Check looks for

**Errors** block a build. They are what the game can't play, or what is almost surely a mistake:

- unknown types, lanes or directions;
- notes outside the song;
- two notes in one lane at once, or a note in a lane that is being held;
- a note under 40 ms after a hold ends in its lane;
- a hold under 30 ms;
- notes 1-15 ms apart (a broken chord);
- more than 4 notes at one time.

Field charts follow the game's 519 field charts instead:

- heights 0-8, and a curve's path times inside its hold;
- at most 2 notes at one time, never two at one height;
- no note at a height while a hold holds it;
- no curve starting before the previous one ends.

They warn on chords under 2 heights apart, a curve that never moves, a release slide against the
curve's last move, and notes during a curve.

**Warnings** never block a build. They are about where the chart sits in the song, the same for
every difficulty:

- the first note comes before 1.2 s (the game's own charts start 1.26 s in or later);
- the last note ends less than 2 s before the song does (the game's leave 2.6 s or more);
- holds shorter than 100 ms;
- notes 2 ms or more off the stated BPM's 16th or triplet grid;
- no level given.

Nothing judges a chart against its difficulty: the difficulty and level are yours to pick. For
reference, the game's Basic charts average about 1 note per second, on the beat. Expert charts run
1.2-2.5 notes per second, Ultimate 1.9-4.2 and Supreme 3.2-6.5.

### Drafting charts with an AI agent

Charting by hand takes a while. [`skill/chart-song`](skill/chart-song/SKILL.md) is an agent skill (a
`SKILL.md` with its scripts, for Claude Code and other agents that read skills) that drafts the charts
from the song's audio:

- it finds the beat grid and maps the song bar by bar (onsets, drums, the melody's notes);
- it writes Ultimate first in a compact bar notation, then reduces it to Expert and Basic;
- it compiles them into `charts.json` and compares their statistics with the game's own charts.

I can't guarantee the quality of the charts it makes, but hopefully it gives you a baseline chart to
play and refine.

To use it with Claude Code:

1. Copy the `chart-song` folder into `~/.claude/skills/`, or into `.claude/skills/` in your project
   folder.
2. Install Python with `numpy` and `Pillow` for the skill's scripts (the DLC maker itself needs no
   Python).
3. Press Check once, so the song's cut audio is decoded.
4. Ask for charts for a song, for example "chart the song in `My DLC/First Song`".

The skill keeps its work in `chart-work/` in the song folder. The text charts there are the source:
edit them and run `scripts/bars.py` again, rather than editing `charts.json` by hand.

## The window

`theatrhythm-dlcmaker.exe` is a Windows window in the style of `theatrhythm-setup.exe`;
`theatrhythm-dlcmaker` on Linux is the same window, with the same fields, buttons and stage editor.

- **Project folder** (top): a folder of DLCs, or one DLC folder. Its tree is on the left: the DLCs,
  and their songs with their stage and problem counts. **New DLC...** and **New song...** create the
  folders.
- **The selection's fields** (right):
  - A DLC: pack, label and series names, and its logos, shown under their fields.
  - A song: title, sort name, stage, cut, fade, preview, chart offset and copyright. Also its audio,
    video, jacket, thumbnail and charts, each with **Choose...** to copy a file in. **Use audio from
    the video** takes the music from the video; the Audio field then greys out.
  - A song's charts per difficulty: level, notes, BPM, file and check result.
  - Grey text in an empty field is the value used when it stays empty.
  - **Save** (or Enter) writes the edits to `dlc.json` or `song.json`.
- **Problems** lists the selection's errors and warnings; **Log** shows what each action printed.
- **Check** decodes the audio and checks every chart against it. **Install** builds the packs into
  the save folder's `addcont/`; close the game first. **Export packs...** builds them into a folder
  you choose, to copy into another save's `addcont/` (see
  [Project, DLCs and packs](#project-dlcs-and-packs)).

**The Stage editor** (**Stage editor...** on a battle or field song) opens in its own window:

- **Start from** (battle): a series, then one of its songs, or **Random**. This sets the background
  and the enemies to that stage's own.
- **Background**: a series, then a background, named after a song of that series that uses it. For a
  field song this is the only row: a field from the game's field songs, with its camera and sky.
- **Enemies** (battle): up to 10 waves of up to 3 monsters, shown as on the game's stage select. A
  boss wave is tinted and waits until it is beaten. Each wave has **Boss**/**Not boss**, **Up** and
  **Delete**; × removes one monster.
- Clicking a monster, a **+** or **Add wave** opens the monster picker on the right: a search box and
  every monster in the base game with its card picture. Colour variants that share a name are
  numbered ("Ahriman 1" to "Ahriman 5").
- **Restore default** goes back to the chosen stage's own background and enemies. **Done** saves to
  `song.json`; **Cancel** (or Esc) leaves it as it was.

The stages and monsters come from your dump. They are read the first time the editor opens (a few
seconds) and cached in the temp folder. Only stages whose monsters and background are in the base
game are offered.

The window keeps no project logic of its own: it runs its own program with a command for
everything, so it behaves exactly like the command line. Its settings are in `dlcmaker.ini` next to
the exe.

## Command line

```
theatrhythm-dlcmaker new     FOLDER             start a DLC folder with an example song
theatrhythm-dlcmaker check   PATH               parse and check (decodes audio, not video)
theatrhythm-dlcmaker build   PATH --out DIR     build each DLC into DIR/<label>/
theatrhythm-dlcmaker install PATH --user SAVE   build into SAVE/addcont/
                         [--dlc DIR]            also skip the IDs of the packs in the game's extra DLC folder
theatrhythm-dlcmaker extract SONG FOLDER        a game song as a song folder (private tests)
```

`PATH` is one DLC folder (it has `dlc.json`) or a project folder of them. All commands take
`--game DUMP` (or `THEATRHYTHM_GAME_DIR`) and `--ffmpeg PATH`.

- `check` prints the parsed tree, IDs, chart stats, errors and warnings, and exits with code 1 on
  errors.
- `build` and `install` run the same checks first and stop on any error.
- `install` refuses to run while the game is running, since the add-on files would be in use.

**Game songs as song folders.** `theatrhythm-dlcmaker extract 481 FOLDER --game DUMP --title "..."`
writes a game song as an ordinary song folder: its audio as `audio.ogg`, its charts as
`charts.json`, the event video, jacket and thumbnail, and a `song.json` with the song's preview,
colour and stage. All 881 battle and event charts and 519 field charts convert back to the same
notes. The folder holds the game's own files, so it is only for private tests and as a starting
point; never share it.

A song with `"clone": 482` in `song.json` instead reuses that game song's audio and charts from your
dump at build time, so nothing is copied into the project. Set `stage` to the game song's.

## Troubleshooting

- **The game stays on the loading notes at boot.** A pack failed to load. Uninstall packs (delete
  their folders from `addcont/`) until it boots, then rebuild that one with the current dlcmaker.
- **"Going to Music Stage" never finishes.** Packs built before the jacket lottery fix (see below)
  freeze the game once 5 or more DLC songs are installed. Install them again.
- **Notes are early or late.** Set `chart_offset_ms` in `song.json` (positive = later) and install
  again.
- **Install says the game is running.** Close it first; the game keeps the pack files open.
- **A field stage shows only a grid.** That is Auto mode's look. Use Play to see the field.
- **Check is slow the first time.** It decodes the audio once, into `.dlcmaker-cache/`.

## How a build maps onto the game

- **IDs.** A pack gets `DLCPACKnnnnnnn` (100-999). Its series gets `SERIESnnnnnnn` (200-999), in its own
  sort group. Each song gets `MUSICnnnnnnn` (600-9999). IDs already used by other packs in the target
  `addcont/` are skipped, and so are those of the packs in the game's extra DLC folder (`--dlc`; its
  `<title ID>/` folder too), and once given out they are kept in `dlc.lock.json`.
- **Tables.** Rows go into the 10 pack tables (Music, Sound, Series, DLCMusic, DLCPack, ...), cloned
  from a template song's rows.
- **Series.** The series row has a `JacketLottery` weight of 0.025, like the game's own DLC group.
  Music Stages draws its sub-jackets from weighted groups only. With weight 0 and 5 or more DLC songs
  installed, it never finds one of them, and the game freezes on "Going to Music Stage".
- **Battle stages.** The Music row takes the `stage_from` song's background and monster pack. A song
  with `enemy_waves` gets its own `MonsterPackTable` row (260010000 + the song number), and every
  monster number is checked against the game's MonsterTable.
- **Field stages.** The Music row is a field song's: the template's, or `stage_from`'s background,
  start position, camera effect, sky box and shadow. Charts use musicType 0, with curved holds as
  CurveHold records.
- **Text.** Text goes to 5 I2 text sources: song names, sort names, series name, pack name and
  copyright.
- **Audio.** Audio is decoded to 48 kHz stereo and stored as FSB5 PCM16 in cloned AudioClip bundles
  (the full song and the preview). The game's `MusicLength` is the audio length + 1 s.
- **Charts.** Each difficulty becomes a `TriggerFile` v1.0 TextAsset.
- **Art.** Jacket, thumbnail and logo bundles are cloned under the new names and re-encoded from your
  images (BC7/DXT1, PS4 tiling).
- **Event video.** The video is re-encoded to H.264 Baseline 4.0, 1080p30, no audio, 3 s longer than
  the music; a video already in that form is only cut. It goes to
  `StreamingAssets/Movie/EMS/MUSICnnnnnnn.mp4` inside the pack, which the runner serves in place of
  the game's files.
- **Pack files.** `catalog.json` (the base catalog plus the pack's keys) and `dlcparam.json` complete
  the pack.

The runner's side is `src/runtime_content.c`: every folder in `<save>/addcont/` is an installed
add-on, mounted at `/addcont<n>` when the game asks for it. Packs have no bonus items, so the
runner also hides the "DLC songs added" window's bonus row when it would be empty
(`src/runtime_dlc_bonus.c`).

## Not done yet

- Importing `.chart` or MIDI charts, and reducing a dense chart to the easier difficulties.
- A chart preview in the window (notes drawn over the song, playing with the audio).
