#!/usr/bin/env python3
"""Extract main.dol from a GameCube disc image and verify it is the expected build.

Usage: extract_dol.py <image.iso> <out main.dol>
"""
import hashlib
import struct
import sys

EXPECTED_ID = b'GWRE01'
EXPECTED_SHA1 = 'd500d8f5bab39ae5fed67d0a0864095a9ea190d9'


def main():
    iso_path, out_path = sys.argv[1:3]
    with open(iso_path, 'rb') as f:
        hdr = f.read(0x440)
        if hdr[:6] != EXPECTED_ID:
            sys.exit(f'error: {iso_path} is not Wave Race: Blue Storm (USA) (disc ID {hdr[:6]!r}, expected {EXPECTED_ID!r}).\n'
                     'Note: only uncompressed .iso images are supported; convert .rvz/.gcz first (see README).')
        dol_off = struct.unpack('>I', hdr[0x420:0x424])[0]
        f.seek(dol_off)
        dh = f.read(0x100)
        offs = struct.unpack('>18I', dh[0x00:0x48])
        sizes = struct.unpack('>18I', dh[0x90:0xD8])
        dol_size = max(o + s for o, s in zip(offs, sizes) if s)
        f.seek(dol_off)
        dol = f.read(dol_size)
    sha1 = hashlib.sha1(dol).hexdigest()
    if sha1 != EXPECTED_SHA1:
        sys.exit(f'error: main.dol SHA-1 {sha1} does not match the supported build ({EXPECTED_SHA1}).')
    with open(out_path, 'wb') as f:
        f.write(dol)


if __name__ == '__main__':
    main()
