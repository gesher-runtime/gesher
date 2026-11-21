#!/usr/bin/env python3
"""Freeze an owned Android-native-Parcel record-format probe; never executes ADB.

Unchanged production NotificationStore/NotificationIdentity plus the shared
migration corpus. All service boundaries are named fixtures. Android classes
come exclusively from the device boot class path.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "framework-classes", "core-classes", "d8-jar", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--normal-only", action="store_true",
                        help="Package only genuine valid v4/v5 compatibility cases, with no malformed inputs")
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    source = output / "source"; source.mkdir()
    classes = output / "classes"; classes.mkdir()
    dex = output / "dex"; dex.mkdir()
    inputs = [ROOT / "app/java/com/gesher/server/NotificationStore.java",
              ROOT / "app/java/com/gesher/server/NotificationIdentity.java"]
    entrypoint = "NotificationStoreNormalReference" if args.normal_only else "NotificationStoreReference"
    if not args.normal_only:
        inputs.append(ROOT / "tests/dex-services/com/gesher/server/NotificationStoreFormatDevice.java")
    inputs.append(ROOT / ("tests/notification-identity/android-reference/" + entrypoint + ".java"))
    manifest = {"scope": "Native Android Parcel parser/writer and record-format qualification only",
                "windows_executed": False, "android_executed": False,
                "service_seams": ["owned data directory", "supplied package/code identity", "protected-mode flag",
                                  "logging", "PendingIntent lookup/creation/activation explicitly forbidden"],
                "production_sources": {}, "sources": {}}
    manifest["normal_formats_only"] = args.normal_only
    for item in inputs:
        copied = source / item.name; shutil.copyfile(item, copied)
        manifest["sources"][str(item.relative_to(ROOT))] = {"sha256": sha(copied), "frozen": str(copied)}
        if "/app/java/" in str(item): manifest["production_sources"][str(item.relative_to(ROOT))] = sha(copied)
    service_path = ROOT / "app/java/com/gesher/server/NotificationManagerService.java"
    service = service_path.read_text()
    start = service.index("    static String toastTag(String tag, int id) {")
    end = service.index("    private int removeWindowsToast(", start)
    methods = service[start:end]
    if methods.count("static ") != 3: raise RuntimeError("Unexpected production identity method boundaries")
    template_path = ROOT / "tests/notification-identity/android-reference/ReferenceServices.java.in"
    generated = source / "ReferenceServices.java"
    generated.write_text(template_path.read_text().replace("// @@TOAST_IDENTITY_METHODS@@", methods))
    manifest["identity_methods"] = {"source": str(service_path.relative_to(ROOT)), "source_sha256": sha(service_path),
                                    "excerpt_sha256": hashlib.sha256(methods.encode()).hexdigest(),
                                    "template_sha256": sha(template_path), "generated_sha256": sha(generated)}
    manifest["builder_sha256"] = sha(Path(__file__))
    commands = []
    def run(command: list[str], name: str) -> None:
        commands.append(command)
        result = subprocess.run(command, capture_output=True)
        (output / (name + ".log")).write_bytes(result.stdout + result.stderr)
        if result.returncode: raise RuntimeError(f"{name} failed; see {output / (name + '.log')}")
    java, javac, jar = (str(args.java_home.resolve() / "bin" / name) for name in ("java", "javac", "jar"))
    cp = os.pathsep.join(str(path.resolve()) for path in (args.framework_classes, args.core_classes))
    run([javac, "-source", "17", "-target", "17", "-encoding", "UTF-8", "-XDstringConcat=inline",
         "-cp", cp, "-d", str(classes), *map(str, sorted(source.glob("*.java")))], "javac")
    providers = sorted(str(path.relative_to(classes)) for path in classes.rglob("*.class"))
    if not providers or any(not name.startswith("com/gesher/server/") for name in providers):
        raise RuntimeError("Probe must not include any Android, Java or third-party provider")
    manifest["compiled_providers"] = providers
    archive = output / "probe-classes.jar"
    run([jar, "cf", str(archive), "-C", str(classes), "."], "jar")
    run([java, "-cp", str(args.d8_jar.resolve()), "com.android.tools.r8.D8", "--release", "--min-api", "36",
         "--output", str(dex), str(archive)], "d8")
    payload = dex / "classes.dex"; digest = sha(payload)
    manifest["payload"] = {"path": str(payload), "sha256": digest, "bytes": payload.stat().st_size}
    manifest["toolchain"] = {"javac": subprocess.check_output([javac, "-version"], text=True).strip(),
                             "d8_jar": str(args.d8_jar.resolve()), "d8_sha256": sha(args.d8_jar)}
    (output / "build-commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    manifest_path = output / "manifest.json"; manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    remote = "/data/local/tmp/gesher-notification-format-" + digest[:12]
    setup = {"description": "Owned native-Parcel notification store probe setup only", "manifest_sha256": sha(manifest_path),
             "steps": [{"adb": ["push", str(payload), remote + ".dex"], "timeout": 90},
                       {"adb": ["shell", "chmod", "0444", remote + ".dex"], "timeout": 30},
                       {"adb": ["shell", "sha256sum", remote + ".dex"], "timeout": 30}]}
    execute = {"description": "Frozen production notification record parser/writer on native Android Parcel",
               "scope": "app_process native Parcel bytes only; no APK permission or Windows toast qualification",
               "device_dex_sha256": digest, "manifest_sha256": sha(manifest_path),
               "steps": [{"adb": ["shell", "env", "CLASSPATH=" + remote + ".dex", "app_process", "/system/bin",
                                  "com.gesher.server." + entrypoint, phase, remote],
                          "timeout": 90, "allow_failure": False} for phase in ("write", "read")]}
    for name, value in (("setup-request.json", setup), ("execute-request.json", execute)):
        (output / name).write_text(json.dumps(value, indent=2) + "\n")
    for path in [*source.iterdir(), payload, manifest_path, output / "build-commands.json",
                 output / "setup-request.json", output / "execute-request.json"]: path.chmod(0o444)
    print(json.dumps({"manifest": str(manifest_path), "manifest_sha256": sha(manifest_path),
                      "payload_sha256": digest, "compiled_provider_count": len(providers)}, indent=2))


if __name__ == "__main__": main()
