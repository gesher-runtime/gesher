#!/usr/bin/env python3
"""Independent integer/rational oracle for finite binary128 parsing.

No host float, Decimal context, gdtoa, or quad library supplies expected bits.
The production record executable receives decimal/hex strings through stdin.
Android API quirks (NaN grammar, errno and fenv) use the separate device receipt.
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import random
import subprocess
import time


def binary128(value: Fraction, mode: int) -> int:
    negative = value < 0
    value = abs(value)
    sign = int(negative) << 127
    if not value:
        return sign
    n, d = value.numerator, value.denominator
    exponent = n.bit_length() - d.bit_length()
    if (n < d << exponent) if exponent >= 0 else (n << -exponent < d):
        exponent -= 1
    unit = max(exponent - 112, -16494)
    scaled_n, scaled_d = (n, d << unit) if unit >= 0 else (n << -unit, d)
    significand, remainder = divmod(scaled_n, scaled_d)
    increment = (mode == 1 and (2 * remainder > scaled_d or
                               (2 * remainder == scaled_d and significand & 1)))
    increment |= bool(remainder) and ((mode == 2 and not negative) or (mode == 3 and negative))
    significand += increment
    if significand >= 1 << 113:
        significand >>= 1
        unit += 1
    if not significand:
        return sign
    if significand < 1 << 112:
        return sign | significand
    exponent = unit + 112
    if exponent > 16383:
        infinity = mode == 1 or (mode == 2 and not negative) or (mode == 3 and negative)
        return sign | ((0x7fff << 112) if infinity else ((0x7fff << 112) - 1))
    return sign | ((exponent + 16383) << 112) | (significand - (1 << 112))


def power2(n: int, exponent: int) -> Fraction:
    return Fraction(n << exponent) if exponent >= 0 else Fraction(n, 1 << -exponent)


def cases():
    # Every tested input has finite mathematical value no greater than the
    # largest representable value. Overflow's pinned Bionic behavior belongs
    # to the device/API corpus, not an ideal-IEEE expectation here.
    for exponent in (-16494, -16493, -16400, -16382, -100, -1, 0, 53, 64, 112, 1024, 16382):
        for n in (1, 3, (1 << 112) - 1, (1 << 112) + 1, (1 << 113) - 1):
            shift = exponent - (n.bit_length() - 1)
            if shift < -16494 or exponent > 16383:
                continue
            for sign in (1, -1):
                yield f'{"-" if sign < 0 else ""}0x{n:x}p{shift}', power2(sign * n, shift)
    # Normal and subnormal exact midpoints, including nearest-even odd/even
    # pairs and carry into the next exponent.
    for unit in (-16494, -16400, -16382, -113, -112, -1, 0, 100, 16270):
        for q in (0, 1, 2, (1 << 112) - 1, 1 << 112, (1 << 113) - 2):
            n = 2 * q + 1
            for sign in (1, -1):
                yield f'{"-" if sign < 0 else ""}0x{n:x}p{unit - 1}', power2(sign * n, unit - 1)
    rng = random.Random(0x5639)
    for _ in range(1024):
        width = rng.randrange(1, 180)
        n = rng.randrange(1 << (width - 1), 1 << width)
        shift = rng.randrange(-16494, 16200) - width + 1
        sign = rng.choice((-1, 1))
        yield f'{"-" if sign < 0 else ""}0x{n:x}p{shift}', power2(sign * n, shift)
    for _ in range(1024):
        length = rng.randrange(1, 151)
        digits = str(rng.randrange(1, 10)) + ''.join(str(rng.randrange(10)) for _ in range(length - 1))
        exponent = rng.randrange(-4950, 4900)
        sign = rng.choice((-1, 1))
        shift = exponent - length + 1
        n = int(digits) * sign
        value = Fraction(n * 10 ** shift) if shift >= 0 else Fraction(n, 10 ** -shift)
        yield f'{"-" if sign < 0 else ""}{digits[0]}.{digits[1:]}e{exponent}', value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    corpus = [(text, mode, binary128(value, mode)) for text, value in cases() for mode in range(4)]
    started = time.monotonic()
    result = subprocess.run([str(args.executable.resolve())],
                            input=''.join(f'{mode}\t{text}\n' for text, mode, _ in corpus),
                            text=True, capture_output=True, timeout=120, check=True)
    elapsed = time.monotonic() - started
    rows = result.stdout.splitlines()
    if len(rows) != len(corpus):
        raise SystemExit(f'expected {len(corpus)} records, got {len(rows)}')
    failures = []
    for (text, mode, expected), row in zip(corpus, rows):
        fields = row.split()
        actual, end, error, flags, unchanged = int(fields[0], 16), *map(int, fields[1:])
        if actual != expected or end != len(text) or unchanged != 1:
            failures.append({'text': text, 'mode': mode, 'expected': f'{expected:032x}',
                             'actual': f'{actual:032x}', 'end': end, 'errno': error,
                             'flags': flags, 'rounding_preserved': unchanged})
    report = {'cases': len(corpus), 'seconds': elapsed, 'failures': failures,
              'oracle': 'exact Python integer ratios, 113 significant bits, nearest-even and directed modes'}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Finite binary128: {len(corpus)} mode/input pairs, {elapsed:.3f}s, {len(failures)} failures')
    for failure in failures[:10]:
        print(json.dumps(failure))
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
