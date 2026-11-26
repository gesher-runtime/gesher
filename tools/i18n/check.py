#!/usr/bin/env python3
"""Check translations, their consumers, and the portable Java behavior tests."""
import json
import re
import subprocess
import tempfile
from pathlib import Path
from generate import ROOT, main

main()
messages = json.loads((ROOT / 'tools/i18n/messages.json').read_text(encoding='utf-8'))['messages']
sources = [ROOT / 'app/java/com/gesher/app/Launcher.java',
           ROOT / 'app/java/com/gesher/app/shell/AppShell.java',
           ROOT / 'app/java/com/gesher/server/SystemServer.java',
           ROOT / 'app/java/com/gesher/app/webview/GesherWebViewProvider.java']
for source in sources:
    for key in re.findall(r'\btext\("([a-z_]+)"', source.read_text(encoding='utf-8')):
        assert key in messages, (source, key)
for source in (ROOT / 'tools/package').glob('*.ps1'):
    for key in re.findall(r"Get-GesherText '([a-z_]+)'", source.read_text(encoding='utf-8-sig')):
        assert key in messages, (source, key)
with tempfile.TemporaryDirectory(prefix='gesher-ui-') as work:
    # Exercise the exact production mapper without loading Android services. Only
    # UiText's host-language lookup is replaced; real UiMessages formats the text.
    launcher = (ROOT / 'app/java/com/gesher/app/Launcher.java').read_text(encoding='utf-8')
    start = '    static String describeFailure(Throwable t) {'
    end = '\n    /** Writes the failure'
    if launcher.count(start) != 1 or launcher.count(end) != 1:
        raise ValueError('Launcher failure-mapping boundary changed')
    method = launcher[launcher.index(start):launcher.index(end)]
    host = Path(work) / 'Launcher.java'
    host.write_text('package com.gesher.app;\n'
                    'import static com.gesher.app.LauncherFailureTest.text;\n'
                    'final class Launcher {\n' + method + '\n}\n', encoding='utf-8')
    subprocess.run(['javac', '-encoding', 'UTF-8', '-d', work,
                    str(ROOT / 'app/java/com/gesher/app/locale/UiMessages.java'),
                    str(ROOT / 'tests/i18n/UiMessagesTest.java'),
                    str(ROOT / 'tests/i18n/LauncherFailureTest.java'), str(host)], check=True)
    subprocess.run(['java', '-cp', work, 'UiMessagesTest'], check=True)
    subprocess.run(['java', '-cp', work, 'com.gesher.app.LauncherFailureTest'], check=True)
