"""Split a one-frame log from the sc6vrr DLL into render passes (step 2).

Usage: python3 -I tests/frame_passes.py <frame-*.tsv>

A pass starts whenever the bound render targets or depth target change. For
each pass this prints the targets (named T1, T2, ... by first appearance), the
draws, clears, and dispatches in it, and which earlier render targets it reads
as textures. Those reads show how passes depend on each other.

DXMT's OMSetRenderTargets calls OMSetRenderTargetsAndUnorderedAccessViews
through the hooked vtable, so each binding appears twice; equal bindings do not
start a new pass.
"""
import collections
import re
import sys

FORMATS = {
    1: 'RGBA32_TYPELESS', 2: 'RGBA32F', 9: 'RGBA16_TYPELESS', 10: 'RGBA16F', 11: 'RGBA16_UNORM',
    16: 'RG32F', 19: 'R32G8X24_TYPELESS', 20: 'D32S8X24', 23: 'RGB10A2_TYPELESS', 24: 'RGB10A2_UNORM',
    26: 'R11G11B10F', 27: 'RGBA8_TYPELESS', 28: 'RGBA8_UNORM', 29: 'RGBA8_SRGB', 34: 'RG16F', 35: 'RG16_UNORM',
    39: 'R32_TYPELESS', 40: 'D32F', 41: 'R32F', 42: 'R32_UINT', 44: 'R24G8_TYPELESS', 45: 'D24S8',
    49: 'RG8_UNORM', 53: 'R16_TYPELESS', 54: 'R16F', 55: 'D16', 56: 'R16_UNORM', 57: 'R16_UINT', 61: 'R8_UNORM',
    65: 'A8_UNORM', 70: 'BC1_TYPELESS', 71: 'BC1_UNORM', 72: 'BC1_SRGB', 73: 'BC2_TYPELESS', 76: 'BC3_TYPELESS',
    77: 'BC3_UNORM', 78: 'BC3_SRGB', 79: 'BC4_TYPELESS', 80: 'BC4_UNORM', 82: 'BC5_TYPELESS', 83: 'BC5_UNORM',
    84: 'BC5_SNORM', 86: 'BGRA8_TYPELESS', 87: 'BGRA8_UNORM', 88: 'BGRX8_UNORM', 90: 'BGRA8_TYPELESS',
    91: 'BGRA8_SRGB', 92: 'BGRX8_TYPELESS',
}
TEXTURE = re.compile(r'tex2d:([0-9A-Fa-f]+):(\d+)x(\d+):format=(\d+):mips=(\d+):array=(\d+):samples=(\d+)')


class Names:
    """Short names for texture addresses, in order of first appearance."""

    def __init__(self):
        self.names, self.info = {}, {}

    def name(self, text):
        m = TEXTURE.search(text)
        if not m:
            return None
        addr = m.group(1)
        if addr not in self.names:
            self.names[addr] = f'T{len(self.names) + 1}'
            w, h, fmt, mips, array = (int(m.group(i)) for i in range(2, 7))
            extra = (f' mips={mips}' if mips > 1 else '') + (f' array={array}' if array > 1 else '')
            self.info[self.names[addr]] = f'{w}x{h} {FORMATS.get(fmt, fmt)}{extra}'
        return self.names[addr]


def main() -> None:
    names = Names()
    passes = []
    current = None
    written = set()
    for line in open(sys.argv[1]):
        if line.startswith('#'):
            continue
        f = line.rstrip('\n').split('\t')
        call = f[1]
        if call in ('OMSetRenderTargets', 'OMSetRenderTargetsAndUnorderedAccessViews'):
            targets = tuple(names.name(x) or 'none' for x in f[3:] if x.startswith('rt'))
            depth = next((names.name(x) or 'none' for x in f[3:] if x.startswith('depth=')), 'none')
            key = (targets, depth)
            if current is None or current['key'] != key:
                current = {'key': key, 'start': int(f[0]), 'draws': collections.Counter(), 'indices': 0,
                           'clears': [], 'dispatches': 0, 'reads': set(), 'ps': set(), 'viewport': None}
                passes.append(current)
            continue
        if current is None:
            continue
        if call.startswith('Draw'):
            current['draws'][call] += 1
            m = re.search(r'count=(\d+)', f[2])
            current['indices'] += int(m.group(1)) if m else 0
            ps = next((x[3:] for x in f if x.startswith('ps=')), 'none')
            current['ps'].add(ps)
            for t in current['key'][0] + (current['key'][1],):
                if t != 'none':
                    written.add(t)
        elif call in ('ClearRenderTargetView', 'ClearDepthStencilView'):
            current['clears'].append(names.name(f[2]) or 'none')
            written.add(names.name(f[2]) or 'none')
        elif call.startswith('Dispatch'):
            current['dispatches'] += 1
        elif call in ('PSSetShaderResources', 'CSSetShaderResources'):
            for x in f[4:]:
                t = names.name(x)
                if t and t in written:
                    current['reads'].add(t)
        elif call == 'RSSetViewports' and current['viewport'] is None:
            current['viewport'] = f[3].split('=', 1)[1].split(',depth')[0]

    shown = [p for p in passes if p['draws'] or p['clears'] or p['dispatches']]
    print(f'{len(shown)} passes with work (of {len(passes)} target bindings), '
          f'{sum(sum(p["draws"].values()) for p in shown)} draws, {sum(p["dispatches"] for p in shown)} dispatches')
    print()
    for i, p in enumerate(shown, 1):
        targets, depth = p['key']
        draws = sum(p['draws'].values())
        work = []
        if draws:
            work.append(f'{draws} draws, {p["indices"]} vertices or indices, {len(p["ps"])} pixel shaders')
        if p['clears']:
            work.append(f'clears {",".join(p["clears"])}')
        if p['dispatches']:
            work.append(f'{p["dispatches"]} dispatches')
        reads = f'  reads {",".join(sorted(p["reads"], key=lambda t: int(t[1:])))}' if p['reads'] else ''
        print(f'{i:3}. call {p["start"]:5}  targets {",".join(targets)}  depth {depth}  '
              f'viewport {p["viewport"] or "-"}')
        print(f'      {"; ".join(work)}{reads}')
    print()
    print('Textures:')
    for name, info in names.info.items():
        print(f'  {name:5} {info}')


if __name__ == '__main__':
    main()
