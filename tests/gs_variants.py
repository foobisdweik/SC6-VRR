"""Convert each vertex + geometry shader combination that SC6 drew (step 3).

Usage: python3 -I tests/gs_variants.py <capture dir> <airconv>

DXMT runs a geometry shader as a Metal mesh pipeline. It converts the vertex
shader together with the geometry shader, the input layout, the index buffer
format, and whether the topology is a strip (d3d11_shader.cpp). This script
does the same with the airconv command line options --ia-layout,
--index-format, and --strip-topology (added on the local dxmt-SC6 branch
sc6/airconv-cli-gs). The capture does not record the index buffer format, so
every variant uses 16-bit indices (assumed).

Output: <capture dir>/convert-airconv/gs-variants/<key>.{vs,gs}.metallib and
gs-variants.tsv. <key> is "<vs>_<gs>_<first 16 of layout>_<list|strip>".
"""
import csv
import pathlib
import struct
import subprocess
import sys

# DXGI_FORMAT -> MTLAttributeFormat raw values (printed from Metal's MTLVertexFormat).
ATTRIBUTE_FORMATS = {
    2: 31, 3: 39, 6: 30, 7: 38, 10: 27, 11: 21, 12: 15, 13: 24, 14: 18, 16: 29, 17: 37, 24: 41, 28: 9,
    30: 3, 31: 12, 32: 6, 34: 25, 35: 19, 36: 13, 37: 22, 38: 16, 41: 28, 42: 36, 43: 32, 49: 7, 87: 42,
}
STRIP_TOPOLOGIES = {3, 5}  # D3D11 line strip and triangle strip
INDEX_FORMAT_UINT16 = 1


def input_registers(dxbc: bytes) -> dict:
    """Maps (SEMANTIC, index) to the input register, from the ISGN or ISG1 chunk."""
    if dxbc[:4] != b'DXBC':
        return {}
    count = struct.unpack_from('<I', dxbc, 28)[0]
    result = {}
    for i in range(count):
        off = struct.unpack_from('<I', dxbc, 32 + 4 * i)[0]
        tag = dxbc[off:off + 4]
        if tag not in (b'ISGN', b'ISG1'):
            continue
        data = off + 8
        entries = struct.unpack_from('<I', dxbc, data)[0]
        size, first = (24, 0) if tag == b'ISGN' else (32, 4)
        for e in range(entries):
            base = data + 8 + e * size + first
            name_off, sem_index = struct.unpack_from('<II', dxbc, base)
            register = struct.unpack_from('<I', dxbc, base + 16)[0]
            name = dxbc[data + name_off:dxbc.index(b'\0', data + name_off)].decode('ascii').upper()
            result[(name, sem_index)] = register
    return result


def layout_lines(root: pathlib.Path, layout: str, vs: str) -> list:
    if layout in ('none', 'unknown'):
        return []
    regs = input_registers((root / 'dxbc' / f'{vs}.dxbc').read_bytes())
    lines = []
    for row in (root / 'layouts' / f'{layout}.txt').read_text().splitlines():
        f = row.split('\t')
        if len(f) != 7 or row.startswith('#') or f[0] == 'semantic':
            continue
        reg = regs.get((f[0].upper(), int(f[1])))
        if reg is None:
            continue  # the shader does not read this element
        fmt = ATTRIBUTE_FORMATS.get(int(f[2]))
        if fmt is None:
            raise ValueError(f'unmapped vertex format {f[2]}')
        lines.append(f'{reg} {f[3]} {f[4]} {fmt} {f[5]} {f[6]}')
    return lines


def main() -> None:
    root, airconv = pathlib.Path(sys.argv[1]), sys.argv[2]
    out = root / 'convert-airconv' / 'gs-variants'
    out.mkdir(parents=True, exist_ok=True)
    variants = set()
    for path in sorted(root.glob('bindings-*.tsv')):
        with path.open(newline='') as f:
            for row in csv.DictReader(f, delimiter='\t'):
                if row.get('gs') in (None, 'none', 'unknown') or row['vs'] in ('none', 'unknown'):
                    continue
                strip = 'strip' if int(row['topology']) in STRIP_TOPOLOGIES else 'list'
                variants.add((row['vs'], row['gs'], row['input_layout'], strip))
    results = []
    for vs, gs, layout, strip in sorted(variants):
        key = f'{vs}_{gs}_{layout[:16]}_{strip}'
        base = out / key
        log = base.with_suffix('.log').open('w')
        common = ['--strip-topology'] if strip == 'strip' else []
        ok = {}
        try:
            ia = base.with_suffix('.ia')
            ia.write_text('\n'.join(layout_lines(root, layout, vs)) + '\n')
            steps = {
                'vs': [airconv, str(root / 'dxbc' / f'{vs}.dxbc'), '--geometry-after-vertex',
                       str(root / 'dxbc' / f'{gs}.dxbc'), '--ia-layout', str(ia),
                       '--index-format', str(INDEX_FORMAT_UINT16), *common],
                'gs': [airconv, str(root / 'dxbc' / f'{gs}.dxbc'), '--vertex-before-geometry',
                       str(root / 'dxbc' / f'{vs}.dxbc'), *common],
            }
            for half, cmd in steps.items():
                air, lib = f'{base}.{half}.air', f'{base}.{half}.metallib'
                ok[half] = (subprocess.run(cmd + ['-o', air], stdout=log, stderr=log).returncode == 0 and
                            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', lib],
                                           stdout=log, stderr=log).returncode == 0)
        except (OSError, ValueError) as e:
            log.write(f'{e}\n')
        log.close()
        results.append((key, 'ok' if ok.get('vs') else 'fail', 'ok' if ok.get('gs') else 'fail'))
    with (root / 'convert-airconv' / 'gs-variants.tsv').open('w') as f:
        for r in results:
            f.write('\t'.join(r) + '\n')
    print(f'vertex+geometry variants: {len(results)}, vertex half linked {sum(r[1] == "ok" for r in results)}, '
          f'geometry half linked {sum(r[2] == "ok" for r in results)}')


if __name__ == '__main__':
    main()
