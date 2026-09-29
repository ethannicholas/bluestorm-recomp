"""DOL loader and dtk symbols.txt parser."""
import re
import struct


class Dol:
    def __init__(self, path):
        data = open(path, 'rb').read()
        self.data = data
        offs = struct.unpack('>18I', data[0x00:0x48])
        addrs = struct.unpack('>18I', data[0x48:0x90])
        sizes = struct.unpack('>18I', data[0x90:0xD8])
        self.bss_addr, self.bss_size, self.entry = struct.unpack('>3I', data[0xD8:0xE4])
        self.sections = []  # (addr, size, file_offset, is_text)
        for i in range(18):
            if sizes[i]:
                self.sections.append((addrs[i], sizes[i], offs[i], i < 7))

    def read32(self, addr):
        for a, s, o, _ in self.sections:
            if a <= addr < a + s:
                return struct.unpack_from('>I', self.data, o + addr - a)[0]
        raise KeyError(hex(addr))

    def in_text(self, addr):
        return any(t and a <= addr < a + s for a, s, _, t in self.sections)

    def text_sections(self):
        return [(a, s, o) for a, s, o, t in self.sections if t]


SYM_RE = re.compile(r'^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // (.*)$')


def load_symbols(path):
    """Returns list of dicts: name, section, addr, type, size."""
    out = []
    for line in open(path):
        m = SYM_RE.match(line.strip())
        if not m:
            continue
        name, sect, addr, rest = m.groups()
        attrs = {}
        for tok in rest.split():
            if ':' in tok:
                k, v = tok.split(':', 1)
                attrs[k] = v
        out.append(dict(name=name, section=sect, addr=int(addr, 16),
                        type=attrs.get('type'),
                        size=int(attrs['size'], 16) if 'size' in attrs else None))
    return out
