#!/usr/bin/env python3
"""Check the actual speech APK boundary after R8, using production Java sources.

The APK backends compile separately against the original API classes, just as
framework.yml uses gesher-app-classes.jar. Compare that public API with the
definitions in the optimized DEX, including nested classes, descriptors,
fields, and static/instance dispatch. This is a bytecode contract check; it
does not replace the Windows speech or APK lifecycle tests.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import java_member_inventory as inventory


def run(command: list[str]) -> None:
    result = subprocess.run(command, text=True, capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(
            f"Command failed ({result.returncode}): {' '.join(command)}\n"
            + result.stdout + result.stderr
        )


def pack_classes(directory: Path, destination: Path) -> None:
    with zipfile.ZipFile(destination, "w") as archive:
        for path in sorted(directory.rglob("*.class")):
            archive.write(path, path.relative_to(directory).as_posix())


def public_api(directory: Path) -> dict[str, inventory.Class]:
    result = {}
    for path in directory.rglob("*.class"):
        item = inventory.class_members(path.read_bytes())
        if item.flags & inventory.PUBLIC:
            result[item.name] = item
    if not result:
        raise AssertionError("No production speech API classes were compiled")
    return result


def dex_api(archive_path: Path) -> dict[str, inventory.Class]:
    result = {}
    with zipfile.ZipFile(archive_path) as archive:
        for name in archive.namelist():
            if name.endswith(".dex"):
                classes = inventory.dex_members(archive.read(name))
                if result.keys() & classes.keys():
                    raise AssertionError("Duplicate class definitions in optimized DEX")
                result.update(classes)
    if not result:
        raise AssertionError("R8 did not emit any class definitions")
    return result


def check_api(expected: dict[str, inventory.Class], actual: dict[str, inventory.Class]) -> int:
    errors = []
    members = 0
    for name, source in sorted(expected.items()):
        optimized = actual.get(name)
        if optimized is None:
            errors.append(f"missing/renamed class: {name}")
            continue
        if not optimized.flags & inventory.PUBLIC:
            errors.append(f"class is no longer public: {name}")
        for key, member in sorted(source.members.items()):
            if not member.flags & (inventory.PUBLIC | inventory.PROTECTED):
                continue
            members += 1
            emitted = optimized.members.get(key)
            label = f"{name}.{member.name}{member.descriptor}"
            if emitted is None:
                errors.append(f"missing/renamed {member.kind}: {label}")
                continue
            if (member.flags & inventory.PUBLIC and not emitted.flags & inventory.PUBLIC
                    or member.flags & inventory.PROTECTED
                    and not emitted.flags & (inventory.PUBLIC | inventory.PROTECTED)):
                errors.append(f"access narrowed: {label}")
            if bool(member.flags & inventory.STATIC) != bool(emitted.flags & inventory.STATIC):
                errors.append(f"static/instance dispatch changed: {label}")
    if errors:
        raise AssertionError("R8 broke the separate speech APK API:\n" + "\n".join(errors))
    return members


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--r8", type=Path, required=True)
    parser.add_argument("--rules", type=Path, default=ROOT / "app/proguard-rules.pro",
                        help="Override only for testing an earlier or candidate rules file")
    args = parser.parse_args()
    java_home = args.java_home.resolve()
    executable = ".exe" if os.name == "nt" else ""
    java = str(java_home / f"bin/java{executable}")
    javac = str(java_home / f"bin/javac{executable}")
    r8 = str(args.r8.resolve())
    rules = str(args.rules.resolve())
    sources = [ROOT / f"app/java/com/gesher/server/{name}.java"
               for name in ("WindowsSpeech", "WindowsSpeechRecognition")]
    backends = [ROOT / "app" / app / "java/com/gesher" / package / f"{name}.java"
                for app, package, names in (
                    ("gesher-tts", "tts", ("SpeechBackend", "WindowsSpeechBackend")),
                    ("gesher-stt", "stt", ("RecognizerBackend", "WindowsRecognizerBackend")))
                for name in names]
    with tempfile.TemporaryDirectory(prefix="gesher-r8-speech-") as temporary:
        work = Path(temporary)
        api_classes = work / "api-classes"
        consumers = work / "consumers"
        api_classes.mkdir()
        consumers.mkdir()
        compile_args = [javac, "-source", "17", "-target", "17", "-Xlint:-options",
                        "-encoding", "UTF-8", "-XDstringConcat=inline"]
        run(compile_args + ["-d", str(api_classes), *map(str, sources)])
        api_jar = work / "compile-api.jar"
        pack_classes(api_classes, api_jar)
        # Keep the APK code out of the framework R8 program. Feeding it into
        # that program would hide the real separately compiled ABI regression.
        run(compile_args + ["-cp", str(api_jar), "-d", str(consumers), *map(str, backends)])
        consumer_jar = work / "consumers.jar"
        pack_classes(consumers, consumer_jar)
        run([java, "-cp", r8, "com.android.tools.r8.D8", "--release", "--min-api", "36",
             "--lib", str(java_home), "--classpath", str(api_jar),
             "--output", str(work / "consumers-dex.jar"), str(consumer_jar)])
        optimized = work / "optimized.jar"
        run([java, "-cp", r8, "com.android.tools.r8.R8", "--release", "--min-api", "36",
             "--lib", str(java_home), "--pg-conf", rules,
             "--output", str(optimized), str(api_jar)])
        expected = public_api(api_classes)
        count = check_api(expected, dex_api(optimized))
        print(f"PASS: {len(expected)} production speech API classes and {count} public/protected "
              "members survive R8; four real APK sources compile separately against the original API")


if __name__ == "__main__":
    main()
