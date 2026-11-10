# Save editor

Edits Theatrhythm Final Bar Line's save files on PC. Open one, pick something on the left (a
character, an item, a quest stage, a song's score...) and change its values on the right.

**Close the game before you save.** The game keeps your save in memory and writes it back when it
saves, which would undo your changes (or yours would undo its progress). The editor refuses to save
while `theatrhythm-fbl.exe` is running.

## The files

Your saves are in the game's save folder (`save=` in the game's `paths.ini`), under
`savedata/1/CUSA32853/SaveData/`:

| File | What it holds |
|---|---|
| `backup.inf` | Most of your progress: player stats, Series Quests (worlds and stages), characters and their abilities, parties, summon stones, moogles, airships, items, profile card illustrations, CollectaCards, theater movies, Options, feats seen, tips and help |
| `musicBackup.inf` | Songs: which are unlocked and favourites, and each difficulty and play style's high score, rank, chain, clear mark and counts |
| `proficaBackup.inf` | Your profile card |

The tree shows what the file you opened holds. Each file is the game's own format: encrypted,
compressed MessagePack (see [docs/development.md](../../docs/development.md#save-files)).

## Using it

- **Save file**: the editor opens the save folder's `backup.inf` by itself when it is unpacked in
  the game's release folder (it reads the game's `paths.ini`), else the last file you opened.
  **Open...** picks another.
- **Game folder**: your decrypted dump. It is only read, for names: with it, entries show as
  "Cloud" or "Potion"; without it, as "Character 230000031" or "Item 270000000".
- **Find**: type a name and press Enter or **Find**. An exact name is found first ("Cloud" is the
  character, not "Cloud Smiles"); press it again for the next one.
- **Values**: tick boxes are on/off values; numbers are checked as you type and kept only when they
  are whole numbers the game's field can hold. Greyed-out values (dates, the save's version, account
  IDs) are shown but can't be changed.
- **Pictures instead of IDs**: where the save holds a character, moogle, airship or summon (a
  party's members, moogle and airship; a summon stone's summon), the editor shows its picture and
  name instead of the ID.
  **Change...** (or clicking the picture) opens a grid of pictures, as the DLC maker's monster
  picker does: click one to choose it. Ones you haven't unlocked yet, and characters already in
  that party, are greyed out and can't be picked, because the game wouldn't allow them either.
- **Moogles and Airships**: these pages show one at a time. **Selected:** picks which (locked ones
  too, greyed out), and its values show below, where you can unlock it.
- **Change all entries**: a list (characters, moogles, items, CollectaCards...) offers to switch one
  on/off value on every entry at once (**All on** unlocks every moogle), or to set one number on all
  of them (every item's count).
- **Save** writes the file in the game's format. The first save of a session copies the file it
  replaces into `save-backups/` next to the editor, named with the date and time. To undo, copy that
  file back over the save (with the game closed) and rename it to `backup.inf` (or
  `musicBackup.inf`, `proficaBackup.inf`).

The game may react to what you change the next time it starts: unlocking every moogle earns the
"Acquired every moogle style!" feat, for example. Unlocking things the game would never unlock (DLC
content you don't own, characters it hasn't introduced yet) is untested.

## Command line

The same exe, with a command:

```bash
theatrhythm-saveeditor.exe dump backup.inf > backup.json
```

prints the file's contents as JSON.

```bash
theatrhythm-saveeditor.exe check backup.inf musicBackup.inf proficaBackup.inf
```

decrypts each file, writes it back in memory and checks that it reads the same, byte for byte.

```bash
theatrhythm-saveeditor.exe set backup.inf edited.inf "RhythpoInfo/TotalRhythpoValue=12345" "MyMoogleInfo/MyMoogles/*/UnlockedFlag=true"
```

writes a copy with values changed: keys and array indexes split by `/`, `*` for every entry.
`rewrite IN OUT` writes a copy with nothing changed.

## Building

Windows, Git Bash with MSYS2 CLANG64 (as for the runner, [docs/building.md](../../docs/building.md)):

```bash
bash tools/saveeditor/app/build.sh
```

builds `tools/saveeditor/app/out/theatrhythm-saveeditor.exe`, linked statically. Names are read with
the DLC maker's core (`tools/dlcmaker/core`). There is no Linux build yet (the crypto uses Windows'
`bcrypt`) and no release package.
