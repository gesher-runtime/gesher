#!/usr/bin/env python3
"""Build/run actual native WebStorage admission and dispatch with a recording COM seam.

The Windows URLMON parser and Gesher callback implementation are real. No browser
or profile is ever created. --source-ref replaces only ClearOriginData for a safe
negative control; the current origin parser remains separately tested.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True, type=Path)
    parser.add_argument("--sdk-include", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--no-run", action="store_true", help="Cross-compile only; not runtime evidence")
    parser.add_argument("--source-ref", help="Use an earlier native dispatch implementation")
    parser.add_argument("--negative-control", action="store_true",
                        help="Omit only origin admission; require the recording test to reject it")
    args = parser.parse_args()
    if args.source_ref and args.negative_control:
        parser.error("choose --source-ref or --negative-control")
    relative = "compat/webview/webview_host.cc"
    current = (ROOT / relative).read_text()
    dispatch = current
    if args.source_ref:
        dispatch = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                  cwd=ROOT, check=True, capture_output=True, text=True).stdout
    source = (ROOT / "tests/native/webstorage_origin_host_test.cc.in").read_text()
    source = source.replace("@JSON_STRING@", function(current, "std::wstring JsonString("))
    source = source.replace("@ORIGIN_STORAGE_KEY@", function(current, "bool OriginStorageKey("))
    body = function(dispatch, "bool Host::ClearOriginData(")
    if args.negative_control:
        guard = "if (!OriginStorageKey(origin, &key)) return false;"
        if body.count(guard) != 1:
            raise ValueError("native admission guard changed; update the negative control")
        body = body.replace(guard, "(void)OriginStorageKey(origin, &key);")
    source = source.replace("@CLEAR_ORIGIN_DATA@", body.replace("ICoreWebView2*", "RecordingCore*"))
    query = function(current, "bool Host::QueryOriginStorage(")
    source = source.replace("@QUERY_ORIGIN_STORAGE@", query.replace("ICoreWebView2*", "RecordingCore*"))
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gesher-webstorage-native-") as temporary:
        cpp = Path(temporary) / "contracts.cc"
        cpp.write_text(source)
        # llvm-mingw's driver symlink name selects its target triple; retain it.
        subprocess.run([str(args.cxx.absolute()), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fno-exceptions", "-static",
                        "-I", str(ROOT / "compat/webview"),
                        "-isystem", str(ROOT / "compat/webview/include"),
                        "-isystem", str(args.sdk_include.resolve()), str(cpp),
                        "-lurlmon", "-loleaut32", "-lole32", "-luuid", "-o", str(output)],
                       check=True, timeout=120)
    print("Built native WebStorage contracts:", output, flush=True)
    if not args.no_run:
        if args.negative_control:
            result = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
            if result.returncode != 1 or "FAIL: direct Host rejects invalid origin" not in result.stderr:
                raise RuntimeError("recording fixture did not reject missing native admission: "
                                   + result.stdout + result.stderr)
            print("WebStorage native negative control: missing admission rejected; no browser used")
        else:
            subprocess.run([str(output)], check=True, timeout=30)


if __name__ == "__main__":
    main()
