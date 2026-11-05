#!/usr/bin/env python3
import argparse
import re
import struct
from pathlib import Path

ROOT_LIBRARIES = frozenset({
    'libCling', 'libCore', 'libGpad', 'libGraf', 'libGraf3d', 'libHist', 'libImt', 'libMathCore',
    'libMathMore', 'libMatrix', 'libMultiProc', 'libNet', 'libPhysics', 'libPostscript', 'libRIO',
    'libRint', 'libThread', 'libTree', 'libTreePlayer', 'libROOTDataFrame', 'libROOTNTuple',
    'libROOTNTupleUtil', 'libROOTVecOps'
})
UNVERSIONED_SYSTEM_LIBRARIES = frozenset({'libgcc_s.so', 'libstdc++.so'})
VERSIONED_LIBRARY = re.compile(r'(lib[A-Za-z0-9_]+)\.so\.[0-9]+(?:\.[0-9]+)*')


def dependencies(data):
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected a little-endian ELF64 payload')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phentsize, phnum = struct.unpack_from('<HH', data, 54)
    if phentsize < 56 or not phnum or phoff + phentsize * phnum > len(data):
        raise ValueError('Invalid ELF program header table')
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize) for i in range(phnum)]
    dynamic = next((item for item in segments if item[0] == 2), None)
    if dynamic is None or dynamic[2] + dynamic[5] > len(data) or dynamic[5] % 16:
        raise ValueError('Invalid ELF PT_DYNAMIC')
    entries = [struct.unpack_from('<QQ', data, pos) for pos in range(dynamic[2], dynamic[2] + dynamic[5], 16)]
    strtab = next((value for tag, value in entries if tag == 5), None)
    strsz = next((value for tag, value in entries if tag == 10), None)
    if strtab is None or not strsz:
        raise ValueError('ELF has no dynamic string table')
    segment = next((item for item in segments if item[0] == 1 and item[3] <= strtab and strtab + strsz <= item[3] + item[5]), None)
    if segment is None:
        raise ValueError('ELF dynamic string table is outside PT_LOAD')
    start = segment[2] + strtab - segment[3]
    if start + strsz > len(data):
        raise ValueError('ELF dynamic string table is truncated')
    needed = []
    for tag, offset in entries:
        if tag != 1:
            continue
        if offset >= strsz:
            raise ValueError('Invalid DT_NEEDED offset')
        pos = start + offset
        end = data.find(b'\0', pos, start + strsz)
        if end < 0:
            raise ValueError('Unterminated DT_NEEDED')
        needed.append((data[pos:end].decode('ascii'), pos, end))
    return needed


def normalize(payload, root_libdir):
    path = Path(payload)
    data = bytearray(path.read_bytes())
    replacements = []
    for name, start, end in dependencies(data):
        if name in UNVERSIONED_SYSTEM_LIBRARIES:
            raise ValueError(f'Unsafe unversioned system dependency: {name}')
        match = VERSIONED_LIBRARY.fullmatch(name)
        if not match or match.group(1) not in ROOT_LIBRARIES:
            continue
        generic = match.group(1) + '.so'
        library = Path(root_libdir) / generic
        if not library.is_file():
            continue
        with library.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                raise ValueError(f'ROOT runtime library is not ELF: {library}')
        replacement = generic.encode('ascii')
        data[start:end] = replacement + b'\0' * (end - start - len(replacement))
        replacements.append(f'{name} -> {generic}')
    dependencies(data)
    if replacements:
        path.write_bytes(data)
    print('ROOT payload dependencies:', ', '.join(replacements) if replacements else 'unchanged')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('payload')
    parser.add_argument('root_libdir')
    args = parser.parse_args()
    normalize(args.payload, args.root_libdir)
