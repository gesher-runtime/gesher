"""CI-only structural audit of WebM cue positions in actual muxer output."""
import argparse
from pathlib import Path


def vint(data, pos, keep_marker=False):
    if pos >= len(data) or not data[pos]:
        raise ValueError('invalid EBML variable integer')
    mask, length = 0x80, 1
    while not data[pos] & mask:
        mask >>= 1
        length += 1
    if length > 8 or pos + length > len(data):
        raise ValueError('truncated EBML variable integer')
    value = data[pos] if keep_marker else data[pos] & (mask - 1)
    for byte in data[pos + 1:pos + length]:
        value = (value << 8) | byte
    return value, pos + length


def elements(data, start, end):
    while start < end:
        ident, pos = vint(data, start, True)
        size, pos = vint(data, pos)
        if pos + size > end:
            raise ValueError('element outside parent')
        yield ident, pos, pos + size, start
        start = pos + size
    if start != end:
        raise ValueError('trailing element bytes')


def integer(data, start, end):
    if not 1 <= end - start <= 8:
        raise ValueError('invalid integer width')
    return int.from_bytes(data[start:end], 'big')


def audit(path):
    data = path.read_bytes()
    top = list(elements(data, 0, len(data)))
    segments = [e for e in top if e[0] == 0x18538067]
    if len(segments) != 1:
        raise ValueError('expected one Segment')
    _, base, end, _ = segments[0]
    children = list(elements(data, base, end))
    clusters = {start - base for ident, _, _, start in children if ident == 0x1f43b675}
    cue_elements = [e for e in children if e[0] == 0x1c53bb6b]
    if not clusters or len(cue_elements) != 1:
        raise ValueError('missing clusters or unique Cues')
    points = []
    for ident, start, end, _ in elements(data, cue_elements[0][1], cue_elements[0][2]):
        if ident != 0xbb:
            continue
        time, positions = None, []
        for child, a, b, _ in elements(data, start, end):
            if child == 0xb3:
                time = integer(data, a, b)
            elif child == 0xb7:
                fields = {field: integer(data, x, y) for field, x, y, _ in elements(data, a, b)
                          if field in (0xf7, 0xf1)}
                positions.append(fields)
        if time is None or not positions:
            raise ValueError('incomplete CuePoint')
        for fields in positions:
            if fields.get(0xf7) != 1 or fields.get(0xf1) not in clusters:
                raise ValueError('cue does not address video track and real cluster')
        points.append(time)
    if not points or points != sorted(points):
        raise ValueError('missing or unordered cue times')
    print(f'webm cue trace: {path} clusters={len(clusters)} points={len(points)} times={points}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    for codec in ('vp8-vorbis', 'vp9-opus'):
        for order in ('interleaved', 'blocked', 'short-interleaved', 'short-blocked'):
            audit(args.root / f'{codec}-{order}.webm')
    print('webm cue verification: 0 failures')


if __name__ == '__main__':
    main()
