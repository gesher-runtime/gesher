#!/usr/bin/env python3
"""Run InputMethodManagerService's production callback-taking handwriting methods on a JVM.

Unlike test_handwriting_unavailable.py this needs no built framework classes: the methods
are sliced out of the production source and compiled against minimal declarations of the
few framework types they touch. It checks null callbacks and callbacks that throw
RemoteException.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RELATIVE = 'app/java/com/gesher/server/InputMethodManagerService.java'
STUBS = {
    'android/os/RemoteException.java': '''package android.os;
public class RemoteException extends Exception {
    public RemoteException(String message) { super(message); }
    public RuntimeException rethrowAsRuntimeException() { throw new RuntimeException(this); }
}''',
    'android/view/inputmethod/CursorAnchorInfo.java': 'package android.view.inputmethod; public class CursorAnchorInfo {}',
    'com/android/internal/inputmethod/IInputMethodClient.java': 'package com.android.internal.inputmethod; public interface IInputMethodClient {}',
    'com/android/internal/inputmethod/IBooleanListener.java': '''package com.android.internal.inputmethod;
public interface IBooleanListener { void onResult(boolean value) throws android.os.RemoteException; }''',
    'com/android/internal/inputmethod/IConnectionlessHandwritingCallback.java': '''package com.android.internal.inputmethod;
public interface IConnectionlessHandwritingCallback {
    void onResult(CharSequence text) throws android.os.RemoteException;
    void onError(int code) throws android.os.RemoteException;
}''',
}


def slice_between(source: str, start: str, end: str | None) -> str:
    if source.count(start) != 1 or (end is not None and source.count(end) != 1):
        raise ValueError('production handwriting method boundary changed')
    begin = source.index(start)
    stop = source.index(end, begin) if end is not None else source.rindex('}')
    return source[begin:stop]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    source = (subprocess.check_output(['git', 'show', args.source_ref + ':' + RELATIVE], cwd=ROOT, text=True)
              if args.source_ref else (ROOT / RELATIVE).read_text())
    methods = slice_between(source, '    @Override\n    public void startConnectionlessStylusHandwriting(',
                            '    @Override\n    public void prepareStylusHandwritingDelegation(')
    methods += slice_between(source, '    @Override\n    public void acceptStylusHandwritingDelegationAsync(', None)
    template = (ROOT / 'tests/handwriting/HandwritingCallbackHost.java.in').read_text()
    with tempfile.TemporaryDirectory(prefix='gesher-handwriting-callbacks-') as tmp:
        work = Path(tmp)
        sources = []
        for name, content in STUBS.items():
            path = work / 'src' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            sources.append(str(path))
        host = work / 'src/com/gesher/server/HandwritingCallbackHost.java'
        host.parent.mkdir(parents=True, exist_ok=True)
        host.write_text(template.replace('// PRODUCTION_HANDWRITING_METHODS', methods))
        sources.append(str(host))
        classes = work / 'classes'
        classes.mkdir()
        subprocess.run([str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8', '-d', str(classes), *sources],
                       check=True, timeout=90)
        subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(classes),
                        'com.gesher.server.HandwritingCallbackHost'], check=True, timeout=60)


if __name__ == '__main__':
    main()
