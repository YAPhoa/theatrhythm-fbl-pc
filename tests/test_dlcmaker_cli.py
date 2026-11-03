"""The DLC maker's command line (tools/dlcmaker/app/out/theatrhythm-dlcmaker): a scripted session of the
window's commands (new, new-dlc, new-song, set, add-file, info, check) on a generated project, compared
with what the Python DLC maker (the last commit before the C++ rewrite, 92fc050^) printed and wrote for
the same session: tests/data/dlcmaker_cli.json. With the game dump (THEATRHYTHM_GAME_DIR), the project's
pack is built and every file compared by SHA-256 with the Python DLC maker's build.

Needs the built program (bash tools/dlcmaker/app/build.sh) and an ffmpeg: THEATRHYTHM_FFMPEG, the
release's minimal one in out/dlcmaker-ffmpeg (tools/dlcmaker/build-ffmpeg.sh), or one on PATH.
"""
from paths import ROOT
import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

WINDOWS = os.name == 'nt'
EXE = ROOT / 'tools' / 'dlcmaker' / 'app' / 'out' / ('theatrhythm-dlcmaker.exe' if WINDOWS else 'theatrhythm-dlcmaker')
EXPECTED = ROOT / 'tests' / 'data' / 'dlcmaker_cli.json'
GAME = Path(os.environ['THEATRHYTHM_GAME_DIR']) if os.environ.get('THEATRHYTHM_GAME_DIR') else None
CMD = [str(EXE)]  # what runs the commands (the Python DLC maker's `python -m dlcmaker` made the expected data)


def find_ffmpeg():
    name = 'ffmpeg.exe' if WINDOWS else 'ffmpeg'
    for c in (os.environ.get('THEATRHYTHM_FFMPEG'), ROOT / 'out' / 'dlcmaker-ffmpeg' / name, shutil.which('ffmpeg')):
        if c and Path(c).is_file():
            return str(c)
    return None


FFMPEG = find_ffmpeg()


def png(width, height, pixel):
    """An RGBA PNG whose pixel (x, y) is pixel(x, y): the jacket and logo, made here so the test needs
    no picture files."""
    rows = b''.join(b'\0' + b''.join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def tone(path, seconds=24, rate=48000):
    """A 48 kHz stereo 16-bit WAV, 440 Hz left and 660 Hz right: ffmpeg passes it through unchanged."""
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(2), w.setsampwidth(2), w.setframerate(rate)
        w.writeframes(b''.join(struct.pack('<hh', int(6000 * math.sin(2 * math.pi * 440 * i / rate)),
                                           int(6000 * math.sin(2 * math.pi * 660 * i / rate))) for i in range(seconds * rate)))


def battle_charts():
    def notes(step, types):
        out = []
        for i, beat in enumerate(range(4, 44, step)):
            kind = types[i % len(types)]
            n = {'beat': beat, 'type': kind, 'lane': i % 4}
            if kind == 'slide':
                n['dir'] = 90 * (i % 4)
            if kind == 'hold':
                n['end_beat'] = beat + 1
            if kind == 'twin':
                n.update(dir=0, dir2=180)
            out.append(n)
        return out
    return {'bpm': 120, 'beat0_ms': 1500, 'levels': {'basic': 2, 'expert': 5, 'ultimate': 8},
            'difficulties': {'basic': notes(4, ['tap', 'hold']), 'expert': notes(2, ['tap', 'slide', 'hold']),
                             'ultimate': notes(1, ['tap', 'slide', 'hold', 'twin'])}}


def field_chart(level, step):
    return {'level': level, 'bpm': 120, 'offset_ms': 1500,
            'notes': [{'beat': b, 'type': 'tap', 'lane': (b * 3) % 9} for b in range(4, 40, step)]}


class Session:
    """Runs commands in a temp folder and keeps what they printed, the folder written as W and paths
    with / (so the transcript is the same on Windows and Linux)."""

    def __init__(self, root, ffmpeg):
        self.root = root
        self.env = {**os.environ, 'THEATRHYTHM_FFMPEG': ffmpeg, 'PYTHONIOENCODING': 'utf-8'}
        self.env.pop('THEATRHYTHM_GAME_DIR', None)
        self.log = []

    def clean(self, text):
        text = text.replace('\r', '')
        native = str(self.root)
        for form in (native.replace('\\', '\\\\'), native, self.root.as_posix()):
            text = text.replace(form, 'W')
        return text.replace('\\\\', '/').replace('\\', '/')

    def run(self, *args, record=True, usage=False):
        p = subprocess.run([*CMD, *map(str, args)], capture_output=True, env=self.env)
        out, err = p.stdout.decode('utf-8'), p.stderr.decode('utf-8')
        if usage:  # its wording is the program's own: only that it is the usage
            err = 'usage' if 'set FOLDER KEY=VALUE' in err else err
        if record:
            self.log.append({'args': [self.clean(str(a)) for a in args], 'code': p.returncode,
                             'out': self.clean(out), 'err': self.clean(err).strip()})
        return p.returncode, out, err

    def files(self):
        """The project files the commands wrote (JSON configs and charts), by W-relative path."""
        return {p.relative_to(self.root).as_posix(): p.read_bytes().decode('utf-8').replace('\r', '')
                for p in sorted(self.root.rglob('*.json')) if '.dlcmaker-cache' not in p.parts
                and p.relative_to(self.root).parts[0] in ('ws', 'Example')}


def session(root, ffmpeg):
    """The scripted session: a workspace with a buildable DLC (a battle and a field song with a tone,
    charts, a jacket and a logo, in a folder with a non-ASCII name) and a scratch DLC whose song takes
    every kind of edit, good and bad."""
    s = Session(root, ffmpeg)
    src = root / 'src'
    src.mkdir()
    tone(src / 'tone.wav')
    shutil.copy(ROOT / 'tests' / 'data' / 'sine.mp3', src / 'sine.mp3')
    (src / 'jacket.png').write_bytes(png(96, 96, lambda x, y: (x * 2, y * 2, 128, 255)))
    (src / 'logo.png').write_bytes(png(120, 48, lambda x, y: (255, 64, 32, 255 if 8 <= x < 112 and 8 <= y < 40 else 0)))
    (src / 'charts.json').write_text(json.dumps(battle_charts(), indent=1))
    for d, lv, step in (('basic', 3, 4), ('expert', 6, 2), ('ultimate', 9, 1)):
        (src / f'{d}.json').write_text(json.dumps(field_chart(lv, step), indent=1))
    (src / 'notes.json').write_text('{"bpm": 120,')
    ws = root / 'ws'
    ws.mkdir()
    pack, scratch = ws / 'Pack Ü 1', ws / 'Scratch'
    battle, field, edits = pack / 'Tone Battle', pack / 'Tone Field', scratch / 'Edits'

    s.run('new', root / 'Example')
    s.run('new', root / 'Example')
    s.run('new-dlc', ws, '  Pack Ü 1 ')
    s.run('new-dlc', ws, 'bad:name')
    s.run('new-dlc', pack, 'Inner')
    s.run('new-dlc', ws, 'Scratch')
    s.run('new-song', pack, 'Tone Battle')
    s.run('new-song', pack, 'Tone Field', 'field')
    s.run('new-song', pack, 'S', 'bogus')
    s.run('new-song', ws, 'S')
    s.run('new-song', scratch, 'Edits', 'battle')
    s.run('add-file', battle, 'audio', src / 'tone.wav')
    s.run('add-file', battle, 'chart', src / 'charts.json')
    s.run('add-file', battle, 'jacket', src / 'jacket.png')
    s.run('add-file', field, 'audio', src / 'tone.wav')
    s.run('add-file', field, 'chart', src / 'basic.json')
    s.run('add-file', field, 'chart', src / 'expert.json')
    s.run('add-file', field, 'chart', src / 'ultimate.json')
    s.run('add-file', pack, 'logo', src / 'logo.png')
    s.run('add-file', edits, 'audio', src / 'sine.mp3')
    s.run('add-file', edits, 'audio', src / 'jacket.png')
    s.run('add-file', edits, 'wat', src / 'tone.wav')
    s.run('add-file', edits, 'chart', src / 'notes.json')
    s.run('add-file', edits, 'jacket', src / 'missing.png')
    s.run('set', battle, 'fade_out=0', 'preview_start=2', 'preview_end=12', 'copyright=Test')
    s.run('set', field, 'fade_out=0', 'end=22.5')
    s.run('set', pack, 'series.name=Tone Series', 'label=ToneTest')
    s.run('set', edits, 'start=1.5', 'end= 90 ', 'fade_out=abc')
    s.run('set', edits, 'start=1.5', 'chart_offset_ms=12.0', 'preview_start=30', 'audio_from_video=TRUE',
          'stage_from=481', 'enemies=1_2', 'background=190000095')
    s.run('set', edits, 'stage=battle', 'enemy_waves=0:83,84|1:5', 'title=Ünï ☃')
    s.run('set', edits, 'audio_from_video=maybe')
    s.run('set', edits, 'enemy_waves=0:x')
    s.run('set', edits, 'stage=field', 'preview_end=45', 'enemies=x')
    s.run('set', edits, 'stage=field', 'preview_end=45')
    s.run('set', edits, 'preview_start=', 'preview_end=', 'end=', 'audio_from_video=')
    s.run('set', edits, 'nokey')
    s.run('set', usage=True)
    s.run('info', ws)
    s.run('check', ws)
    s.run('check', pack)
    s.run('info', pack)
    return s


@unittest.skipUnless(EXE.is_file(), 'needs the DLC maker (bash tools/dlcmaker/app/build.sh)')
@unittest.skipUnless(FFMPEG, 'needs ffmpeg (THEATRHYTHM_FFMPEG, out/dlcmaker-ffmpeg or PATH)')
class CommandLineTests(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name).resolve()
        cls.session = session(cls.root, FFMPEG)
        cls.files = cls.session.files()  # before the build test adds its copy
        cls.expected = json.loads(EXPECTED.read_text(encoding='utf-8'))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_commands_print_what_the_python_dlc_maker_printed(self):
        log = self.session.log
        self.assertEqual(len(log), len(self.expected['log']))
        for got, want in zip(log, self.expected['log']):
            with self.subTest(args=want['args']):
                self.assertEqual(got, want)

    def test_project_files_are_the_ones_the_python_dlc_maker_wrote(self):
        files = self.files
        self.assertEqual(sorted(files), sorted(self.expected['files']))
        for name, text in self.expected['files'].items():
            with self.subTest(file=name):
                self.assertEqual(json.loads(files[name]), json.loads(text))

    def test_decoded_audio_is_the_tone_unchanged(self):
        with wave.open(str(self.root / 'src' / 'tone.wav')) as w:
            want = w.readframes(w.getnframes())
        song = self.root / 'ws' / 'Pack Ü 1' / '.dlcmaker-cache' / 'Tone Battle' / 'song.wav'
        with wave.open(str(song)) as w:
            self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()), (2, 2, 48000))
            self.assertEqual(w.readframes(w.getnframes()), want)

    @unittest.skipUnless(GAME and (GAME / 'Media/StreamingAssets/aa/catalog.json').is_file(),
                         'needs the game dump (THEATRHYTHM_GAME_DIR)')
    def test_built_pack_is_the_python_dlc_makers(self):
        # A copy: the build writes dlc.lock.json, which the other tests do not expect.
        pack, out = self.root / 'build' / 'Pack Ü 1', self.root / 'out'
        shutil.copytree(self.root / 'ws' / 'Pack Ü 1', pack)
        code, text, err = self.session.run('build', pack, '--out', out, '--game', GAME, record=False)
        self.assertEqual(code, 0, text + err)
        got = {p.relative_to(out).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted(out.rglob('*')) if p.is_file()}
        self.assertEqual(got, self.expected['build'])


if __name__ == '__main__':
    unittest.main()
