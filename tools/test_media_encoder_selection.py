#!/usr/bin/env python3
"""Check the real encoder factory and the unchanged extracted capability function.

The backend functions are explicit creation observers. This gate proves selection
and capability declarations, not Media Foundation encode output or MediaCodec JNI.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'compat/media/jni/media_codec_jni.cc').read_text()
start = source.index('std::vector<std::pair<int, int>> ProfileLevels(')
end = source.index('\njobject JNICALL MCL_getCodecCapabilities', start)
function = source[start:end]
print('ProfileLevels SHA256 ' + hashlib.sha256(function.encode()).hexdigest(), flush=True)
with tempfile.TemporaryDirectory(prefix='gesher-encoder-selection-') as temporary:
    work = Path(temporary)
    unit = work / 'profiles.cc'
    unit.write_text('#include <string>\n#include <vector>\n#include <utility>\nnamespace gesher::media {\n' + function + '\n}\n')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-fno-exceptions', '-fsanitize=undefined', '-fno-sanitize-recover=undefined',
        '-DGESHER_MEDIA_STANDALONE', '-I', str(ROOT), '-I', str(ROOT / 'tests/media/recorder-host/include'),
        str(ROOT / 'tests/media/recorder-host/encoder_selection.cc'), str(ROOT / 'compat/media/encode/encoder.cc'),
        str(unit), '-o', str(work / 'selection')], check=True, timeout=60)
    subprocess.run([str(work / 'selection')], check=True, timeout=15)
