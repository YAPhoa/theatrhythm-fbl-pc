# SPDX-License-Identifier: GPL-2.0-or-later
"""Audio analysis for charting a dlcmaker song by hand (numpy; Pillow for `sheet`).

    python analyze.py features SONG [--wav PATH] [--lo 52 --hi 84]   onset curves, melody pitch (MIDI range)
    python analyze.py timing   SONG [--bpm N] [--lo 70 --hi 200] [--downbeat P] [--bar-beats 4]
    python analyze.py map      SONG [BAR0 [BAR1]] [--brief]
    python analyze.py sheet    SONG BAR0 BAR1 [--out PNG]

SONG is a song folder (it has song.json). The audio is the cut the game plays, which
`theatrhythm-dlcmaker check DLC` decodes into DLC/.dlcmaker-cache/<song>/song.wav, so times here are
the charts' times. Everything is written to SONG/chart-work/.

features  5 ms frames: onset strength of the whole mix, kick (percussive 35-120 Hz), snare
          (percussive 150-350 Hz + 1.5-6 kHz), hats/cymbals (7-16 kHz) and the melody (the
          centre-panned harmonic part, 250-3000 Hz); the melody's pitch from harmonic salience on the
          centre channel; log-frequency spectrograms for sheets.
timing    tempo candidates (Fourier magnitude of the onset curve with its 8th and 16th harmonics), a
          dynamic-programming beat track, a straight-line fit and the drift from it, the bar's first
          beat (the strongest kick phase), and a regression of strong onsets onto the 8th grid. Writes
          chart-work/timing.json: bpm, beat0_ms (a downbeat: bar 0), bar_beats and beats_ms (the
          tracked beats; set "use_tracked": true in it for a song that drifts).
map       one block per bar: the onsets on each 16th (3-9 by strength against the band's strong hits,
          '.' for none or weak) for low (kick and bass attacks), snare, hat (hats and cymbals), all
          (the whole mix) and melody, and the melody notes (16th slot:name/length in 16ths). --brief: one line per bar to find sections.
sheet     a PNG of bars BAR0-BAR1 (spectrogram, centre spectrogram with the pitch line, onset strips,
          beat and bar lines) for a look at the music.
"""
import json, sys, wave
from pathlib import Path
import numpy as np

SR, HOP = 48000, 240
FPS = SR / HOP
NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
BANDS = {'low': 'kick', 'snare': 'snare', 'hat': 'hat', 'all': 'all', 'melody': 'melody'}  # map row: curve

def work(song): w = Path(song) / 'chart-work'; w.mkdir(exist_ok=True); return w

def song_wav(song):
    song = Path(song).resolve(); wav = song.parent / '.dlcmaker-cache' / song.name / 'song.wav'
    if not wav.is_file(): sys.exit(f'{wav} is missing: run `theatrhythm-dlcmaker check {song.parent}` first (it decodes the cut)')
    return wav

def read_wav(path):
    with wave.open(str(path), 'rb') as w:
        if w.getframerate() != SR or w.getsampwidth() != 2: sys.exit(f'{path}: need 48 kHz 16-bit PCM (what dlcmaker check writes)')
        ch = w.getnchannels(); x = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(np.float32) / 32768.0
    x = x.reshape(-1, ch)
    return x[:, 0].copy(), x[:, -1].copy()

def frames(x, nfft):
    pad = nfft // 2; xp = np.concatenate([np.zeros(pad, np.float32), x, np.zeros(pad, np.float32)])
    return xp, 1 + (len(xp) - nfft) // HOP

def stft(x, nfft, maxbin):
    """Complex STFT, 5 ms hop, bins below maxbin, computed in chunks."""
    xp, n = frames(x, nfft); win = np.hanning(nfft).astype(np.float32); out = np.empty((n, maxbin), np.complex64)
    for i in range(0, n, 1000):
        j = min(n, i + 1000); idx = np.arange(i, j)[:, None] * HOP + np.arange(nfft)[None, :]
        out[i:j] = np.fft.rfft(xp[idx] * win, axis=1)[:, :maxbin]
    return out

def medfilt(M, k, axis):
    pad = k // 2; out = np.empty_like(M)
    Mp = np.pad(M, ((pad, pad), (0, 0)) if axis == 0 else ((0, 0), (pad, pad)), mode='edge')
    for i in range(0, M.shape[0], 400):
        j = min(M.shape[0], i + 400)
        v = Mp[i:j + 2 * pad] if axis == 0 else Mp[i:j]
        out[i:j] = np.median(np.lib.stride_tricks.sliding_window_view(v, k, axis=axis), axis=-1)
    return out

def logfreq(freqs, fmin, fmax, nbins):
    edges = np.geomspace(fmin, fmax, nbins + 1); M = np.zeros((nbins, len(freqs)), np.float32)
    for b in range(nbins):
        sel = (freqs >= edges[b]) & (freqs < edges[b + 1])
        if sel.any(): M[b, sel] = 1.0 / sel.sum()
        else: M[b, min(np.searchsorted(freqs, np.sqrt(edges[b] * edges[b + 1])), len(freqs) - 1)] = 1.0
    return M, edges

def flux(X, lag=2):
    return np.concatenate([np.zeros(lag, np.float32), np.maximum(X[lag:] - X[:-lag], 0).sum(axis=1)])

def norm(v):
    v = v - np.percentile(v, 5); return np.clip(v / (np.percentile(v, 99.5) + 1e-9), 0, 1.5).astype(np.float32)

def halve(M):
    """10 ms frames, float16, for the sheets."""
    n = len(M) // 2 * 2; return (0.5 * (M[0:n:2] + M[1:n:2])).astype(np.float16)

def pitch_track(Ch, f4, lo=52, hi=84, thr=2.2):
    """Melody pitch (MIDI, NaN where none stands out) and its contrast, from harmonic salience."""
    midis = np.arange(45, 88.01, 0.1); binw = SR / 4096; nb = Ch.shape[1]
    A = np.log1p(200 * Ch / (np.percentile(Ch, 99.9) + 1e-9)); sal = np.zeros((len(A), len(midis)), np.float32)
    for i, m in enumerate(midis):
        f0 = 440 * 2 ** ((m - 69) / 12)
        for h in range(1, 9):
            fb = f0 * h / binw
            if fb >= nb - 2: break
            k = int(fb); a = fb - k; sal[:, i] += 0.8 ** (h - 1) * ((1 - a) * A[:, k] + a * A[:, k + 1])
    sel = np.where((midis >= lo) & (midis <= hi))[0]; S = sal[:, sel]
    med = np.median(sal[:, (midis >= 50) & (midis <= 86)], axis=1) + 1e-9
    i = S.argmax(axis=1); pk = S[np.arange(len(S)), i]; p = midis[sel][i].copy()
    for k in range(len(p)):  # an octave below that is nearly as strong is the real note
        if p[k] - 12 >= lo:
            j = int(round((p[k] - 12 - midis[0]) / 0.1)); a, b = max(j - 3, 0), j + 4
            if sal[k, a:b].max() >= 0.85 * pk[k]: p[k] = midis[a + np.argmax(sal[k, a:b])]
    con = pk / med; pt = np.where(con > thr, p, np.nan); out = pt.copy()
    for k in range(3, len(pt) - 3):
        w = pt[k - 3:k + 4]; w = w[~np.isnan(w)]
        if len(w) >= 4 and not np.isnan(pt[k]): out[k] = np.median(w)
    return out.astype(np.float32), con.astype(np.float32)

def features(song, wav=None, lo=52, hi=84):
    L, R = read_wav(wav or song_wav(song)); M = 0.5 * (L + R); dur = len(M) / SR
    print(f'{dur:.1f} s of audio; this takes a minute or two')
    S = np.abs(stft(M, 2048, 700)); f2 = np.fft.rfftfreq(2048, 1 / SR)[:700]
    LF, spec_edges = logfreq(f2, 30, 16000, 300); spec = 20 * np.log10(S @ LF.T + 1e-6)
    lg = np.log1p(1000 * S / (S.max() + 1e-9))
    on_all = flux(lg); on_hat = flux(lg[:, (f2 >= 7000) & (f2 < 16000)])
    P = medfilt(S, 21, axis=1); H = medfilt(S, 21, axis=0)
    Sp = S * (P ** 2 / (P ** 2 + H ** 2 + 1e-12)); del P, H
    lgp = np.log1p(1000 * Sp / (Sp.max() + 1e-9)); band = lambda lo, hi: flux(lgp[:, (f2 >= lo) & (f2 < hi)])
    on_kick = band(35, 120); on_snare = 0.5 * band(150, 350) + 0.5 * band(1500, 6000); del Sp, lgp, lg
    rms = 10 * np.log10((S[:, :] ** 2).mean(axis=1) + 1e-12); del S
    # The melody: what is the same in both channels (vocals, leads and bass are centre-panned, wide
    # guitars and rooms are not), then its harmonic part.
    nb = int(5000 / (SR / 4096)) + 1; CL = stft(L, 4096, nb); CR = stft(R, 4096, nb)
    coh = np.clip(2 * np.real(CL * np.conj(CR)) / (np.abs(CL) ** 2 + np.abs(CR) ** 2 + 1e-12), 0, 1)
    C = (np.abs(0.5 * (CL + CR)) * coh ** 4).astype(np.float32); del CL, CR, coh
    f4 = np.fft.rfftfreq(4096, 1 / SR)[:nb]
    Hc = medfilt(C, 31, axis=0); Pc = medfilt(C, 15, axis=1); Ch = C * (Hc ** 2 / (Hc ** 2 + Pc ** 2 + 1e-12)); del C, Hc, Pc
    LF2, cen_edges = logfreq(f4, 80, 5000, 240); cen = 20 * np.log10(Ch @ LF2.T + 1e-6)
    on_mel = flux(np.log1p(1000 * Ch[:, (f4 > 250) & (f4 < 3000)] / (Ch.max() + 1e-9)))
    pitch, pcon = pitch_track(Ch, f4, lo, hi)
    n = min(len(on_all), len(pitch))
    np.savez_compressed(work(song) / 'feat.npz', fps=FPS, dur=dur, rms_db=rms[:n].astype(np.float32),
                        on_all=norm(on_all)[:n], on_kick=norm(on_kick)[:n], on_snare=norm(on_snare)[:n],
                        on_hat=norm(on_hat)[:n], on_melody=norm(on_mel)[:n], pitch=pitch[:n], pcon=pcon[:n],
                        spec=halve(spec[:n]), spec_edges=spec_edges, cen=halve(cen[:n]), cen_edges=cen_edges)
    print(f'wrote {work(song) / "feat.npz"} ({n} frames)')

def load(song):
    p = Path(song) / 'chart-work' / 'feat.npz'
    if not p.is_file(): sys.exit(f'{p} is missing: run `python analyze.py features {song}` first')
    return dict(np.load(p))

def envelope(F):
    return F['on_all'] + 0.7 * F['on_kick'] + 0.7 * F['on_snare'] + 0.5 * F['on_melody']

def tempo_candidates(env, lo, hi, dur):
    """BPMs by the Fourier magnitude of the onset curve at 100 fps, with the 8th and 16th harmonics."""
    e = np.maximum(env[0:len(env) // 2 * 2:2], env[1:len(env) // 2 * 2:2]); t = np.arange(len(e)) / 100.0
    sel = (t > 1.0) & (t < dur - 1.0); t, e = t[sel], e[sel] - e[sel].mean()
    bpms = np.arange(lo, hi, 0.05); score = np.zeros(len(bpms))
    for i in range(0, len(bpms), 50):
        f = bpms[i:i + 50, None] / 60.0
        for h, w in ((1, 1.0), (2, 0.6), (4, 0.3)): score[i:i + 50] += w * np.abs(np.exp(-2j * np.pi * h * f * t) @ e)
    out = []
    for i in np.argsort(-score):
        if all(abs(bpms[i] - b) > 1.5 for b, _ in out): out.append((float(bpms[i]), float(score[i])))
        if len(out) == 6: break
    return out

def track(env, bpm, t0, t1, tight=400.0):
    """Ellis's dynamic-programming beat tracker: beats on strong onsets, close to the period."""
    period = 60.0 / bpm * FPS; a, b = int(t0 * FPS), int(t1 * FPS)
    e = env[a:b] / (env[a:b].std() + 1e-9); n = len(e); score = e.copy(); back = -np.ones(n, int)
    lo, hi = int(round(period * 0.8)), int(round(period * 1.25))
    for i in range(lo, n):
        prev = np.arange(max(0, i - hi), i - lo + 1)
        c = score[prev] - tight * np.log((i - prev) / period) ** 2; j = c.argmax(); score[i] = e[i] + c[j]; back[i] = prev[j]
    i = int(np.argmax(score[-hi:]) + n - hi); beats = []
    while i >= 0: beats.append(i); i = back[i]
    return (np.array(beats[::-1]) + a) / FPS

def timing(song, bpm=None, lo=70.0, hi=200.0, downbeat=None, bar_beats=4):
    F = load(song); dur = float(F['dur'])
    env = envelope(F); env = np.maximum(env - np.convolve(env, np.ones(80) / 80, mode='same'), 0)
    cands = tempo_candidates(env, lo, hi, dur)
    print('tempo candidates (half or double of the right one often scores too):')
    for b, s in cands: print(f'  {b:7.2f} BPM  score {s:9.1f}')
    bt = track(env, bpm or cands[0][0], 0.5, dur - 0.5); k = np.arange(len(bt)); A = np.vstack([k, np.ones_like(k)]).T
    (per, off), *_ = np.linalg.lstsq(A, bt, rcond=None); res = bt - (k * per + off)
    keep = np.abs(res) <= np.percentile(np.abs(res), 85); (per, off), *_ = np.linalg.lstsq(A[keep], bt[keep], rcond=None)
    res = bt - (k * per + off)
    print(f'\nbeat track: {len(bt)} beats, line fit {60 / per:.3f} BPM, median |residual| {np.median(np.abs(res)) * 1000:.1f} ms')
    # Strong onsets near the 8th grid, regressed onto it: the final tempo and phase.
    on = F['on_all'] + 0.5 * F['on_kick']
    pk = np.array([i for i in range(3, len(on) - 3) if on[i] == on[i - 3:i + 4].max() and on[i] > 0.35]) / FPS
    q = per / 2; kk = np.round((pk - off) / q); m = np.abs(pk - (off + kk * q)) < 0.07
    if m.sum() > 20:
        p = np.polyfit(kk[m], pk[m], 1); r = pk[m] - np.polyval(p, kk[m]); good = np.abs(r) < np.percentile(np.abs(r), 80)
        p2 = np.polyfit(kk[m][good], pk[m][good], 1)
        if abs(60 / (2 * p2[0]) - 60 / per) < 0.1: per, off = 2 * p2[0], p2[1]
        print(f'onset regression: {m.sum()} onsets, {60 / (2 * p2[0]):.3f} BPM')
    res = bt - (off + np.round((bt - off) / per) * per)
    print('drift (median offset of the tracked beats from the grid per 20 s; a steady song stays within about 15 ms):')
    spread = []
    for w in range(0, int(dur), 20):
        s = (bt >= w) & (bt < w + 20)
        if s.sum() > 3: spread.append(np.median(res[s])); print(f'  {w:4d}-{w + 20:4d} s  {spread[-1] * 1000:+6.1f} ms  ({s.sum()} beats)')
    kick = F['on_kick'] + 0.5 * F['on_all']; grid = off + np.arange(int((dur - off) / per)) * per
    strength = []
    for ph in range(bar_beats):
        v = [kick[max(int(t * FPS) - 3, 0):int(t * FPS) + 6].max() for t in grid[ph::bar_beats] if t * FPS + 6 < len(kick)]
        strength.append(float(np.mean(v))); print(f'  beat phase {ph}: kick strength {strength[-1]:.3f}')
    ph = downbeat if downbeat is not None else int(np.argmax(strength))
    b0 = off + ph * per
    while b0 - bar_beats * per >= 0: b0 -= bar_beats * per
    beats_ms = [None] * (int((dur - b0) / per) + 2)  # the tracked beat nearest each grid beat
    for t in bt:
        j = int(round((t - b0) / per))
        if 0 <= j < len(beats_ms) and abs(t - (b0 + j * per)) < per / 3: beats_ms[j] = round(t * 1000, 1)
    known = [j for j, v in enumerate(beats_ms) if v is not None]
    beats_ms = [float(np.interp(j, known, [beats_ms[i] for i in known])) for j in range(len(beats_ms))]
    drifts = max(spread) - min(spread) > 0.03 if spread else False
    out = {'bpm': round(60 / per, 3), 'beat0_ms': round(b0 * 1000, 1), 'bar_beats': bar_beats, 'length_s': round(dur, 3),
           'use_tracked': False, 'beats_ms': [round(v, 1) for v in beats_ms]}
    (work(song) / 'timing.json').write_text(json.dumps(out) + '\n')
    print(f'\nwrote timing.json: {out["bpm"]} BPM, bar 0 at {out["beat0_ms"]} ms (beat phase {ph}), {bar_beats} beats a bar')
    if drifts: print('the tempo drifts by more than 30 ms: set "use_tracked": true in timing.json to chart on the tracked beats')
    print('check it with `map`: kicks on beat 1 and snares on 2 and 4 mean the grid and the bar are right')

class Grid:
    """Bar/beat positions to seconds, from timing.json."""
    def __init__(self, song):
        p = Path(song) / 'chart-work' / 'timing.json'
        if not p.is_file(): sys.exit(f'{p} is missing: run `python analyze.py timing {song}` first')
        c = json.loads(p.read_text()); self.bpm, self.bar_beats = c['bpm'], c.get('bar_beats', 4)
        self.beat0 = c['beat0_ms'] / 1000.0; self.beat = 60.0 / self.bpm
        self.tracked = np.array(c['beats_ms']) / 1000.0 if c.get('use_tracked') else None
    def at(self, beat):
        """Seconds of a beat index (float) from bar 0."""
        if self.tracked is not None and 0 <= beat <= len(self.tracked) - 1: return float(np.interp(beat, np.arange(len(self.tracked)), self.tracked))
        return self.beat0 + beat * self.beat
    def bar(self, b): return self.at(b * self.bar_beats)

def name(m): r = int(round(m)); return f'{NAMES[r % 12]}{r // 12 - 1}'

def melody_notes(F):
    """(start s, end s, MIDI) of each steady stretch of the pitch track."""
    pt = F['pitch']; segs = []; cur = None; gap = 0
    for k, v in enumerate(pt):
        if np.isnan(v):
            gap += 1
            if cur and gap > 6: segs.append(cur); cur = None
            continue
        gap = 0
        if cur is None or abs(v - np.median(cur[2][-12:])) > 0.8:
            if cur: segs.append(cur)
            cur = [k, k, [v]]
        else: cur[1] = k; cur[2].append(v)
    if cur: segs.append(cur)
    out = []
    for s, e, m in [(s / FPS, (e + 1) / FPS, float(np.median(v))) for s, e, v in segs if (e - s + 1) / FPS >= 0.06]:
        if out and round(out[-1][2]) == round(m) and s - out[-1][1] < 0.07: out[-1] = (out[-1][0], e, out[-1][2])
        else: out.append((s, e, m))  # a note split by vibrato or a short blip stays one note
    return out

def peaks(curve):
    """An onset curve's peaks above its local median, scaled so its strong hits are about 9."""
    med = medfilt(curve[:, None], 201, axis=0)[:, 0]; v = curve - med
    keep = [i for i in range(6, len(v) - 6) if v[i] > 0.08 and curve[i] == curve[i - 6:i + 7].max()]
    out = np.zeros(len(curve), np.float32)
    if keep: out[keep] = v[keep] / (np.percentile(v[keep], 95) + 1e-9)
    return out

def strength(pk, t, reach):
    i = int(round(t * FPS)); r = int(reach * FPS); return float(pk[max(i - r, 0):i + r + 1].max(initial=0))

def cell(v):
    d = int(min(v, 0.99) * 10); return '.' if d < 3 else str(d)

def bar_map(song, b0=None, b1=None, brief=False):
    F = load(song); G = Grid(song); dur = float(F['dur']); notes = melody_notes(F)
    last = int((dur - G.beat0) / (G.beat * G.bar_beats)); b0 = 0 if b0 is None else b0; b1 = last if b1 is None else min(b1, last)
    rows = {k: peaks(F['on_' + c]) for k, c in BANDS.items()}; reach = min(0.03, G.beat / 8)  # half a 32nd
    slots = 4 * G.bar_beats; ruler = ''.join(str(i + 1) + '...' for i in range(G.bar_beats))
    if brief: print('bar    time   loud dB  hits   low snare  melody (notes)')
    for b in range(b0, b1 + 1):
        ts = [G.at(b * G.bar_beats + s / 4) for s in range(slots)]; t0, t1 = ts[0], G.bar(b + 1)
        mel = [(s, e, m) for s, e, m in notes if t0 - 0.02 <= s < t1 - 0.02]
        if brief:
            loud = float(np.mean(F['rms_db'][int(t0 * FPS):int(t1 * FPS)] if t1 * FPS < len(F['rms_db']) else -99))
            hits, kicks, snares = (sum(strength(rows[k], t, reach) >= 0.4 for t in ts) for k in ('all', 'low', 'snare'))
            rng = f'{name(min(m for *_, m in mel))}-{name(max(m for *_, m in mel))} ({len(mel)})' if mel else '-'
            print(f'{b:3d}  {t0:6.2f}  {loud:7.1f}  {hits:4d}  {kicks:4d} {snares:5d}  {rng}')
            continue
        print(f'bar {b}  {t0:.3f}-{t1:.3f} s'); print(f'          {ruler}')
        for k in BANDS: print(f'  {k:7s} {"".join(cell(strength(rows[k], t, reach)) for t in ts)}')
        print('  notes   ' + '  '.join(f'{int(round((s - t0) / (G.beat / 4)))}:{name(m)}/{max(1, int(round((e - s) / (G.beat / 4))))}' for s, e, m in mel))

def sheet(song, b0, b1, out=None):
    from PIL import Image, ImageDraw
    F = load(song); G = Grid(song); px = 12; slots = 4 * G.bar_beats; per_row = 4
    def colour(M, floor):
        lo, hi = np.percentile(M, floor), np.percentile(M, 99.7); v = np.clip((M - lo) / (hi - lo + 1e-9), 0, 1)
        return np.stack([v ** 0.6 * 255, v ** 1.2 * 230, v ** 3 * 255], -1).astype(np.uint8)
    spec, cen = colour(F['spec'].astype(np.float32), 30), colour(F['cen'].astype(np.float32), 70)
    rows = [list(range(b, min(b + per_row, b1 + 1))) for b in range(b0, b1 + 1, per_row)]
    hs, hc, strip = 110, 130, 10; row_h = 16 + hs + hc + 3 * strip + 8; width = per_row * slots * px
    img = Image.new('RGB', (width, row_h * len(rows)), (16, 16, 20)); d = ImageDraw.Draw(img)
    for r, bars in enumerate(rows):
        y0 = r * row_h
        for c, b in enumerate(bars):
            x0 = c * slots * px; t0, t1 = G.bar(b), G.bar(b + 1)
            cols = np.clip((np.linspace(t0, t1, slots * px, endpoint=False) * 100).astype(int), 0, len(spec) - 1)
            for M, y, h in ((spec, y0 + 16, hs), (cen, y0 + 16 + hs, hc)):
                part = M[cols][:, ::-1]; rows_i = np.linspace(0, part.shape[1] - 1, h).astype(int)
                img.paste(Image.fromarray(part[:, rows_i].transpose(1, 0, 2).copy()), (x0, y))
            # the melody's pitch on the centre spectrogram
            edges = F['cen_edges']; fr = (np.linspace(t0, t1, slots * px, endpoint=False) * FPS).astype(int)
            for x, i in enumerate(fr):
                if i < len(F['pitch']) and not np.isnan(F['pitch'][i]):
                    f = 440 * 2 ** ((F['pitch'][i] - 69) / 12); j = np.log(f / edges[0]) / np.log(edges[-1] / edges[0])
                    if 0 <= j < 1: d.point((x0 + x, y0 + 16 + hs + int((1 - j) * hc)), fill=(255, 60, 60))
            for n, key in enumerate(('on_kick', 'on_snare', 'on_hat')):
                y = y0 + 16 + hs + hc + n * strip; curve = F[key]
                for x, i in enumerate(fr):
                    v = int(min(curve[i] if i < len(curve) else 0, 1) * 255); d.line([(x0 + x, y), (x0 + x, y + strip - 2)], fill=(v, v, 0 if n else v))
            for s in range(slots + 1):
                x = x0 + s * px; colr = (255, 255, 255) if s == 0 else (90, 90, 110) if s % 4 == 0 else None
                if colr: d.line([(x, y0 + 14), (x, y0 + row_h - 8)], fill=colr)
            d.text((x0 + 3, y0 + 2), f'bar {b}  {t0:.2f}s', fill=(230, 230, 230))
    out = Path(out) if out else work(song) / f'sheet_{b0}-{b1}.png'; img.save(out); print(f'wrote {out}')

def main(argv):
    if len(argv) < 2 or argv[0] not in ('features', 'timing', 'map', 'sheet'): sys.exit(__doc__)
    cmd, song, rest = argv[0], argv[1], argv[2:]
    def opt(key, kind=float, default=None):
        if key in rest: i = rest.index(key); v = rest[i + 1]; del rest[i:i + 2]; return kind(v)
        return default
    if cmd == 'features': features(song, opt('--wav', str), opt('--lo', default=52), opt('--hi', default=84))
    elif cmd == 'timing':
        timing(song, opt('--bpm'), opt('--lo', default=70.0), opt('--hi', default=200.0), opt('--downbeat', int), opt('--bar-beats', int, 4))
    elif cmd == 'map':
        brief = '--brief' in rest; rest = [a for a in rest if a != '--brief']; nums = [int(a) for a in rest]
        bar_map(song, nums[0] if nums else None, nums[1] if len(nums) > 1 else (nums[0] if nums else None), brief)
    else:
        out = opt('--out', str); sheet(song, int(rest[0]), int(rest[1]), out)

if __name__ == '__main__': main(sys.argv[1:])
