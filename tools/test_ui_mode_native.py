#!/usr/bin/env python3
"""Execute the production AppsUseLightTheme function with a declared Win32 registry boundary."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A C++ compiler is required")
    source = (ROOT / "compat/devsvc/devsvc_win32.cc").read_text()
    begin = source.index("int AppsUseLightTheme() {")
    end = source.index("\n}", begin) + 2
    body = (ROOT / "tests/ui-mode/native_theme_host.cc.in").read_text().replace(
        "@PRODUCTION_FUNCTION@", source[begin:end])
    with tempfile.TemporaryDirectory(prefix="gesher-ui-native-") as temporary:
        directory = Path(temporary)
        test = directory / "theme.cc"
        test.write_text(body)
        binary = directory / "theme"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
