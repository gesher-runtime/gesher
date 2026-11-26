#!/usr/bin/env python3
"""Qualify the real bounded Annex-B helper and complete native MP4 writer.

The helper and MP4/Ogg writer translation units execute unmodified. Format's
metadata map and the unselected, failing WebM factory are explicit host seams.
FFmpeg encodes the real inputs and independently decodes every real finalized
stream. Malformed byte-pattern controls are labeled and never called codecs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'compat/media/encode/codec_config.cc'
INPUTS = [SOURCE, 'compat/media/encode/codec_config.h', 'tests/media/annexb_split_contracts.cc',
          'tests/media/annexb_mux_work.cc', 'compat/media/mux/mp4_muxer.cc', 'compat/media/mux/ogg_muxer.cc',
          'compat/media/mux/muxer.h', 'compat/media/player/extractor.h', 'compat/media/player/data_source.h',
          'compat/media/audio/log.h', 'tests/media/recorder-host/include/media/NdkMediaFormat.h',
          'tools/test_annexb_scan.py']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                            timeout=180, **kwargs)
    if result.returncode:
        raise RuntimeError(f'{shlex.join(list(map(str, command)))}\n{result.stdout}{result.stderr}')
    return result


def reference_units(data):
    """Independent bytes.find packet preparation, preserving existing semantics."""
    at = data.find(b'\x00\x00\x01')
    result = []
    while at >= 0:
        start = at + 3
        following = data.find(b'\x00\x00\x01', start)
        if following < 0:
            if start < len(data):
                result.append(data[start:])
            break
        result.append(data[start:following].rstrip(b'\x00'))
        at = following
    return result


def converted(packet):
    if packet.startswith((b'\x00\x00\x01', b'\x00\x00\x00\x01')):
        return b''.join(struct.pack('>I', len(nal)) + nal for nal in reference_units(packet)
                        if nal and nal[0] & 31 != 9)
    return packet


def pack(path, width, height, packets):
    path.write_bytes(struct.pack('<4I', 0x31425641, width, height, len(packets)) +
        b''.join(struct.pack('<IIq', len(data), flags, timestamp) + data
                 for data, flags, timestamp in packets))


def unpack(path):
    data = path.read_bytes()
    magic, width, height, count = struct.unpack_from('<4I', data)
    assert magic == 0x31425641
    at = 16
    packets = []
    for _ in range(count):
        size, flags, timestamp = struct.unpack_from('<IIq', data, at)
        at += 16
        assert size <= len(data) - at
        packets.append((data[at:at + size], flags, timestamp))
        at += size
    assert at == len(data)
    return width, height, packets


def prepare_inputs(directory):
    directory.mkdir(parents=True, exist_ok=True)
    cases = []
    commands = []
    real = [('tiny', 16, 16, 1, False), ('small', 320, 180, 30, False),
            ('720p', 1280, 720, 30, False), ('1080p', 1920, 1080, 30, False),
            ('4k', 3840, 2160, 15, False), ('flat', 1280, 720, 30, True)]
    for name, width, height, frames, flat in real:
        encoded = directory / f'{name}.h264'
        source = f'color=black:size={width}x{height}:rate=30' if flat else f'testsrc2=size={width}x{height}:rate=30'
        command = ['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', source,
                   '-frames:v', str(frames), '-an', '-c:v', 'libx264', '-preset', 'ultrafast',
                   '-x264-params', 'threads=1:keyint=12:bframes=0:repeat-headers=1:aud=1',
                   '-pix_fmt', 'yuv420p', '-f', 'h264', encoded]
        run(command)
        commands.append(list(map(str, command)))
        probe = json.loads(run(['ffprobe', '-v', 'error', '-f', 'h264', '-show_packets',
            '-show_entries', 'packet=pos,size,flags', '-of', 'json', encoded]).stdout)
        compressed = encoded.read_bytes()
        packets = []
        consumed = 0
        for i, entry in enumerate(probe['packets']):
            pos, size = int(entry['pos']), int(entry['size'])
            assert pos == consumed and size > 0 and pos + size <= len(compressed)
            packets.append((compressed[pos:pos + size], int('K' in entry['flags']), i * 1000000 // 30))
            consumed += size
        assert consumed == len(compressed) and len(packets) == frames
        config = []
        for kind in (7, 8):
            nals = [nal for nal in reference_units(packets[0][0]) if nal and nal[0] & 31 == kind]
            assert nals
            config.append(nals[0])
        pack(directory / f'{name}.packets', width, height, packets)
        pack(directory / f'{name}.csd', width, height, [(nal, 2, 0) for nal in config])
        cases.append({'name': name, 'width': width, 'height': height, 'packets': f'{name}.packets',
            'config': f'{name}.csd', 'encoded': encoded.name, 'configuration': 'format',
            'real_encoded_stream': True, 'frames': frames})
    source = next(case for case in cases if case['name'] == '1080p')
    width, height, packets = unpack(directory / source['packets'])
    pack(directory / 'avcc.packets', width, height, [(converted(data), flags, pts) for data, flags, pts in packets])
    cases.append({**source, 'name': 'avcc-control', 'packets': 'avcc.packets'})
    cases.append({**source, 'name': 'config-sample', 'configuration': 'config'})
    # These exercise complete writer work on opaque malformed payloads. Their
    # container bytes are checked; no decoder-success or real-codec claim.
    patterns = [('dense-zero', b'\x00' * 65535), ('escaped-zero', b'\x00\x00\x03' * 21845),
                ('all-one', b'\x01' * 65535), ('dense-prefix', (b'\x00\x00\x01\x65\x01\x01\x01') * 8192)]
    for name, payload in patterns:
        packet = b'\x00\x00\x01\x65' + payload
        pack(directory / f'{name}.packets', 16, 16, [(packet, 1, i * 1000000 // 30) for i in range(30)])
        cases.append({'name': name, 'width': 16, 'height': 16, 'packets': f'{name}.packets',
                      'config': 'tiny.csd', 'configuration': 'format', 'real_encoded_stream': False})
    manifest = {'cases': cases, 'generation_commands': commands,
                'ffmpeg_version': run(['ffmpeg', '-version']).stdout,
                'inputs': {path.name: {'sha256': sha(path), 'bytes': path.stat().st_size}
                           for path in sorted(directory.iterdir()) if path.is_file()}}
    (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def frame_hashes(path):
    output = run(['ffmpeg', '-v', 'error', '-xerror', '-threads', '1', '-i', path,
                  '-map', '0:v:0', '-fps_mode', 'passthrough', '-pix_fmt', 'yuv420p',
                  '-f', 'framehash', '-hash', 'sha256', '-']).stdout
    return [tuple(part.strip() for part in line.split(','))[-2:]
            for line in output.splitlines() if line and not line.startswith('#')]


def verify_container(path, packets):
    data = path.read_bytes()
    boxes = {}
    at = 0
    while at < len(data):
        assert len(data) - at >= 8
        size, kind = struct.unpack_from('>I4s', data, at)
        header = 8
        if size == 1:
            assert len(data) - at >= 16
            size = struct.unpack_from('>Q', data, at + 8)[0]
            header = 16
        assert size >= header and size <= len(data) - at and kind not in boxes
        boxes[kind] = data[at + header:at + size]
        at += size
    assert at == len(data) and set(boxes) == {b'ftyp', b'mdat', b'moov'}
    assert boxes[b'mdat'] == b''.join(converted(packet[0]) for packet in packets)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare-ref', help='Immutable source for baseline helper; common writer/caller objects are identical')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--contracts-only', action='store_true')
    parser.add_argument('--reuse-fixtures', type=Path)
    args = parser.parse_args()
    input_hashes = {path: sha(ROOT / path) for path in INPUTS}
    with tempfile.TemporaryDirectory(prefix='gesher-annexb-') as temporary:
        work = args.output_dir.resolve() if args.output_dir else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        compile_commands = []
        flags = ['-std=gnu++23', '-O1' if args.sanitize else '-O2', '-g', '-pthread',
                 '-fno-exceptions', '-fsized-deallocation', '-mno-ms-bitfields',
                 '-Wall', '-Wextra', '-Werror', '-I', ROOT, '-I', ROOT / 'compat/media/encode',
                 '-I', ROOT / 'tests/media/recorder-host/include', '-DGESHER_MEDIA_STANDALONE']
        link = []
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie']
            link += ['-fsanitize=address,undefined', '-no-pie']
        cxx = shlex.split(args.cxx)

        def compile_object(source, output, extra=()):
            command = [*cxx, *flags, *extra, '-c', source, '-o', output]
            run(command)
            compile_commands.append(list(map(str, command)))

        contract_object = work / 'contracts.o'
        compile_object(ROOT / 'tests/media/annexb_split_contracts.cc', contract_object)
        sources = [('candidate', ROOT / SOURCE)]
        baseline_ref = None
        if args.compare_ref:
            baseline_ref = run(['git', 'rev-parse', '--verify', args.compare_ref + '^{commit}'], cwd=ROOT).stdout.strip()
            baseline = work / 'baseline-codec_config.cc'
            baseline.write_bytes(subprocess.check_output(['git', 'show', baseline_ref + ':' + SOURCE], cwd=ROOT))
            sources.insert(0, ('baseline', baseline))
        receipts = []
        for name, source in sources:
            obj, binary = work / f'{name}-codec.o', work / f'{name}-contracts'
            compile_object(source, obj)
            command = [*cxx, '-pthread', *link, contract_object, obj, '-o', binary]
            run(command); compile_commands.append(list(map(str, command)))
            result = run([binary])
            (work / f'{name}-contracts.log').write_text(result.stdout + result.stderr)
            assert 'PASS' in result.stdout
            print(name + ': ' + result.stdout.strip(), flush=True)
            receipts.append({'variant': name, 'source_sha256': sha(source), 'object_sha256': sha(obj),
                             'binary_sha256': sha(binary), 'stdout': result.stdout, 'stderr': result.stderr})
        negatives = []
        if args.negative_controls:
            text = (ROOT / SOURCE).read_text()
            mutations = [
                ('skip-zero-containing-word', 'if (((word - low) & ~word & high) == 0)',
                 'if (((word - low) & ~word & high) == 0 || word != 0)', 'leading garbage ignored and only interior zeros trimmed'),
                ('skip-nine-bytes', 'i += sizeof(word);', 'i += sizeof(word) + 1;', 'prefix inside or crossing a complete word'),
                ('drop-interior-empty', 'nals.emplace_back(p + start, end - start);',
                 'if (end > start) nals.emplace_back(p + start, end - start);', 'consecutive start codes retain interior empty NAL'),
                ('retain-interior-padding', 'while (end > start && p[end - 1] == 0) end--;',
                 'while (false && end > start && p[end - 1] == 0) end--;', 'leading garbage ignored and only interior zeros trimmed'),
                ('drop-final-zero', 'nals.emplace_back(p + start, n - start);',
                 'nals.emplace_back(p + start, n - start - (p[n - 1] == 0));', 'four-byte start and final trailing zero retained'),
                ('emit-final-empty', 'start < n)', 'start <= n)', 'final empty NAL omitted'),
            ]
            for name, before, after, assertion in mutations:
                assert text.count(before) == 1, name
                source, obj, binary = work / f'mutant-{name}.cc', work / f'mutant-{name}.o', work / f'mutant-{name}'
                source.write_text(text.replace(before, after))
                compile_object(source, obj)
                command = [*cxx, '-pthread', *link, contract_object, obj, '-o', binary]
                run(command); compile_commands.append(list(map(str, command)))
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
                (work / f'mutant-{name}.log').write_text(result.stdout + result.stderr)
                assert result.returncode == 1 and result.stderr.count('FAIL Annex B:') == 1
                assert 'FAIL Annex B: ' + assertion in result.stderr, (name, result.stderr)
                assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
                negatives.append({'name': name, 'expected_assertion': assertion, 'source_sha256': sha(source),
                                  'binary_sha256': sha(binary), 'returncode': result.returncode, 'stderr': result.stderr})
                print('rejected ' + name + ': ' + assertion, flush=True)
        evidence = {'baseline_ref': baseline_ref, 'source': SOURCE, 'variants': receipts, 'negative_controls': negatives}
        if not args.contracts_only:
            fixtures = args.reuse_fixtures.resolve() if args.reuse_fixtures else work / 'fixtures'
            manifest = json.loads((fixtures / 'manifest.json').read_text()) if args.reuse_fixtures else prepare_inputs(fixtures)
            for name, expected in manifest['inputs'].items():
                assert sha(fixtures / name) == expected['sha256']
            common = []
            for name, relative in [('caller', 'tests/media/annexb_mux_work.cc'),
                                   ('mp4', 'compat/media/mux/mp4_muxer.cc'), ('ogg', 'compat/media/mux/ogg_muxer.cc')]:
                obj = work / f'{name}.o'
                # Existing muxer virtual declarations deliberately ignore some
                # arguments. This is the same local warning setting as its
                # production target; it does not suppress helper diagnostics.
                compile_object(ROOT / relative, obj, ['-Wno-unused-parameter'])
                common.append(obj)
            mux_receipts = []
            for name, _ in sources:
                binary = work / f'{name}-mux'
                command = [*cxx, '-pthread', *link, *common, work / f'{name}-codec.o', '-o', binary]
                run(command); compile_commands.append(list(map(str, command)))
                for case in manifest['cases']:
                    output = work / f'{name}-{case["name"]}.mp4'
                    result = run([binary, fixtures / case['packets'], fixtures / case['config'], output,
                                  '-', 1, 0, case['configuration'], 'prepare'])
                    _, _, packets = unpack(fixtures / case['packets'])
                    verify_container(output, packets)
                    reference = work / f'{sources[0][0]}-{case["name"]}.mp4'
                    assert output.read_bytes() == reference.read_bytes(), case['name']
                    decoded = []
                    if case['real_encoded_stream']:
                        decoded = frame_hashes(output)
                        expected = frame_hashes(fixtures / case['encoded'])
                        assert decoded == expected and len(decoded) == case['frames'], case['name']
                    mux_receipts.append({'variant': name, 'case': case['name'], 'output_sha256': sha(output),
                        'bytes': output.stat().st_size, 'decoded_frame_hashes': decoded,
                        'observation': json.loads(result.stdout), 'binary_sha256': sha(binary)})
                    print(f'{name} {case["name"]}: complete file verified, {len(decoded)} independently decoded frames', flush=True)
            evidence['mux'] = mux_receipts
            evidence['fixtures'] = manifest
            evidence['common_objects'] = {path.name: sha(path) for path in common}
        evidence['commands'] = compile_commands
        assert input_hashes == {path: sha(ROOT / path) for path in INPUTS}, 'source changed during qualification'
        evidence['input_hashes'] = input_hashes
        (work / 'receipt.json').write_text(json.dumps(evidence, indent=2) + '\n')
        print('Annex-B bounded scan and complete mux qualification: PASS', flush=True)


if __name__ == '__main__':
    main()
