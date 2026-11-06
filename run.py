#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Theatrhythm Final Bar Line on PC: prepare the game image and start the probe.

    python run.py            prepare if needed, then start the game
    python run.py check      prepare if needed, then list the PS4 imports the runtime lacks
    python run.py prepare    rebuild the game image only

Options: --game DIR (the decrypted dump; default THEATRHYTHM_GAME_DIR), --timeout SECONDS (play only),
--out DIR (the game image, default out/game), --dlc DIR (one more folder of DLC next to the save's
addcont, e.g. shadPS4's addcont; default THEATRHYTHM_DLC_DIR). Other arguments go to theatrhythm-fbl. Saves land in
user/, shader caches in the folder every copy of the game shares (shared_cache_dir), options in
user/settings.ini, or settings.ini here until that exists (see settings.example.ini).
"""
import argparse
import collections
import os
from pathlib import Path
import subprocess
import sys

WINDOWS = sys.platform == 'win32'
EXE_SUFFIX = '.exe' if WINDOWS else ''
PORT = Path(__file__).resolve().parent
# The eboot and the modules the linker starts at boot (scripts/link_modules.py, DEFAULT_MODULES);
# the deferred ones are every other .prx in IMAGE_MODULE_DIRS.
IMAGE_INPUTS = ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx',
                'Media/Modules/PS4Util.prx', 'Media/Modules/Il2CppUserAssemblies.prx')
IMAGE_MODULE_DIRS = ('sce_module', 'Media/Plugins')


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def parse_cpu_features(output):
    """Instruction sets `theatrhythm-fbl --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """THEATRHYTHM_ISA_REWRITE=1 forces the ISA rewrite table, 0 disables it; otherwise the CPU decides."""
    policy = env.get('THEATRHYTHM_ISA_REWRITE')
    if policy in ('0', '1'):
        return policy == '1'
    return bool(missing)


def shared_cache_dir(env=os.environ):
    """Shader and pipeline caches (and the GPU library's logs): one folder per PC, shared by run.py
    and set-up copies of the game (src/launch.c), whatever their save folder.
    THEATRHYTHM_GPU_USER_DIR overrides it."""
    if env.get('THEATRHYTHM_GPU_USER_DIR'):
        return Path(env['THEATRHYTHM_GPU_USER_DIR'])
    if WINDOWS:
        return Path(env.get('LOCALAPPDATA') or Path.home() / 'AppData' / 'Local') / 'Theatrhythm Final Bar Line'
    return Path(env.get('XDG_CACHE_HOME') or Path.home() / '.cache') / 'theatrhythm-fbl'


def run_script(name, *args, check=True):
    command = [sys.executable, str(PORT / 'scripts' / name), *map(str, args)]
    result = subprocess.run(command, stdin=subprocess.DEVNULL, env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})
    if check and result.returncode:
        sys.exit(result.returncode)
    return result.returncode


def image_is_current(game, out):
    linked = out / 'boot-linked.bin'
    if not linked.is_file() or not (out / 'content.bin').is_file():
        return False
    if not all((out / folder).is_dir() for folder in ('button_icons', 'config_icon', 'controller_pad')):
        return False
    built = linked.stat().st_mtime
    sources = [game / name for name in IMAGE_INPUTS] + list((PORT / 'scripts').glob('*.py'))
    sources += [path for folder in ('button_icons', 'config_icon', 'controller_pad') for path in (PORT / 'assets' / folder).glob('*.bc7')]
    sources += [path for folder in IMAGE_MODULE_DIRS for path in (game / folder).glob('*.prx')]
    return all(not path.is_file() or path.stat().st_mtime < built for path in sources)


def prepare(game, out, force=False):
    if not force and image_is_current(game, out):
        print(f'Game image is current: {out / "boot-linked.bin"}', flush=True)
        return
    out.mkdir(parents=True, exist_ok=True)
    run_script('prepare.py', game, '--out', out)
    run_script('link_libc.py', game, '--out', out)
    run_script('link_modules.py', game, '--out', out)
    run_script('content_profile.py', game, '--out', out, '--sku', 'full')
    run_script('button_icons.py', game, '--out', out)
    run_script('config_icon.py', game, '--out', out)
    run_script('controller_pad.py', game, '--out', out)


def find_probe():
    probe = Path(os.environ['THEATRHYTHM_EXE']) if os.environ.get('THEATRHYTHM_EXE') else PORT / 'out' / ('theatrhythm-fbl' + EXE_SUFFIX)
    if not probe.is_file():
        fail(f'{probe} not found: build it with build.sh (MSYS2 CLANG64 shell on Windows).')
    # Development builds use the MSYS2 CLANG64 DLLs (SDL3, FFmpeg, ...).
    if WINDOWS and not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            os.environ['PATH'] = f'{clang64}{os.pathsep}{os.environ.get("PATH", "")}'
    return probe


def check(probe, out):
    """--check-imports, summarized by library; the full list goes to out/missing-imports.txt."""
    result = subprocess.run([str(probe), str(out / 'boot-linked.bin'), '--check-imports'], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    missing = [line[len('MISSING: '):] for line in result.stdout.splitlines() if line.startswith('MISSING: ')]
    (out / 'missing-imports.txt').write_text(''.join(line + '\n' for line in missing), encoding='utf-8')
    by_library = collections.defaultdict(list)
    for line in missing:
        key, _, name = line.partition(' (')
        nid, _, library = key.partition('#')
        by_library[library].append(nid if name.startswith('name not resolved') else name.rstrip(')'))
    for library, names in sorted(by_library.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        shown = ', '.join(names[:6]) + (', ...' if len(names) > 6 else '')
        print(f'{len(names):4} {library:28} {shown}')
    print(f'{len(missing)} imports missing; full list: {out / "missing-imports.txt"}')
    return 1 if missing else 0


def play(probe, game, out, timeout, user, extra, dlc=None):
    env = os.environ
    features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True)
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        return 20
    isa_args = []
    if isa_rewrite_wanted(parse_cpu_features(features.stdout), env):
        if run_script('isa_rewrite.py', out / 'boot-linked.bin', '--out', out, check=False) == 0:
            isa_args = ['--isa-rewrite', str(out / 'isa-rewrite.bin')]
    # As the release's launcher: the save folder's settings.ini, else the one next to this script.
    env.setdefault('THEATRHYTHM_CONFIG', str(user / 'settings.ini'))
    env.setdefault('THEATRHYTHM_CONFIG_DEFAULT', str(PORT / 'settings.ini'))
    command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--app0', str(game), '--user', str(user), '--timeout', str(timeout), *isa_args,
               *(['--dlc', str(dlc)] if dlc else []), *extra]
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    try:
        return process.wait()
    except KeyboardInterrupt:
        process.terminate()
        return 130


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('command', nargs='?', choices=('play', 'check', 'prepare'), default='play')
    p.add_argument('--game', type=Path, default=os.environ.get('THEATRHYTHM_GAME_DIR'))
    p.add_argument('--timeout', type=int, default=int(os.environ.get('THEATRHYTHM_TIMEOUT', '0')))
    p.add_argument('--user', type=Path, default=PORT / 'user', help='saves, temp0 and data (default: user/)')
    p.add_argument('--out', type=Path, default=PORT / 'out' / 'game', help='the game image (default: out/game)')
    p.add_argument('--dlc', type=Path, default=os.environ.get('THEATRHYTHM_DLC_DIR') or None,
                   help="one more folder of DLC, next to the save's addcont (e.g. shadPS4's addcont)")
    a, extra = p.parse_known_args()
    if a.game is None:
        fail('Pass the decrypted game dump with --game DIR or set THEATRHYTHM_GAME_DIR.')
    game = a.game.resolve()
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (pass --game or set THEATRHYTHM_GAME_DIR).')
    out = a.out.resolve()
    prepare(game, out, force=a.command == 'prepare')
    if a.command == 'prepare':
        return 0
    probe = find_probe()
    # The GPU library's folder (shader caches, logs) reaches every probe run through the environment,
    # the --cpu-features one too: the shared cache, never the save folder, so test boots on a copy
    # of the save do not write to ./user.
    user = a.user.resolve()
    user.mkdir(parents=True, exist_ok=True)
    os.environ['THEATRHYTHM_GPU_USER_DIR'] = str(shared_cache_dir())
    # Development runs log every routine event; installed copies only faults, stops and errors.
    os.environ.setdefault('THEATRHYTHM_VERBOSE', '1')
    if a.command == 'check':
        return check(probe, out)
    return play(probe, game, out, a.timeout, user, extra, a.dlc.resolve() if a.dlc else None)


if __name__ == '__main__':
    sys.exit(main())
