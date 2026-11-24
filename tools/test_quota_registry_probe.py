#!/usr/bin/env python3
"""Test the exact quota probe evidence gates; never supplies a browser inventory.

The host inputs are explicitly isolated. Actual support is established only by
the separate --quota-registry Windows run. Two mutation controls prove omitted
origins and acknowledged-but-incomplete deletion cannot pass the evidence gate.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def section(text, first, last):
    return text[text.index(first):text.index(last, text.index(first))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    helper = (root / "probes/webview/browser_session_probe.h").read_text()
    probe = (root / "probes/webview/quota_registry_probe.h").read_text()
    template = (root / "tests/native/quota_registry_probe_host.cc.in").read_text()
    reader = section(helper, "constexpr size_t kMaxJson", "inline std::wstring Quote")
    inventory = section(probe, "struct Observation", "struct State")
    variants = {"current": inventory,
                "missing-origin-control": inventory.replace(
                    "for (const auto& item : positive) if (totals[item.key] == 0) return false;",
                    "// Negative control: the required-origin check is removed."),
                "incomplete-delete-control": inventory.replace(
                    "for (const auto& item : removed) if (totals[item.key] != 0) return false;",
                    "// Negative control: the removal check is removed.")}
    assert all(value != inventory for key, value in variants.items() if key != "current"), "mutation did not apply"
    with tempfile.TemporaryDirectory(prefix="gesher-quota-registry-") as temp:
        work = Path(temp)
        for name, body in variants.items():
            source = work / (name + ".cc")
            executable = work / name
            source.write_text(template.replace("@JSON_READER@", reader).replace("@INVENTORY@", body))
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g", "-O1"]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command + [str(source), "-o", str(executable)], check=True, timeout=60)
            result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=15)
            print(name + ": " + result.stdout.strip(), flush=True)
            if name == "current":
                if result.returncode:
                    raise AssertionError(result.stderr)
            else:
                required = ("missing persisted origin rejected" if name == "missing-origin-control"
                            else "first-party-only deletion rejected")
                if result.returncode != 1 or required not in result.stderr:
                    raise AssertionError(f"{name} did not reject its intended defect: {result.returncode}\n{result.stderr}")
                print(name + ": intended regression rejected", flush=True)
        # Parse the exact embedded fixture script too. This does not simulate
        # IndexedDB, origins or WebView2; all of those require Windows execution.
        node = shutil.which("node")
        if not node:
            raise RuntimeError("node is required to syntax-check the embedded browser fixture")
        page = section(probe, 'R"HTML(<!doctype html>', 'inline bool Start(')
        script = page.split("<script>\n", 1)[1].split("</script>", 1)[0]
        script = script.replace(")HTML\") + expected + R\"HTML(", "https://syntax-only.invalid")
        fixture = work / "storage-fixture.js"
        fixture.write_text(script)
        subprocess.run([node, "--check", str(fixture)], check=True, timeout=15)
        registry = section(probe, 'static const wchar_t* script = LR"JS(', ')JS";')
        registry = registry.split('LR"JS(', 1)[1]
        fixture.write_text(registry)
        subprocess.run([node, "--check", str(fixture)], check=True, timeout=15)
        print("Embedded browser fixture and live registry script: syntax passed", flush=True)


if __name__ == "__main__":
    main()
