---
name: chart-song
description: Draft Theatrhythm Final Bar Line charts (Basic, Expert, Ultimate, optionally Supreme) for a dlcmaker song folder from its audio. Finds the beat grid, maps the song bar by bar, writes the charts in a bar notation and compiles them to charts.json, then checks them with dlcmaker. Use when asked to chart a song, make notes or charts for a custom song, or redo a dlcmaker song's charts.
---

# Charting a song for dlcmaker

You make a **baseline**: charts that are on the beat, follow the music and sit at the right density for
each difficulty. They are not tested in play. The person plays them and fixes what feels wrong, so say
what you were unsure of when you hand them over.

This process was worked out on two songs: a 190 BPM rock song charted as a battle stage, and an
88 BPM ballad charted as a field stage. Both were compared with the game's own charts, and the numbers
below come from those comparisons.

`SCRIPTS` below is this skill's `scripts/` folder. `SONG` is the song folder (it has `song.json`), and
`DLC` is the folder above it (it has `dlc.json`). The dlcmaker guide is `tools/dlcmaker/README.md` in
the theatrhythm-fbl-pc repository; its "Charts" section defines the JSON format these scripts write.

## Before you start

- **The song folder.** It needs a `song.json` with the right `stage` (`battle`, `event` or `field`)
  and the cut (`start`, `end`), plus the audio or video. Ask the person for the stage if it isn't set.
  If they want a different cut, change it now: every time in the chart is counted from `start`.
- **Python.** Use Python 3.10 or newer with `numpy` and `Pillow`:
  `python -m pip install numpy Pillow`. The DLC maker itself is a program of its own
  (`theatrhythm-dlcmaker`), not Python.
- **The cut audio.** Run `theatrhythm-dlcmaker check DLC`, or press Check in the dlcmaker window. Check
  decodes the cut the game plays into `DLC/.dlcmaker-cache/<song>/song.wav`, and the scripts read it
  from there. Its errors about missing charts are expected at this point. If the cut changes later,
  run Check again, then `features` and `timing` again.
- **Privacy.** The audio is the person's. Keep it, and everything made from it (`chart-work/`), on
  their machine. Don't upload any of it.

All the work goes in `SONG/chart-work/`: the analysis, `structure.md` and the chart text files. The
compiled charts go to `SONG/charts.json`, which replaces any per-difficulty `.json` charts.

## 1. Timing

```
python SCRIPTS/analyze.py features SONG
python SCRIPTS/analyze.py timing SONG
```

`features` takes about 10 s per minute of audio. `timing` prints tempo candidates, fits a beat grid,
reports drift and picks the downbeat. It then writes `chart-work/timing.json`: the BPM, `beat0_ms`
(the start of bar 0) and `bar_beats`.

Check the result. Don't trust it blindly:

- **Double or half tempo** is the usual mistake. The top candidate is often twice the felt tempo for
  ballads, or half of it for fast songs. Pick the beat a listener would clap to (most songs land
  between 70 and 180 BPM), then rerun with `--bpm N`.
- **The downbeat.** In `analyze.py map SONG 8 11`, a pop or rock backbeat shows `low` hits on slots 0
  and 8 and `snare` on 4 and 12 (beats 1 and 3, then 2 and 4). If the snare lands on 0 and 8, the
  bar is a beat off: rerun with `--downbeat` set to another phase. `timing` prints the kick strength of
  each phase. Sections should start on bar numbers that are multiples of 4 or 8 in `map --brief`.
- **Drift.** A steady song stays within about 15 ms in every 20 s window. A live recording that drifts
  more needs `"use_tracked": true` in `timing.json`. Then positions follow the tracked beats, and
  charts.json gets no BPM.
- **Metre.** Use `--bar-beats 3` for a waltz. For other tuplets, write the positions as `B+n/d`.
  A song that changes tempo partway can't be charted on one grid with these tools; tell the person.

## 2. Map the song

```
python SCRIPTS/analyze.py map SONG --brief        one line per bar: loudness, hits, melody range
python SCRIPTS/analyze.py map SONG 16 23          bars 16-23 in detail
python SCRIPTS/analyze.py sheet SONG 16 31        a PNG of those bars, if you can look at images
```

A detailed bar looks like this:

```
bar 10  13.218-14.481 s
          1...2...3...4...
  low     474.6...3.98744.
  snare   9.........3.94..
  hat     6..5........38..
  all     8..3........55..
  melody  66..9.3...637344
  notes   0:A3/1  1:F4/1  4:F3/1  5:F4/6  11:E4/1  13:G#3/1  14:C4/3
```

- The columns are the bar's 16ths. A digit from 3 to 9 is an onset's strength against that band's
  strong hits; `.` means none, or a weak one.
- `low` is kick drum plus bass attacks, so it is busy in songs with a picked bass. A 9 in `hat` on a
  beat is usually a crash.
- `notes` is a rough transcription of the centre-panned melody, usually the vocal, as
  `slot:pitch/length in 16ths`. Trust the long notes and the contour, not every name. Octave slips
  happen (an F3 among F4s), and in instrumental parts the bass or guitars leak in. The sheet's red
  line is the same pitch track over the centre-channel spectrogram.

Write `chart-work/structure.md` before charting:

- **A section table:** bars, times, section name, what leads (vocal, riff, drums), the big hits
  (crashes, stops, fills) and the held notes, with their positions.
- **A plan per difficulty:** the target note count and density (see the table below), what each
  section follows, where the criticals go and the level.

## 3. Write the hardest chart first

Write `chart-work/ultimate.txt` (or `supreme.txt` if there is one) one bar per line, then reduce it
(step 4). The notation, also in `bars.py`'s docstring:

```
@level 9
# one line per bar; '#' starts a comment
10: 1.0t1 1.0t2 2.0h2>3.2 4.0s16      # chord, hold to 3.2, slide right on beat 4
11: 1.2t1 2.0t1 3.0t0 4.0s09          # down-left to right: the line turns up
16: 1.0t0! 1.2h1>2.2^8 3.0h3>17:1.0^2 # critical tap; hold with an up release; hold into bar 17
```

- **Position:** `B.s` is beat B (from 1) and 16th s (0-3) in the line's bar. Write `B+n/d` for
  triplets (`2+1/3`), and `BAR:B.s` for an end in another bar.
- **Tokens:** `POS t L` tap, `POS s L D` slide, `POS w L DD` twin, `POS h L >POS` hold and (field
  only) `POS c L /POS=L ... >POS=L` curve, written without spaces (`3.0s16`).
- **L** is the lane, 0-3 from left to right. On a field it is the height, 0-8 from the bottom, with 4
  the middle.
- **D** is a keypad digit: 8 up, 9 up-right, 6 right, 3 down-right, 2 down, 1 down-left, 4 left,
  7 up-left.
- **Hold ends:** `^D` adds a release slide and `*` makes the release critical. A `!` anywhere in the
  token makes the note, or the hold's start, critical.

### On every stage

- Put each note on a sound you can point at: an onset digit of about 5 or more, or a melody note's
  start. Leave silence empty.
- Follow one layer at a time: the vocal while it sings, and the riff or drums in instrumental parts.
  Change layers at section boundaries, not in the middle of a phrase.
- Repeated music gets the same pattern. A chorus that comes back is charted the same way, apart from
  its own differences.
- **Start and end:** the first note at 1.2 s or later, and the last note ending at least 2 s (better
  2.6 s) before the cut ends. The last note is critical, usually a twin, a chord of two criticals or a
  hold with a release slide.
- **Criticals** go on section entries, the climax and the last note.

### Battle and event (lanes 0-3)

- **Lanes follow the melody's contour:** higher pitch further right, and a repeated pitch stays in
  its lane. In the game's charts, about two thirds of notes stay in the previous lane and a third move
  one lane. Jumps of two or more lanes are rare (under 2%) and saved for leaps.
- **Taps** are about half the notes. They go on syllables and hits.
- **Slides** are 20-25%. They go on crashes, accents and phrase ends. Their direction follows the
  pitch move, or alternates on repeated hits. Keep them cardinal (up, down, left, right): all of them
  in Basic, about 80% in Expert and 70% in Ultimate.
- **Twins** are 1-3%, on the biggest crashes.
- **Holds** are 20-25%, on sustained notes, 1-2 beats long. About a quarter end in a release slide
  toward where the melody goes next.
- At most 2 notes at once, except at a big ending. Never two notes in one lane, and no note in a lane
  while it is held or within 40 ms of the hold's end. Only Ultimate puts notes beside a hold, and
  only rarely.

### Field (heights 0-8, curves)

- **Height follows pitch,** with each section's register centred on height 4. In the game's charts
  about 75% of next notes sit at the same height, so keep steady pitches level and move on real
  leaps.
- **Hold for about half the song's time.** About 90% of holds are curves: a sustained note whose
  pitch moves, with a path point at each new pitch (1-2 points). The next note usually starts at the
  height where the curve ended, and a release slide follows the curve's last move.
- At most 2 notes at once, never at one height, and usually 4 heights apart, on band hits. Put no
  notes during a curve.
- Criticals are 5-11% in the game's field charts.

## 4. Reduce to the other difficulties

- **Expert:** drop the 16th neighbours, ornaments and some chords, and hold where Ultimate repeats.
  Gaps are mostly an 8th or more, and the smallest is about 180 ms.
- **Basic:**
  - **Battle and event:** notes on the beat (three quarters of them) and gaps of about 400 ms or
    more. Lanes stay or move one, never jump. Put nothing during a hold, and give each slide a beat
    of rest before and after it. Holds run about 2 beats.
  - **Field:** draw whole phrases as long curves, with notes mostly on beats and about 400 ms or
    more apart.
- **Supreme** (optional) goes above Ultimate. Without it, the game doesn't offer one.

Keep each reduction inside its parent: a note in Basic is a note in Expert at the same time, so the
difficulties read as one chart at three densities.

## 5. Compile and check

```
python SCRIPTS/bars.py SONG          writes SONG/charts.json and prints each chart's statistics
theatrhythm-dlcmaker check DLC       the game's rules: errors block a build, warnings don't
```

Compare `bars.py`'s numbers with the game's medians. Battle and event figures come from 36 songs'
charts. Field figures come from 159 field songs and 11 ballads among them. `nps` counts notes from the
first to the last note on battle and event stages, and every judged hit (hold ends too) on a field.

| | Basic | Expert | Ultimate | Supreme |
|---|---|---|---|---|
| battle/event nps | 0.9-1.1 | 1.5-1.7 (1.2-2.5) | 2.3-3.1 (1.9-4.2) | 3.2-6.5 |
| field nps | 0.8-0.9 | 1.2-1.4 | 1.7-2.2 | |
| smallest gap, ms | about 400 | about 180 | about 150 | |
| same lane / height | 0.56 / 0.75 | 0.64 / 0.71 | 0.67 / 0.74 | |
| criticals | about 6% | about 4% | about 3% | |
| level (battle) | 2-3 at about 1 nps | 4-5 at about 1.5-1.6 | 8 at 2.3, 9 at 2.7, 10 at 3.1 | 11 and up |

Field levels: ballads sit at Basic 2-4, Expert 4-6 and Ultimate 6-10. Pick levels by density and by
how hard the patterns are. The person can change them in `@level`.

Fix every Check error, rerun `bars.py`, and check again. Warnings about notes off the BPM grid mean a
position is wrong, unless you meant it.

## 6. Hand over

Tell the person:

- what each section follows, the levels and the note counts;
- what you were unsure of: the tempo choice, unclear melody parts, sections charted from the drums;
- how to try it: Install from the dlcmaker window, then play it in Music Stages;
- if every note is early or late by the same amount, set `chart_offset_ms` in `song.json` (positive
  means later) and install again;
- the text files in `chart-work/` are the source: edit them and run `bars.py` again, rather than
  editing charts.json by hand.
