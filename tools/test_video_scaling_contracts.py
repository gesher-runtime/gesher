#!/usr/bin/env python3
"""Exercise production video scaling through QueueSurface, display and Parcel code.

Complete QueueSurface and RenderFrame plus the actual display/cache/Parcel methods
are compiled. Android native-window dispatch, I420 conversion, Skia rasterization,
JNI handles and Parcel storage are explicit host boundaries. Real decoder, Skia,
Binder and ART coverage lives in surfaceapp's mandatory Windows scaling run.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from test_media_surface_contracts import method, section

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref', help='Compile production at this revision, keeping these assertions')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--android-reference', type=Path,
                        help='Optional Android 16 r4 GLConsumer.cpp primary-source comparison')
    parser.add_argument('--generate', type=Path, help='Keep the generated host translation unit')
    args = parser.parse_args()
    originals: dict[str, str] = {}

    def read(path: str) -> str:
        if args.source_ref:
            value = subprocess.run(['git', 'show', args.source_ref + ':' + path], cwd=ROOT,
                                   check=True, capture_output=True, text=True).stdout
        else:
            value = (ROOT / path).read_text()
        originals[path] = value
        return value

    def portable(value: str) -> str:
        # Only unavailable includes and destructor accessibility are adapted.
        return re.sub(r'^\s*#(?:include|pragma).*\n', '', value, flags=re.MULTILINE).replace(
            'protected:', 'public:')

    header = read('compat/media/surface/queue_surface.h')
    queue = read('compat/media/surface/queue_surface.cc')
    render = read('compat/media/video/render.cc')
    display = read('compat/media/surface/surface_jni.cc')
    parcel = read('compat/framework/surface_parcel.cc')
    scaling = 'VideoSourceRect(' in display
    crop = read('compat/media/surface/video_scaling.h') if scaling else ''
    constants = set(re.findall(r'\bNATIVE_WINDOW_[A-Z0-9_]+\b', header + queue))
    constants.discard('NATIVE_WINDOW_TIMESTAMP_AUTO')
    window_boundary = (ROOT / 'tests/media/surface_geometry_host.cc.in').read_text().split('@PRODUCTION@')[0]
    window_boundary = window_boundary.replace('@CONSTANTS@',
        'enum { ' + ', '.join(sorted(constants)) + ' };\n')
    window_boundary = window_boundary.replace('  sp() = default;',
        '  sp() = default;\n  explicit sp(T* ptr) : ptr_(ptr, [](T*) {}) {}')
    image_boundary = (ROOT / 'tests/media/surface_image_cache_host.cc.in').read_text()
    image_boundary = section(image_boundary, 'template<class T> using sk_sp', '@CACHE@')
    image_boundary = re.sub(r'struct SurfaceFrame \{.*?\n\};\n', '', image_boundary, flags=re.DOTALL)
    image_boundary = image_boundary.replace('  uint32_t pixel() const',
        '  int width() const { return info.width; }\n'
        '  int height() const { return info.height; }\n  uint32_t pixel() const')
    reader = method(parcel, 'status_t ReadFrameVideoScalingMode(') if scaling else ''
    writer = method(parcel, '    if (video_scaling_mode != 1) {') if scaling else '(void)data; (void)video_scaling_mode;'
    version = re.search(r'constexpr int32_t kFrameScalingVersion = \d+;', parcel)
    reference = ''
    if args.android_reference:
        raw = args.android_reference.read_text()
        digest = hashlib.sha256(raw.encode()).hexdigest()
        # Exact primary source: frameworks/native, android-16.0.0_r4,
        # libs/gui/GLConsumer.cpp. Do not silently compare another implementation.
        expected = '25cdc494a33f89c8a40aefb5b022b54435b90e277bfbba8c19789f9a8f2b5805'
        if digest != expected:
            raise ValueError('Android reference SHA256 differs: ' + digest)
        reference = method(raw, 'Rect GLConsumer::scaleDownCrop(').replace(
            'Rect GLConsumer::scaleDownCrop(', 'Rect ScaleDownCrop(')
        print('Android reference SHA256 ' + digest, flush=True)

    replacements = {
        '@WINDOW_BOUNDARY@': window_boundary,
        '@SURFACE_PRODUCTION@': portable(header) + portable(queue),
        '@IMAGE_BOUNDARY@': image_boundary,
        '@CACHE@': section(display, 'class FrameImages {', '// --- SurfaceTexture'),
        '@RENDER@': portable(render),
        '@CROP@': portable(crop),
        '@SOURCE_RECT@': method(display, 'SkRect VideoSourceRect(') if scaling else '',
        '@LAYER@': section(display, 'class TextureLayer :', 'TextureLayer* LayerOf('),
        '@COPY@': method(display, 'jboolean JNICALL HR_copyLayerInto('),
        '@VIEW@': section(display, 'class ViewSurfaceDrawable :', '// SurfaceView.nativeGesherDrawSurface'),
        '@PARCEL_VERSION@': version[0] if version else '',
        '@PARCEL_READER@': reader,
        '@PARCEL_WRITER@': writer,
        '@ANDROID_REFERENCE@': reference,
        '@HAS_SCALING@': str(int(scaling)),
        '@HAS_ANDROID_REFERENCE@': str(int(bool(reference))),
    }
    fixture = (ROOT / 'tests/media/video_scaling_host.cc.in').read_text()
    for marker, body in replacements.items():
        if fixture.count(marker) != 1:
            raise ValueError('fixture boundary changed: ' + marker)
        fixture = fixture.replace(marker, body)
    for path, source in originals.items():
        print(hashlib.sha256(source.encode()).hexdigest() + '  ' + path, flush=True)
    if args.generate:
        args.generate.write_text(fixture)
    with tempfile.TemporaryDirectory(prefix='gesher-video-scaling-') as temporary:
        work = Path(temporary)
        generated = work / 'scaling.cc'
        generated.write_text(fixture)
        binary = work / ('scaling.exe' if os.name == 'nt' else 'scaling')
        command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                         '-pthread', str(generated), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
