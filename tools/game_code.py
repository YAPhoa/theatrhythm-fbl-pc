"""The game code the runner patches, in any game version (src/runtime_game_code.c finds it at boot).

usage: python tools/game_code.py check GAME_DIR [GAME_DIR ...]
       python tools/game_code.py make GAME_DIR METHOD|OFFSET [MOVED [BYTES]]

check: finds every GameSite of src/runtime_*.c in each dump the way the runtime does, and prints
its offset in Il2CppUserAssemblies.prx and the C# method there (tools/il2cpp_symbols.py). A site
that is not found is marked FAIL; the exit status is 1 when any is.

make: a pattern for the code at a method's entry (METHOD as in a GameSite, or a module OFFSET in
hex): its instructions, rip-relative displacements and rel32 branch targets as `??`. With BYTES
it covers at least that many bytes (an entry check); without, it grows until it is unique in the
code (a site found by its pattern alone), and one that never is prints its matches (a site with
several instances). MOVED: the bytes a hook moves out of the entry, marked by `|`. Needs capstone
(`pip install --target <scratch>/pylib capstone`, then PYTHONPATH=<scratch>/pylib).

A GameSite: {method, inside, instances, instance, {patterns}}. method: "Namespace.Type::Method",
nested types after `/` ("Game.ScenePartsMain/UIObjectGroup::GetUIObjectInfo"), `(N)` after it for
the overload with N parameters (overloads without code, generic ones, do not count), looked up in
global-metadata.dat and the module's Il2CppCodeGenModule tables; NULL: the site is found by its
pattern in the whole code, which must match `instances` times (the site is match `instance`, by
address). inside 0: the pattern is at the method's entry (16-byte aligned without a method);
inside 1: it is somewhere in the first 64 KiB of the method (there exactly once). The first
pattern that matches is the one used (one per game version where the code differs). Patterns: hex
bytes, `??` any byte, `|` the end of the bytes a hook moves (those are always fixed bytes).
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tools'))
from link_modules import find_module, module  # noqa: E402

SITE = re.compile(r'\{\s*(NULL|"[^"]*")\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*\{((?:\s*(?:"[^"]*"|[A-Z_]+)\s*,?)+)\}\s*\}')
INSIDE_RANGE = 0x10000


class Game:
    """A dump's Il2CppUserAssemblies.prx (flat, at its vaddrs) and global-metadata.dat."""

    def __init__(self, path):
        self.path = Path(path)
        m = module(find_module(self.path, 'Il2CppUserAssemblies.prx'))
        loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
        flat = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
        for p in loads:
            flat[p['vaddr']:p['vaddr'] + p['filesz']] = m['elf'][p['offset']:p['offset'] + p['filesz']]
        text = min(loads, key=lambda p: p['vaddr'])
        self.flat, self.text_end = bytes(flat), text['vaddr'] + text['filesz']
        self.pointer = {target: addend for target, kind, sym, addend in m['relocs'] if kind == 8}  # R_X86_64_RELATIVE
        self.by_value = {}
        for target, addend in self.pointer.items():
            self.by_value.setdefault(addend, []).append(target)
        d = self.meta = (self.path / 'Media/Metadata/global-metadata.dat').read_bytes()
        sanity, version = struct.unpack_from('<Ii', d, 0)
        if sanity != 0xFAB11BAF or version != 29:
            raise ValueError(f'{path}: unsupported metadata (version {version})')
        pairs = struct.unpack_from('<64I', d, 8)
        self.section = lambda i: (pairs[2 * i], pairs[2 * i + 1])  # noqa: E731
        at, size = self.section(19)
        self.types = [struct.unpack_from('<8I8I8H2I', d, at + i * 88) for i in range(size // 88)]
        at, size = self.section(20)
        self.images = [struct.unpack_from('<10I', d, at + i * 40) for i in range(size // 40)]
        at, size = self.section(15)
        self.nested = struct.unpack_from(f'<{size // 4}i', d, at)
        self.cgm = {}

    def string(self, index):
        start = self.section(2)[0] + index
        return self.meta[start:self.meta.index(0, start)].decode('utf8', 'replace')

    def method(self, name):
        """Module offset of a method's code, or an error text."""
        type_path, _, method = name.partition('::')
        outer, *inner = type_path.split('/')
        namespace, _, type_name = outer.rpartition('.')
        found = [i for i, t in enumerate(self.types) if self.string(t[0]) == type_name and self.string(t[1]) == namespace]
        for part in inner:
            found = [n for i in found for n in self.nested[self.types[i][12]:self.types[i][12] + self.types[i][20]]
                     if self.string(self.types[n][0]) == part]
        if len(found) != 1:
            return f'{len(found)} types {type_path}'
        method, _, parameters = method.partition('(')
        parameters = int(parameters.rstrip(')')) if parameters else None
        t = self.types[found[0]]
        at, _ = self.section(5)
        methods = [struct.unpack_from('<6I4H', self.meta, at + i * 32) for i in range(t[9], t[9] + t[16])]
        methods = [m for m in methods if self.string(m[0]) == method and parameters in (None, m[9])]
        image = next(i for i, im in enumerate(self.images) if im[2] <= found[0] < im[2] + im[3])
        table, count = self.code_gen_module(image)
        codes = []
        for m in methods:  # overloads without code of their own (generic methods) do not count
            rid = m[5] & 0xFFFFFF
            code = self.pointer.get(table + (rid - 1) * 8) if table and 0 < rid <= count else None
            if code is not None and code < self.text_end:
                codes.append(code)
        return codes[0] if len(codes) == 1 else f'{len(codes)} methods {method} with code'

    def code_gen_module(self, image):
        """(methodPointers, count) of an image's Il2CppCodeGenModule: the one whose moduleName points
        at the image's file name (the string may follow other data, not only a NUL)."""
        if image not in self.cgm:
            name = self.string(self.images[image][0]).encode() + b'\0'
            self.cgm[image] = (0, 0)
            start = 0
            while (at := self.flat.find(name, start)) >= 0:
                for c in self.by_value.get(at, []):
                    if c + 16 in self.pointer:
                        self.cgm[image] = (self.pointer[c + 16], struct.unpack_from('<I', self.flat, c + 8)[0])
                        break
                if self.cgm[image][0]:
                    break
                start = at + 1
        return self.cgm[image]

    def matches(self, pattern, start, end, aligned, limit=8):
        """Offsets in [start, end) where pattern matches (at most limit)."""
        data, mask, _ = parse(pattern)
        best, i = (0, 0), 0  # the longest run of fixed bytes is searched for
        while i < len(data):
            j = i
            while j < len(data) and mask[j]:
                j += 1
            if j - i > best[1] - best[0]:
                best = (i, j)
            i = j + 1
        out, p = [], self.flat.find(data[best[0]:best[1]], start + best[0], end)
        while p >= 0 and len(out) < limit:
            s = p - best[0]
            if s + len(data) <= end and (not aligned or s % 16 == 0) and \
                    all(not mask[k] or self.flat[s + k] == data[k] for k in range(len(data))):
                out.append(s)
            p = self.flat.find(data[best[0]:best[1]], p + 1, end)
        return out

    def find(self, method, inside, instances, instance, patterns):
        """(offset, pattern number) of a site, or an error text."""
        at = self.method(method) if method else None
        if isinstance(at, str):
            return at
        for k, pattern in enumerate(patterns):
            if at is not None and not inside:
                hits = self.matches(pattern, at, at + len(parse(pattern)[0]), False)
                if hits:
                    return at, k
                continue
            start, end = (at, min(at + INSIDE_RANGE, self.text_end)) if at is not None else (0, self.text_end)
            hits = self.matches(pattern, start, end, at is None and not inside)
            want = 1 if at is not None else instances
            if len(hits) == want:
                return hits[0 if at is not None else instance], k
            if hits:
                return f'{len(hits)} matches, {want} expected'
        return 'code differs' if at is not None else 'no match'


def parse(pattern):
    """(bytes, mask, moved length or None)."""
    data, mask, moved = bytearray(), bytearray(), None
    for token in pattern.split():
        if token == '|':
            moved = len(data)
        elif token == '??':
            data.append(0); mask.append(0)
        else:
            data.append(int(token, 16)); mask.append(1)
    return bytes(data), bytes(mask), moved


def sites():
    """Every GameSite of src/runtime_*.c: (file, method, inside, instances, instance, patterns)."""
    out = []
    for path in sorted((ROOT / 'src').glob('runtime_*.c')):
        source = path.read_text(encoding='utf-8')
        # string macros (#define NAME "..." "..."), and adjacent literals joined as C does
        macros = {m.group(1): ''.join(re.findall(r'"([^"]*)"', m.group(2)))
                  for m in re.finditer(r'#define (\w+) ((?:"[^"]*"\s*(?:\\\n\s*)?)+)', source)}
        for m in SITE.finditer(source):
            method, inside, instances, instance, patterns = m.groups()
            listed = [''.join(macros.get(word, '') if word else text
                              for text, word in re.findall(r'"([^"]*)"|(\w+)', item))
                      for item in patterns.split(',') if item.strip()]
            out.append((path.name, None if method == 'NULL' else method[1:-1], int(inside), int(instances),
                        int(instance), listed))
    return out


def check(games):
    import bisect
    from il2cpp_symbols import symbols
    failed = 0
    table = sites()
    for path in games:
        game = Game(path)
        names = symbols(game.path)
        starts = [a for a, _ in names]
        print(f'== {path} ({len(table)} sites)')
        for file, method, inside, instances, instance, patterns in table:
            found = game.find(method, inside, instances, instance, patterns)
            label = method or f'{patterns[0][:40]}... #{instance + 1} of {instances}'
            if isinstance(found, str):
                failed += 1
                print(f'FAIL {file} {label}: {found}')
                continue
            i = bisect.bisect_right(starts, found[0]) - 1
            there = names[i][1] + (f' +{found[0] - starts[i]:#x}' if found[0] != starts[i] else '')
            print(f'{found[0]:#9x} {file} {label} (pattern {found[1] + 1}) = {there}')
    return 1 if failed else 0


def make(path, site, moved=None, size=None):
    from capstone import CS_ARCH_X86, CS_GRP_CALL, CS_GRP_JUMP, CS_MODE_64, Cs, x86
    game = Game(path)
    offset = int(site, 16) if re.fullmatch(r'(0x)?[0-9a-fA-F]+', site) else game.method(site)
    if isinstance(offset, str):
        raise SystemExit(f'{site}: {offset}')
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    tokens, count, hits = [], 0, []
    for ins in md.disasm(game.flat[offset:offset + 1024], offset):
        data = [f'{b:02x}' for b in ins.bytes]
        if ins.disp_offset and any(op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP for op in ins.operands):
            data[ins.disp_offset:ins.disp_offset + 4] = ['??'] * 4
        if (ins.group(CS_GRP_JUMP) or ins.group(CS_GRP_CALL)) and ins.imm_size == 4:
            data[ins.imm_offset:ins.imm_offset + 4] = ['??'] * 4
        if moved is not None and count < moved < count + len(data):
            raise SystemExit(f'{offset:#x}: {moved} bytes end inside an instruction')
        if moved is not None and count < moved and '??' in data:
            raise SystemExit(f'{offset:#x}: the moved bytes are rip-relative')
        tokens += data
        count += len(data)
        if count == moved:
            tokens.append('|')
        if moved is not None and count < moved:
            continue
        pattern = ' '.join(tokens)
        if size is not None:
            if count >= size:
                break
            continue
        hits = game.matches(pattern, 0, game.text_end, offset % 16 == 0, 16)
        if count >= 12 and hits == [offset]:
            break
    print(f'{offset:#x} "{pattern}"' + (f'  (matches {[hex(h) for h in hits]})' if hits and hits != [offset] else ''))


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == 'check':
        sys.exit(check(sys.argv[2:]))
    if 4 <= len(sys.argv) <= 6 and sys.argv[1] == 'make':
        make(sys.argv[2], sys.argv[3], *(int(v) for v in sys.argv[4:]))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
