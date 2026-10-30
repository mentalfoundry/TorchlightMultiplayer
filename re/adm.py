"""Decodes Torchlight's compiled .adm data files (Pak.zip) to the .DAT text form.

  python adm.py media/Spawnclasses/TREASURE.DAT.adm [more paths...]

Layout (little-endian): u32 version, u32 string count, then per string u32 id, u32 length, UTF-16
characters; then the tree: node = u32 name id, u32 property count, properties (u32 name id, u32 type,
value), u32 child count, children. Value types as seen: 1 int, 2 float, 3 double, 4 unsigned,
5 string id, 6 bool, 7 int64, 8 translated string id.
"""
import struct
import sys
import zipfile

PAK = r'D:\GAMES\Torchlight\Pak.zip'


class Reader:
    def __init__(self, b):
        self.b, self.o = b, 0

    def take(self, fmt):
        v = struct.unpack_from('<' + fmt, self.b, self.o)
        self.o += struct.calcsize('<' + fmt)
        return v[0] if len(v) == 1 else v


def decode(b):
    r = Reader(b)
    r.take('I')  # version
    strings = {}
    for _ in range(r.take('I')):
        sid, n = r.take('II')
        strings[sid] = b[r.o:r.o + 2 * n].decode('utf-16le')
        r.o += 2 * n
    out = []

    def node(depth):
        name = strings.get(r.take('I'), '?')
        out.append('  ' * depth + '[' + name + ']')
        for _ in range(r.take('I')):
            pname = strings.get(r.take('I'), '?')
            t = r.take('I')
            if t == 1:
                v, kind = r.take('i'), 'INTEGER'
            elif t == 2:
                v, kind = r.take('f'), 'FLOAT'
            elif t == 3:
                v, kind = r.take('d'), 'DOUBLE'
            elif t == 4:
                v, kind = r.take('I'), 'UNSIGNED INT'
            elif t in (5, 8):
                v, kind = strings.get(r.take('I'), '?'), 'STRING' if t == 5 else 'TRANSLATE'
            elif t == 6:
                v, kind = bool(r.take('I')), 'BOOL'
            elif t == 7:
                v, kind = r.take('q'), 'INTEGER64'
            else:
                raise ValueError('unknown value type %d at %d' % (t, r.o))
            out.append('  ' * (depth + 1) + '<%s>%s:%s' % (kind, pname, v))
        for _ in range(r.take('I')):
            node(depth + 1)
        out.append('  ' * depth + '[/' + name + ']')

    node(0)
    return '\n'.join(out)


if __name__ == '__main__':
    z = zipfile.ZipFile(PAK)
    for path in sys.argv[1:]:
        print('=== ' + path)
        print(decode(z.read(path)))
