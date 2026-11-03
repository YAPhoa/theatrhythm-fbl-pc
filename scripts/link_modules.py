"""Link the eboot with the game's bundled system modules into one probe image.

Successor of link_libc.py for several modules (default: libc.prx, libSceFios2.prx, then the
Unity modules the eboot names in DT_NEEDED: PS4Util.prx and Il2CppUserAssemblies.prx, the game's
IL2CPP code). Every module is placed after the previous one; imports of
all modules share one import table, so the C runtime can override any
function (host contracts first) and otherwise binds the native export.

The other modules of the dump (the rest of sce_module and Media/Plugins) are linked too but
deferred: the probe maps and relocates them without starting them, and starts one when the game
asks for it (sceKernelLoadStartModule, or sceSysmoduleLoadModule for a game-shipped system module).
Their exports are listed by NID per module for sceKernelDlsym.

Exports are matched by (NID, library identity, module identity). Imports of
libSceLibcInternal are served by libc.prx's same-NID exports: the internal
library is not shipped with the game and exposes the same functions.
Each module and the eboot also carry what sceKernelGetModuleInfoForUnwind reports: the
addresses of .eh_frame_hdr and .eh_frame, the size of .eh_frame and the first segment.
Output: out/boot-linked.bin (format TFPROBE7) and out/link.json.
"""
import collections
import hashlib
import json
from pathlib import Path
import struct
from prepare import parse_self, read_self, span, unpack
from link_libc import encode_id

DEFAULT_MODULES = ('libc.prx', 'libSceFios2.prx', 'PS4Util.prx', 'Il2CppUserAssemblies.prx')
# Where a dump keeps its modules: system ones in sce_module, Unity's own in Media/Modules and
# native plugins in Media/Plugins.
MODULE_DIRS = ('sce_module', 'Media/Modules', 'Media/Plugins')
FS_LOAD = bytes.fromhex('64488b042500000000')  # mov rax, fs:[0]
MODULE_DEFERRED = 1  # module table flag: started on request, not at boot
EH_FRAME_HDR = 0x6474e550  # PT_GNU_EH_FRAME
INTRO_SKIP_MODULE = 'Il2CppUserAssemblies.prx'
INTRO_SKIP_MODULE_SHA256 = 'b8592928fb51f8061290fc23127ccd5199a9564ee4c3f1f5bff3e79e212a4be6'
INTRO_SKIP_CALL_OFFSET = 0x233f736
INTRO_SKIP_END_MOVIE_OFFSET = 0x233fa90
INTRO_SKIP_CALL = bytes.fromhex('e8f5f1e600')
INTRO_SKIP_END_MOVIE = bytes.fromhex('554889e54156')
SPLASH_SKIP_STARTUP_LOGO_ID_OFFSET = 0x17ed4c9
SPLASH_SKIP_MAIN_MENU_ID_OFFSET = 0x17ec94d
SPLASH_SKIP_STARTUP_LOGO_ID = bytes.fromhex('c7431091969800')
SPLASH_SKIP_MAIN_MENU_ID = bytes.fromhex('c7431084969800')


def unwind_info(m, base, size):
    """(eh_frame_hdr, eh_frame, eh_frame size, first segment, its size) of a module loaded at base,
    as shadPS4 module.cpp derives them: .eh_frame runs up to .eh_frame_hdr when it comes first,
    else to the end of the module (size). Zeros for the frame fields without PT_GNU_EH_FRAME."""
    first = next(p for p in m['ph'] if p['type'] in (1, 0x61000010))
    hdr = next((p for p in m['ph'] if p['type'] == EH_FRAME_HDR), None)
    if not hdr:
        return (0, 0, 0, base + first['vaddr'], first['memsz'])
    data = span(m['elf'], hdr['offset'], 8)
    if data[0] != 1 or data[1] not in (0x1b, 0x03):
        raise ValueError('unsupported .eh_frame_hdr encoding')
    value, = unpack('<i' if data[1] == 0x1b else '<I', data, 4)
    frame = hdr['vaddr'] + 4 + value if data[1] == 0x1b else value  # pcrel sdata4 or udata4
    frame_size = hdr['vaddr'] - frame if hdr['vaddr'] > frame else size - hdr['vaddr']
    return (base + hdr['vaddr'], base + frame, frame_size, base + first['vaddr'], first['memsz'])


def canonical(identity, fallback=None):
    """NID#library for an import identity (NID, (library, version), module)."""
    return f'{identity[0]}#{identity[1][0]}' if identity else fallback


def module(path):
    source = read_self(path)
    elf, header, ph, _, missing = parse_self(source)
    dp = next(p for p in ph if p['type'] == 2)
    dynamic = []
    for pos in range(dp['offset'], dp['offset'] + dp['filesz'], 16):
        tag, value = unpack('<QQ', elf, pos)
        if not tag:
            break
        dynamic.append((tag, value))
    tags = dict(dynamic)
    lp = next(p for p in ph if p['type'] == 0x61000000)
    blob = span(elf, lp['offset'], lp['filesz'])
    strings = span(blob, tags[0x61000035], tags[0x61000037])

    def string(offset):
        span(strings, offset, 1)
        return strings[offset:strings.index(0, offset)].decode('ascii')
    libraries, modules = {}, {}
    for tag, value in dynamic:
        if tag in (0x61000013, 0x61000015, 0x6100000d, 0x6100000f):
            table = libraries if tag in (0x61000013, 0x61000015) else modules
            key = encode_id(value >> 48)
            identity = (string(value & 0xffffffff), (value >> 32) & 65535)
            if key in table and table[key] != identity:
                raise ValueError('conflicting module/library IDs')
            table[key] = identity

    def identity(name):
        parts = name.split('#')
        if len(parts) != 3:
            return None  # plain names such as module_start are not linkable
        n, lib, mod = parts
        return (n, libraries[lib], modules[mod])
    symbols = []
    table = span(blob, tags[0x61000039], tags[0x6100003f])
    for pos in range(0, len(table), 24):
        n, info, other, section, value, size = unpack('<IBBHQQ', table, pos)
        name = string(n)
        symbols.append(dict(name=name, type=info & 15, binding=info >> 4, section=section,
                            value=value, size=size, identity=identity(name) if name else None))
    relocations = []
    for ot, st in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        table = span(blob, tags[ot], tags[st])
        for pos in range(0, len(table), 24):
            target, info, addend = unpack('<QQq', table, pos)
            relocations.append((target, info & 0xffffffff, info >> 32, addend))
    return dict(elf=elf, header=header, ph=ph, tags=tags, symbols=symbols, relocs=relocations,
                identity=identity, libraries=libraries, modules=modules,
                sha256=hashlib.sha256(source).hexdigest(), missing=missing)


def find_module(game, filename):
    for folder in MODULE_DIRS:
        path = game / folder / filename
        if path.is_file():
            return path
    raise FileNotFoundError(f'{filename} not found in {", ".join(MODULE_DIRS)} of {game}')


def deferred_modules(game, boot=DEFAULT_MODULES):
    """Every other module the dump ships, in a stable order: sce_module, then Media/Plugins."""
    found = []
    for folder in ('sce_module', 'Media/Plugins'):
        directory = game / folder
        if directory.is_dir():
            found += sorted(p.name for p in directory.iterdir() if p.suffix == '.prx' and p.name not in boot)
    return found


def patch_fs_loads(image, ph, base):
    """Rewrite initial-exec `mov rax, fs:[0]` to GS: glibc owns FS on Linux."""
    patched = 0
    for p in ph:
        if p['type'] != 1 or not p['flags'] & 1:
            continue
        start, end = base + p['vaddr'], base + p['vaddr'] + p['filesz']
        at = image.find(FS_LOAD, start, end)
        while at >= 0:
            image[at] = 0x65
            patched += 1
            at = image.find(FS_LOAD, at + len(FS_LOAD), end)
    return patched


def patch_intro_skip(image, base, m):
    """Replace OpeningMain's movie playback call with its normal end-of-movie transition."""
    if m['sha256'] != INTRO_SKIP_MODULE_SHA256:
        raise ValueError('intro skip patch requires the supported game module revision')
    call = base + INTRO_SKIP_CALL_OFFSET
    end_movie = base + INTRO_SKIP_END_MOVIE_OFFSET
    if image[call:call + len(INTRO_SKIP_CALL)] != INTRO_SKIP_CALL:
        raise ValueError('intro skip patch call site did not match')
    if image[end_movie:end_movie + len(INTRO_SKIP_END_MOVIE)] != INTRO_SKIP_END_MOVIE:
        raise ValueError('intro skip patch target did not match')
    displacement = end_movie - (call + len(INTRO_SKIP_CALL))
    image[call:call + len(INTRO_SKIP_CALL)] = b'\xe8' + struct.pack('<i', displacement)


def patch_splash_skip(image, base, m):
    """Resolve the startup-logo scene to the main menu on the supported game revision."""
    if m['sha256'] != INTRO_SKIP_MODULE_SHA256:
        raise ValueError('splash skip patch requires the supported game module revision')
    startup_logo_id = base + SPLASH_SKIP_STARTUP_LOGO_ID_OFFSET
    main_menu_id = base + SPLASH_SKIP_MAIN_MENU_ID_OFFSET
    if image[startup_logo_id:startup_logo_id + len(SPLASH_SKIP_STARTUP_LOGO_ID)] != SPLASH_SKIP_STARTUP_LOGO_ID:
        raise ValueError('splash skip startup-logo ID did not match')
    if image[main_menu_id:main_menu_id + len(SPLASH_SKIP_MAIN_MENU_ID)] != SPLASH_SKIP_MAIN_MENU_ID:
        raise ValueError('splash skip main-menu ID did not match')
    image[startup_logo_id + 3:startup_logo_id + 7] = SPLASH_SKIP_MAIN_MENU_ID[3:7]


def link(game, out, module_names=DEFAULT_MODULES, deferred=None, skip_splash_logo=True):
    main = module(game / 'eboot.bin')
    raw = (out / 'boot.bin').read_bytes()
    magic, size, entry, ns, nr, ni, flags = unpack('<8s6Q', raw, 0)
    if magic != b'TFPROBE2':
        raise ValueError('linker expects freshly prepared TFPROBE2')
    pos = 56
    segments = [unpack('<3Q', raw, pos + i * 24) for i in range(ns)]
    pos += ns * 24
    names = [span(raw, pos + i * 128, 128).split(b'\0', 1)[0].decode() for i in range(ni)]
    pos += ni * 128
    relocs = [unpack('<QQqq', raw, pos + i * 32) for i in range(nr)]
    pos += nr * 32
    image = bytearray(span(raw, pos, size))
    if pos + size != len(raw):
        raise ValueError('unexpected boot trailer')

    identities = {main['identity'](name): i for i, name in enumerate(names)}
    # Host tables key imports as NID#library: the eboot's own letters (#c#C) differ per game.
    names = [canonical(main['identity'](name), name) for name in names]

    def imported(symbol):
        key = symbol['identity']
        if key not in identities:
            identities[key] = len(names)
            names.append(canonical(key))
        return identities[key]

    exports, by_nid = {}, collections.defaultdict(list)
    table = []
    base = (size + 65535) & ~65535
    fs_patched = patch_fs_loads(image, main['ph'], 0)
    tls_module = 2
    intro_skip_patched = False
    splash_skip_patched = False
    if deferred is None:
        deferred = deferred_modules(game, module_names)
    module_exports = []
    for filename, flags in [(n, 0) for n in module_names] + [(n, MODULE_DEFERRED) for n in deferred]:
        if len(filename.encode()) >= 64:
            raise ValueError(f'{filename}: module name too long')
        m = module(find_module(game, filename))
        loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
        modsize = max(p['vaddr'] + p['memsz'] for p in loads)
        if base + modsize > 512 * 1024 * 1024:
            raise ValueError('linked image exceeds probe limit')
        image.extend(bytes(base + modsize - len(image)))

        def mapped(address, length=8, loads=loads):
            return any(p['vaddr'] <= address and address + length <= p['vaddr'] + p['memsz'] for p in loads)
        for p in loads:
            if p['filesz'] > p['memsz']:
                raise ValueError(f'{filename}: invalid segment size')
            image[base + p['vaddr']:base + p['vaddr'] + p['filesz']] = span(m['elf'], p['offset'], p['filesz'])
            segments.append((base + p['vaddr'], p['memsz'], p['flags']))
        tls = next((p for p in m['ph'] if p['type'] == 7), None)
        module_id = 0
        if tls:
            if tls['filesz'] > tls['memsz'] or not mapped(tls['vaddr'], tls['memsz']):
                raise ValueError(f'{filename}: unsupported TLS layout')
            module_id = tls_module
            tls_module += 1
        tls_relocations = 0
        for target, kind, symid, addend in m['relocs']:
            if not mapped(target):
                raise ValueError(f'{filename}: relocation outside image {target:#x}')
            target += base
            if kind == 8:
                relocs.append((target, 0, base + addend, 0))
            elif kind == 16:
                symbol = m['symbols'][symid]
                if symbol['name'] or symbol['type'] != 3 or addend or not module_id:
                    raise ValueError(f'{filename}: unsupported external TLS relocation')
                struct.pack_into('<Q', image, target, module_id)
                tls_relocations += 1
            elif kind in (1, 6, 7):
                symbol = m['symbols'][symid]
                if symbol['section']:
                    if symbol['section'] == 0xfff1:
                        struct.pack_into('<Q', image, target, symbol['value'] + addend)
                    else:
                        relocs.append((target, 0, base + symbol['value'] + addend, 0))
                elif not symbol['identity'] and symbol['binding'] == 2:
                    struct.pack_into('<Q', image, target, 0)  # weak undefined (module_start/stop)
                else:
                    rk = 2 if symbol['type'] == 1 else 1
                    if (rk == 1 and addend) or not 0 <= addend < 4096:
                        raise ValueError(f'{filename}: unsupported import addend')
                    relocs.append((target, rk, imported(symbol), addend))
            else:
                raise ValueError(f'{filename}: unsupported relocation type {kind}')
        if filename == INTRO_SKIP_MODULE:
            patch_intro_skip(image, base, m)
            intro_skip_patched = True
            if skip_splash_logo:
                patch_splash_skip(image, base, m)
                splash_skip_patched = True
        count = 0
        own_exports = []
        for s in m['symbols']:
            if s['section'] and s['binding'] in (1, 2) and s['identity'] and s['type'] in (1, 2):
                if not mapped(s['value'], max(1, s['size'])):
                    raise ValueError(f'{filename}: export outside load segments')
                if s['identity'] in exports:
                    raise ValueError(f'{filename}: ambiguous export')
                entry_value = (base + s['value'], 2 if s['type'] == 1 else 1, filename)
                exports[s['identity']] = entry_value
                by_nid[(s['identity'][0], s['identity'][1][0])].append(entry_value)
                own_exports.append((s['identity'][0], base + s['value'], entry_value[1]))
                count += 1
        fs_patched += patch_fs_loads(image, m['ph'], base)
        module_exports.append(own_exports)
        param = next((p for p in m['ph'] if p['type'] == 0x61000001), None)
        if param and not mapped(param['vaddr'], 0x18):
            raise ValueError(f'{filename}: procparam outside load segments')
        table.append(dict(file=filename, flags=flags, procparam=base + param['vaddr'] if param else 0,
                          base=base, size=modsize, init=base + m['tags'].get(12, 0),
                          tls_address=base + tls['vaddr'] if tls else 0, tls_memsz=tls['memsz'] if tls else 0,
                          tls_filesz=tls['filesz'] if tls else 0, tls_module=module_id,
                          tls_relocations=tls_relocations, exports=count, sha256=m['sha256'],
                          unwind=unwind_info(m, base, modsize)))
        base = (base + modsize + 65535) & ~65535

    bindings, unresolved = [], []
    for identity, index in identities.items():
        found = exports.get(identity)
        if not found and identity and identity[1][0] == 'libSceLibcInternal':
            candidates = by_nid.get((identity[0], 'libc'), [])
            found = candidates[0] if len(candidates) == 1 else None
        if found:
            bindings.append((index, found[0], found[1]))
        else:
            unresolved.append(names[index])

    main_tls = next((p for p in main['ph'] if p['type'] == 7), None)
    main_tls_values = (main_tls['vaddr'], main_tls['filesz'], main_tls['memsz'], main_tls['align']) if main_tls else (0, 0, 0, 0)
    if main_tls and (main_tls['filesz'] > main_tls['memsz'] or main_tls['memsz'] > 1024 * 1024
                     or main_tls['vaddr'] + main_tls['filesz'] > size):
        raise ValueError('unsupported eboot TLS layout')
    procparam = next(p for p in main['ph'] if p['type'] == 0x61000001)
    with (out / 'boot-linked.bin').open('wb') as f:
        f.write(struct.pack('<8s6Q', b'TFPROBE7', len(image), entry, len(segments), len(relocs), len(names), flags))
        f.write(struct.pack('<Q', procparam['vaddr']))
        f.write(struct.pack('<4Q', *main_tls_values))
        f.write(struct.pack('<Q', len(table)))
        for t in table:
            f.write(struct.pack('<9Q', t['base'], t['size'], t['init'], t['tls_address'], t['tls_memsz'],
                                t['tls_filesz'], t['tls_module'], t['flags'], t['procparam']))
            f.write(t['file'].encode().ljust(64, b'\0'))
        f.write(struct.pack('<Q', len(bindings)))
        for binding in bindings:
            f.write(struct.pack('<3Q', *binding))
        f.write(struct.pack('<Q', sum(map(len, module_exports))))
        for index, exported in enumerate(module_exports):
            for nid, address, kind in exported:
                if len(nid.encode()) >= 16:
                    raise ValueError('export NID too long')
                f.write(struct.pack('<Q', index) + nid.encode().ljust(16, b'\0') + struct.pack('<2Q', address, kind))
        f.write(struct.pack('<Q', len(table) + 1))  # unwind: the modules in table order, then the eboot
        for values in [t['unwind'] for t in table] + [unwind_info(main, 0, size)]:
            f.write(struct.pack('<5Q', *values))
        for segment in segments:
            f.write(struct.pack('<3Q', *segment))
        for name in names:
            if len(name.encode()) >= 128:
                raise ValueError('name too long')
            f.write(name.encode().ljust(128, b'\0'))
        for relocation in relocs:
            f.write(struct.pack('<QQqq', *relocation))
        f.write(image)
    report = dict(modules=[{k: (hex(v) if k in ('base', 'init', 'tls_address', 'procparam') else v)
                            for k, v in t.items() if k != 'unwind'} for t in table],
                  bindings=len(bindings), imports=len(names), fs_loads_patched=fs_patched,
                  intro_skip_patched=intro_skip_patched,
                  splash_skip_patched=splash_skip_patched,
                  main_tls=dict(zip(('vaddr', 'filesz', 'memsz', 'align'), main_tls_values)),
                  unresolved_imports=unresolved)
    (out / 'link.json').write_text(json.dumps(report, indent=2) + '\n')
    summary = ', '.join(f"{t['file']}@{t['base']:#x}" + (' (deferred)' if t['flags'] & MODULE_DEFERRED else '') for t in table)
    print(f'Linked modules: {summary}; {len(bindings)} native bindings, {len(unresolved)} imports left to the host runtime, '
          f'fs->gs patched={fs_patched}; intro skip patched={intro_skip_patched}; '
          f'splash skip patched={splash_skip_patched}')


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    p.add_argument('--modules', nargs='*', default=list(DEFAULT_MODULES))
    p.add_argument('--no-splash-skip', action='store_true',
                   help='keep the startup-logo sequence instead of redirecting to the main menu')
    a = p.parse_args()
    link(a.game, a.out, a.modules, skip_splash_logo=not a.no_splash_skip)
