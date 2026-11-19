#!/usr/bin/env python3
"""Execute the production Windows clipboard access code against explicit OS-call boundaries.

This covers transient errors, process generation, activation changes and ownership of
handles/buffers. clipboard-access.yml separately compiles/runs with actual Windows APIs.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
compiler = shutil.which("c++")
if compiler is None:
    raise SystemExit("A C++ compiler is required")
with tempfile.TemporaryDirectory(prefix="gesher-clipboard-native-") as temporary:
    output = Path(temporary) / "clipboard-access"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I" + str(ROOT / "tests/clipboard/win32_boundary"),
                    "-I" + str(ROOT / "compat/devsvc"),
                    str(ROOT / "compat/devsvc/clipboard_access.cc"),
                    str(ROOT / "tests/clipboard/access_boundary_test.cc"), "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True, timeout=10)
