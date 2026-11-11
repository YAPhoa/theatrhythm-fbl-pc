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
  picker does: click one, then **OK** (or double-click it). Characters already in that party are
  greyed out. Ones you haven't unlocked yet are greyed out too; choosing one offers to unlock it.
  An airship can also be **None**. A party's page has all four members. Each
  member's abilities are chosen from a list of that character's abilities, learned ones first (the
  ones in the member's other slots are left out).
- **Parties**: the **Parties** page lists the parties, one row each with its members' pictures,
  their names and its summon stone; **Current** marks the party the game has selected. Click a row
  (or select it and press Enter) to open the party. **Up**, **Down** and **Remove** show on the row
  under the mouse or the selected row; Alt+Up and Alt+Down move the selected party and Delete
  removes it. Moving keeps the game's current party the same party. **Add a party**, under the list,
  adds an empty one. **Remove** never goes below the game's 5: the game only switches between
  Parties 1 to 5, so parties after Party 5 are listed under "Not selectable in the game". **Party
  Edit** on the same page holds the last sort types the game used, chosen by name.
- **Characters**: one page per character: their values, their **Last set abilities** (chosen from
  lists, as a party member's are) and a table of their abilities with an **Unlocked** box each.
- **Summon stones**: the **Stones** page lists the stones you have, numbered in order, each with
  its summon's picture and effects, in rows like the parties': click a row (or Enter) to open it,
  **Up**, **Down** and **Remove** on the row under the mouse or the selected one, or Alt+Up,
  Alt+Down and Delete. **Add a stone...** picks a summon and adds a stone found by you with one
  effect (EXP received, rarity 1). Parties point at their stone by its number, so the editor keeps
  each party on the same stone (a party whose stone is removed has none). A stone's page has its
  three effects, each chosen from a list by name with its rarity (**None** for an empty slot), and
  **Locked** is the game's lock that keeps it from being sold. A party's **Summon stone** is chosen
  from the list of stones. The game allows 999 stones.
- **Moogles and Airships**: these pages show one at a time. **Showing:** picks which (locked ones
  too, greyed out), and its values show below, where you can unlock it.
- **Song select**: the last selected song is chosen from every song, listed under its series in the
  game's order (a name a series has twice shows its stage type, BMS, FMS or EMS); the sort type and
  sort category are chosen by the game's own names (All, Favorites; Series, Cleared Rank...).
- **Theater**, **Profile card illustrations**, **Help** and **Feat notices seen**: one table each,
  every movie with an **Owned** box, every illustration with an **Unlocked** box, every help page with
  **Unlocked** and **Read** boxes and every feat with **Hint viewed** and **Seen** boxes; click a box
  (or select a row and press Space) to tick it. Illustrations that share a name are numbered, as in the tree. Drag the line between a
  table's column headers to widen either column.
- **All ...** (on a list's page): **Unlock all** / **Lock all** (or **All on** / **All off**)
  switches one value on every entry at once, and **Set on all** sets one number on all of them
  (every item's count). Turning things off, setting a number, or changing more than a few entries
  asks first.
- **Undo** (Ctrl+Z) takes back the last change of many values at once, a picture choice, or a stone
  added, moved or removed (stones and parties). Ctrl+S saves and Ctrl+F goes to Find.
- **Save** writes the file in the game's format. Every save first copies the file it replaces into
  `save-backups/` next to the editor, named with the date and time. **Backups...** > **Restore a
  backup...** puts one back (the file it replaces is backed up too); **Open the backup folder**
  shows them. Close the game before saving or restoring: the editor warns when it is running.

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
