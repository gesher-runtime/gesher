"""CI-only actual executable/linker-map boundary; no VM preload for fonts."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

exe, map_file, compiler = map(Path, sys.argv[1:])
raw = map_file.read_bytes()
mapping = raw.decode('utf-8', errors='strict')
for required in ('native_fonts.cc.obj', 'libminikin.a', 'MinikinSkia.cpp.obj'):
    assert required in mapping, ('Missing actual standalone font input', required)
for forbidden in ('libart_runtime.a', 'libart_runtime_glue.a', 'framework_runtime.dir/',
                  'dalvikvm.dir/', 'JNI_CreateJavaVM', 'JNI_GetCreatedJavaVMs'):
    assert forbidden not in mapping, ('Unexpected VM/framework input', forbidden)
reader = compiler.with_name('llvm-readobj')
imports = subprocess.check_output([str(reader), '--coff-imports', str(exe)], text=True)
assert not re.search(r'(?i)(jvm|dalvik|libart_runtime)', imports), imports
objects = sorted(set(re.findall(r'\S+(?:\.a\([^\n]*?\)|\.obj)', mapping)))
print('FONT_STANDALONE_LINK ' + json.dumps({
    'executable_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
    'map_sha256': hashlib.sha256(raw).hexdigest(),
    'objects': len(objects),
    'font_inputs': [obj for obj in objects if any(part in obj for part in
                    ('native_fonts.', 'MinikinSkia.', 'libminikin.', 'fontations_fallback.'))],
    'import_dlls': re.findall(r'Name: (.+)', imports),
    'vm_inputs': False,
}, sort_keys=True), flush=True)
