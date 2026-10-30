"""Role-specific Windows import policy; no application/private runtime DLLs."""
import json
from pathlib import Path
import re

POLICY = json.loads(Path(__file__).with_name('import-policy.json').read_text(encoding='utf-8-sig'))
def allowed(role, names):
    if role not in POLICY or not names or len(set(n.lower() for n in names)) != len(names):
        return False
    return all(re.fullmatch(r'[a-z0-9_-]+\.dll', n.lower()) and (n.lower() in POLICY[role]['system'] or n.lower() in ('msvcrt.dll', 'ucrtbase.dll') or re.fullmatch(r'api-ms-win-crt-[a-z0-9-]+-l1-1-0\.dll', n.lower())) for n in names)

def controls():
    for role in POLICY:
        assert allowed(role, ['kernel32.dll', 'api-ms-win-crt-runtime-l1-1-0.dll'])
        for bad in ['unknown.dll', 'gesher_private.dll', '../kernel32.dll', 'userenv.dll']:
            assert not allowed(role, ['kernel32.dll', bad]), (role, bad)
        assert not allowed(role, [])
    assert not allowed('gesher_arm64.dll', ['WS2_32.dll'])
    assert not allowed('gesher_arm64.dll', ['WINHTTP.dll'])
    assert not allowed('net_tcp_priority_probe.exe', ['WINHTTP.dll'])
    assert not allowed('foreign-role', ['kernel32.dll'])
    print('PASS TCP priority import policy: unknown/private/path/empty/wrong-role negative controls')

if __name__ == '__main__':
    controls()
