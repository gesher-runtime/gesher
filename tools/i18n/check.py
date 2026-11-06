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
    subprocess.run(['javac', '-encoding', 'UTF-8', '-d', work,
                    str(ROOT / 'app/java/com/gesher/app/locale/UiMessages.java'),
                    str(ROOT / 'tests/i18n/UiMessagesTest.java')], check=True)
    subprocess.run(['java', '-cp', work, 'UiMessagesTest'], check=True)
