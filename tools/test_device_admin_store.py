#!/usr/bin/env python3
"""Exercise durable production device-admin grants; does not emulate Windows locking."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--java-home', type=Path, required=True)
args = p.parse_args()
with tempfile.TemporaryDirectory(prefix='gesher-device-admin-') as temporary:
    work = Path(temporary)
    subprocess.run([str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8', '-d', str(work),
                    str(ROOT / 'app/java/com/gesher/server/DeviceAdminStore.java'),
                    str(ROOT / 'tests/device-admin/DeviceAdminStoreTest.java')], check=True)
    data = work / 'data'
    data.mkdir()
    subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(work),
                    'com.gesher.server.DeviceAdminStoreTest', str(data)], check=True, timeout=20)
