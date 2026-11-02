#!/usr/bin/env python3
import argparse
import re
import struct
from pathlib import Path


def normalize(payload, root_libdir):
    path = Path(payload)
    data = bytearray(path.read_bytes())
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected a little-endian ELF64 payload')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phentsize, phnum = struct.unpack_from('<HH', data, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize) for i in range(phnum)]
    dynamic = next((segment for segment in segments if segment[0] == 2), None)
    if dynamic is None:
        raise ValueError('ELF has no PT_DYNAMIC')
    entries = [struct.unpack_from('<QQ', data, offset) for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16)]
    strtab = next((value for tag, value in entries if tag == 5), None)
    strsz = next((value for tag, value in entries if tag == 10), None)
    if strtab is None or strsz is None:
        raise ValueError('ELF has no dynamic string table')
    segment = next((item for item in segments if item[0] == 1 and item[3] <= strtab and strtab + strsz <= item[3] + item[5]), None)
    if segment is None:
        raise ValueError('ELF string table is outside PT_LOAD')
    start = segment[2] + strtab - segment[3]
    root_libdir = Path(root_libdir)
    replacements = []
    for tag, offset in entries:
        if tag != 1:
            continue
        pos = start + offset
        if offset >= strsz:
            raise ValueError('Invalid DT_NEEDED offset')
        end = data.find(b'\0', pos, start + strsz)
        if end < 0:
            raise ValueError('Unterminated DT_NEEDED')
        name = data[pos:end].decode('ascii')
        match = re.fullmatch(r'(lib[A-Za-z0-9_]+\.so)\.[0-9]+(?:\.[0-9]+)*', name)
        if match and (root_libdir / match.group(1)).is_file():
            generic = match.group(1).encode('ascii')
            data[pos:end] = generic + b'\0' * (end - pos - len(generic))
            replacements.append(f'{name} -> {match.group(1)}')
    if replacements:
        path.write_bytes(data)
    print('ROOT payload dependencies:', ', '.join(replacements) if replacements else 'already portable')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('payload')
    parser.add_argument('root_libdir')
    args = parser.parse_args()
    normalize(args.payload, args.root_libdir)
