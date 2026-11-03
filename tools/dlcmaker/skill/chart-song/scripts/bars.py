# SPDX-License-Identifier: GPL-2.0-or-later
"""Hand-written bar charts -> the song's charts.json, with statistics to compare against the game's.

    python bars.py SONG [--dry]

Reads SONG/chart-work/{basic,expert,ultimate,supreme}.txt and timing.json (from analyze.py timing),
writes SONG/charts.json (one note per line), and prints each chart's statistics. Check the result
with `theatrhythm-dlcmaker check DLC`.

Chart text: one line per bar, `BAR: token token ...`; '#' starts a comment; `@level N` sets the level.
A position is `B.s` (beat B from 1, sixteenth s 0-3) inside the line's bar, `B+n/d` for a tuplet
(beat B plus n/d of a beat: 2+1/3, 2+2/3), or `BAR:B.s` for another bar (a hold's end).
Tokens (L = lane 0-3 left to right; on a field stage the height 0-8, 4 = middle. D = a keypad digit
for a direction: 8 up, 9 up-right, 6 right, 3 down-right, 2 down, 1 down-left, 4 left, 7 up-left):
    POS t L                      tap                       1.0t2
    POS s L D                    slide                     3.0s16
    POS w L D D                  twin slide (two hands)    4.2w146
    POS h L >POS                 hold to POS               1.0h2>2.2   1.0h2>12:1.0 (field: /POS ticks)
    POS c L /POS=L ... >POS=L    field curve: height path  1.0c4/2.0=6>3.0=7
After a hold's end: ^D for a release slide, then * for a critical release; a trailing ! makes the
note (a hold's start) critical, as does one right after the lane:    1.0h2>2.2^8*!   1.0c4!/2.0=6>3.0=7
Notes are on timing.json's grid: bar 0 starts at beat0_ms; with "use_tracked": true, on the tracked beats.
"""
import json, re, sys
from collections import Counter
from pathlib import Path

KEY = {'8': 0, '9': 45, '6': 90, '3': 135, '2': 180, '1': 225, '4': 270, '7': 315}
POS = r'(?:(-?\d+):)?([1-9])(?:\.([0-3])|\+(\d+)/(\d+))'
TOK = re.compile(rf'^{POS}([tswhc])(\d)(.*)$')
FILES = ['basic', 'expert', 'ultimate', 'supreme']

class Grid:
    def __init__(self, song):
        p = song / 'chart-work' / 'timing.json'
        if not p.is_file(): sys.exit(f'{p} is missing: run `python analyze.py timing {song}` first')
        c = json.loads(p.read_text()); self.c = c
        self.bpm, self.beat0, self.bar_beats = c['bpm'], c['beat0_ms'], c.get('bar_beats', 4)
        self.tracked = c['beats_ms'] if c.get('use_tracked') else None
    def ms(self, beat):
        b = self.tracked
        if b and 0 <= beat <= len(b) - 1:
            i = min(int(beat), len(b) - 2); return int(round(b[i] + (beat - i) * (b[i + 1] - b[i])))
        return int(round(self.beat0 + beat * 60000.0 / self.bpm))

def position(m, bar, grid):
    """Beat index from bar 0 of a POS match's groups (bar, beat, sixteenth, n, d)."""
    b = int(m[0]) if m[0] else bar; beat = int(m[1]) - 1
    if beat >= grid.bar_beats: raise ValueError(f'beat {beat + 1} in a {grid.bar_beats}-beat bar')
    frac = int(m[2]) / 4 if m[2] is not None else int(m[3]) / int(m[4])
    return b * grid.bar_beats + beat + frac

def parse_pos(text, bar, grid):
    m = re.fullmatch(POS, text)
    if not m: raise ValueError(f'bad position {text!r}')
    return grid.ms(position(m.groups(), bar, grid))

def parse(path, grid, field):
    notes, level, top = [], None, 8 if field else 3
    for ln, line in enumerate(path.read_text(encoding='utf8').splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if not line: continue
        where = f'{path.name}:{ln}'
        if line.startswith('@level'): level = int(line.split()[1]); continue
        bar_s, _, rest = line.partition(':')
        try: bar = int(bar_s)
        except ValueError: raise SystemExit(f'{where}: a line is `BAR: tokens`')
        for tok in rest.split():
            src = f'{where} {tok}'; crit = '!' in tok; m = TOK.match(tok.replace('!', ''))
            if not m: raise SystemExit(f'{src}: bad token')
            try:
                g = m.groups(); kind, lane, extra = g[5], int(g[6]), g[7]
                if lane > top: raise ValueError(f'lane {lane} (0-{top} on this stage)')
                n = {'t': grid.ms(position(g[:5], bar, grid)), 'type': None, 'lane': lane}
                if kind == 't':
                    if extra: raise ValueError('a tap takes nothing after its lane')
                    n['type'] = 'tap'
                elif kind == 's': n['type'] = 'slide'; n['dir'] = KEY[extra]
                elif kind == 'w': n['type'] = 'twin'; n['dir'], n['dir2'] = KEY[extra[0]], KEY[extra[1]]
                else:
                    end_crit = extra.endswith('*'); extra = extra.rstrip('*'); end_dir = None
                    if '^' in extra: extra, d = extra.split('^'); end_dir = KEY[d]
                    body, end = extra.split('>')
                    if kind == 'h':  # field holds may list the game's pass ticks: /POS before >
                        n['type'] = 'hold'; n['end'] = parse_pos(end, bar, grid)
                        if body: n['ticks'] = [parse_pos(p, bar, grid) for p in body.split('/') if p]
                    else:
                        if not field: raise ValueError('curves are for field stages')
                        n['type'] = 'curve'; n['path'] = []
                        for p in [p for p in body.split('/') if p]:
                            ps, pl = p.split('='); n['path'].append({'t': parse_pos(ps, bar, grid), 'lane': int(pl)})
                        es, el = end.split('='); n['end'] = parse_pos(es, bar, grid); n['end_lane'] = int(el)
                    n['end_dir'] = end_dir
                    if end_crit: n['end_critical'] = True
            except (ValueError, KeyError, IndexError) as e: raise SystemExit(f'{src}: {e or "bad direction"}')
            if crit: n['critical'] = True
            notes.append(n)
    notes.sort(key=lambda n: (n['t'], n['lane']))
    return notes, level

def stats(notes, field):
    """What to compare with the game's charts (see SKILL.md). nps counts notes over the first to the last
    note on battle and event stages, and every judged hit (hold ends too) on a field, as the studies did."""
    heads = [n['t'] for n in notes]; ends = [n['end'] for n in notes if 'end' in n]
    judged = len(heads) + len(ends); span = (max(heads + ends) - min(heads)) / 1000 or 1
    nps = judged / span if field else len(notes) / ((max(heads) - min(heads)) / 1000 or 1)
    kinds = Counter(n['type'] for n in notes); ts = sorted(set(heads)); gaps = [b - a for a, b in zip(ts, ts[1:])]
    chords = sum(1 for t, c in Counter(heads).items() if c > 1)
    crit = sum(1 for n in notes if n.get('critical')) + sum(1 for n in notes if n.get('end_critical'))
    seq = [n for n in notes if Counter(heads)[n['t']] == 1]; moves = Counter()
    for a, b in zip(seq, seq[1:]): moves[min(abs(b['lane'] - (a.get('end_lane', a['lane']))), 3)] += 1
    held = sum(n['end'] - n['t'] for n in notes if 'end' in n) / 1000
    out = {'notes': len(notes), 'judged': judged, 'nps': round(nps, 2),
           'types': dict(kinds), 'chords': chords, 'critical': round(crit / judged, 3),
           'gap_min_ms': min(gaps) if gaps else 0, 'gap_median_ms': sorted(gaps)[len(gaps) // 2] if gaps else 0,
           'held_share': round(held / span, 2), 'first_ms': min(heads), 'last_ms': max(heads + ends)}
    total = sum(moves.values()) or 1
    out['same_lane' if not field else 'same_height'] = round(moves[0] / total, 2)
    if not field: out['lane_moves'] = {k: round(v / total, 2) for k, v in sorted(moves.items())}
    else: out['heights'] = dict(sorted(Counter(n['lane'] for n in notes).items()))
    return out

def write(path, head, charts):
    """charts.json with one note per line: readable and diffable."""
    lines = ['{'] + [f' {json.dumps(k)}: {json.dumps(v, ensure_ascii=False)},' for k, v in head.items()] + [' "difficulties": {']
    for i, (d, notes) in enumerate(charts.items()):
        lines.append(f'  {json.dumps(d)}: [')
        lines += [f'   {json.dumps(n)}' + (',' if j < len(notes) - 1 else '') for j, n in enumerate(notes)]
        lines.append('  ]' + (',' if i < len(charts) - 1 else ''))
    path.write_bytes(('\n'.join(lines + [' }', '}']) + '\n').encode())
    json.loads(path.read_text(encoding='utf8'))

def main(argv):
    if not argv: sys.exit(__doc__)
    song = Path(argv[0]); grid = Grid(song)
    stage = json.loads((song / 'song.json').read_text(encoding='utf8')).get('stage', 'battle')
    field = stage == 'field'; charts, levels = {}, {}
    for d in FILES:
        p = song / 'chart-work' / f'{d}.txt'
        if not p.is_file(): continue
        charts[d], level = parse(p, grid, field)
        if level: levels[d] = level
        print(f'{d}: ' + json.dumps(stats(charts[d], field)))
    if not charts: sys.exit(f'no charts in {song / "chart-work"} (basic.txt, expert.txt, ultimate.txt, supreme.txt)')
    head = {'stage': stage, 'levels': levels}
    if not grid.tracked: head.update(bpm=grid.bpm, beat0_ms=grid.beat0)  # Check's grid warnings use them
    if '--dry' not in argv:
        write(song / 'charts.json', head, charts); print(f'wrote {song / "charts.json"}')

if __name__ == '__main__': main(sys.argv[1:])
