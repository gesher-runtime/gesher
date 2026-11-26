#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Compile actual-provider mutants and require their precise contract failures."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def replace(path,before,after):
    source=path.read_text()
    if source.count(before)!=1: raise RuntimeError('Mutation anchor is not unique: '+before)
    path.write_text(source.replace(before,after))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home',type=Path,required=True)
    parser.add_argument('--okhttp-source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    names=['Http2Bindings.java','Http2Transport.java','Http2SessionPool.java']
    before={name:hashlib.sha256((ROOT/'app/java/android/net/http'/name).read_bytes()).hexdigest() for name in names}
    mutants={
        'partial-publication':'Failed provider resolution poisoned later retries',
        'cached-policy-instance':'Warm binding cached a cleartext allowance',
    }
    receipts=[]
    with tempfile.TemporaryDirectory(prefix='gesher-http2-bindings-negative-') as temporary:
        for name,expected in mutants.items():
            root=Path(temporary)/name; root.mkdir()
            for directory in ['app/java/android/net/http','tests/net','patches/external/okhttp']:
                shutil.copytree(ROOT/directory,root/directory)
            (root/'tools').mkdir()
            for tool in ['test_http_engine.py','test_http_engine_policy.py']:
                shutil.copyfile(ROOT/'tools'/tool,root/'tools'/tool)
            bindings=root/'app/java/android/net/http/Http2Bindings.java'
            if name=='partial-publication':
                replace(bindings,'    private Http2Bindings() throws ReflectiveOperationException {',
                        '    private Http2Bindings() throws ReflectiveOperationException {\n        resolved=this;')
            else:
                replace(bindings,'    final Object cancel, http2;','    final Object cancel, http2;\n    final Object cachedPolicy;')
                replace(bindings,'        policyInstance=policy.getMethod("getInstance");',
                        '        policyInstance=policy.getMethod("getInstance");\n        cachedPolicy=policyInstance.invoke(null);')
                replace(root/'app/java/android/net/http/Http2SessionPool.java',
                        'Object policy = Http2Transport.invoke(api.policyInstance, null);',
                        'Object policy = api.cachedPolicy;')
            classes=root/'compiled'
            built=subprocess.run([sys.executable,str(root/'tools/test_http_engine.py'),
                '--java-home',str(args.java_home),'--okhttp-source',str(args.okhttp_source),
                '--compile-output',str(classes)],capture_output=True,text=True,timeout=180,check=True)
            tested=subprocess.run([str(args.java_home/'bin/java'),'-cp',str(classes),'Http2BindingsLoaderHost'],
                                  capture_output=True,text=True,timeout=60)
            output=tested.stdout+tested.stderr
            if tested.returncode==0 or ('java.lang.AssertionError: '+expected) not in output:
                raise RuntimeError(name+' did not fail its intended assertion: '+output)
            receipts.append({'mutant':name,'expected':expected,'exit':tested.returncode,
                             'compile_output':built.stdout+built.stderr,'output':output})
            print('NEGATIVE_CONTROL_PASS '+name+': '+expected,flush=True)
    after={name:hashlib.sha256((ROOT/'app/java/android/net/http'/name).read_bytes()).hexdigest() for name in names}
    if before!=after: raise RuntimeError('Production source changed during controls')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps({'source_sha256':before,'controls':receipts},indent=2)+'\n')


if __name__=='__main__': main()
