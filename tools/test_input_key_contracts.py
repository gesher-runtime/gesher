#!/usr/bin/env python3
"""Run production key state and modifier producers with bounded OS boundaries.

--source-ref compiles the same fixture against baseline method bodies. Win32
key state and Android event/keymap storage are stand-ins; Windows InputTest
and input_probe exercise the real production keymap and InputChannel in CI.
"""
from __future__ import annotations
import argparse, hashlib, os, re, shlex, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
def body(source, start):
    a=source.index(start); b=source.index('{',a); depth=1; c=b+1
    while depth:
        depth += (source[c]=='{')-(source[c]=='}'); c+=1
    return source[a:c]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-ref');p.add_argument('--sanitize',action='store_true')
    p.add_argument('--cxx',default=os.environ.get('CXX','c++'));a=p.parse_args()
    def read(path):
        return subprocess.check_output(['git','show',a.source_ref+':'+path],cwd=ROOT,text=True) if a.source_ref else (ROOT/path).read_text()
    source=read('compat/input/translate.cc');header=read('compat/input/translate.h')
    window=read('compat/hwui/window/gesher_window.cc');ime=read('compat/input/ime.cc')
    fields=header[header.rindex('  // Keys.'):header.index('  // Touch.',header.rindex('  // Keys.'))]
    start=window.index('    case WM_SYSKEYUP: {')
    producer=window[window.index('      if (int bit = ',start):window.index('      e.type = down ?',start)]
    guard=re.search(r'if \(s.enter_action != 0 && vk == VK_RETURN && (.*?)\) \{',ime).group(1)
    values={'@META@':body(source,'int32_t MetaState('),'@KEY@':body(source,'void Translator::Key('),
      '@TRANSLATE@':body(source,'void Translator::Translate('),'@FIELDS@':fields,'@MODIFIERS@':body(window,'int Window::Modifiers() const'),
      '@IME@':body(ime,'int32_t KeyModifiers()'),'@PRODUCER@':producer,'@GUARD@':guard,
      '@LEGACY_MODIFIER@':body(window,'int ModifierOfKey(') if 'int ModifierOfKey(' in window else ''}
    template=(ROOT/'tests/input/key_contracts_host.cc.in').read_text()
    for key,value in values.items():
        if template.count(key)!=1:raise ValueError(key)
        template=template.replace(key,value)
    helpers = {}
    for name, path in [('input_modifiers.h','compat/hwui/window/input_modifiers.h'),
                       ('key_history.h','compat/input/key_history.h')]:
        probe = subprocess.run(['git','show',a.source_ref+':'+path],cwd=ROOT,text=True,
                               capture_output=True) if a.source_ref else None
        helpers[name] = probe.stdout if probe is not None and probe.returncode == 0 else (ROOT/path).read_text()
    print('input source SHA256',hashlib.sha256((source+header+window+ime+''.join(helpers.values())).encode()).hexdigest(),flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-key-contracts-') as temp:
        src=Path(temp)/'probe.cc';exe=Path(temp)/'probe';src.write_text(template)
        for name, value in helpers.items(): (Path(temp)/name).write_text(value)
        command=shlex.split(a.cxx)+['-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',
          '-I'+str(temp),'-I'+str(ROOT/'compat/input'),'-I'+str(ROOT/'compat/hwui/window'),str(src),'-o',str(exe)]
        if a.sanitize:command+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
        subprocess.run(command,check=True,timeout=120)
        return subprocess.run([str(exe)],timeout=30).returncode
if __name__=='__main__':raise SystemExit(main())
