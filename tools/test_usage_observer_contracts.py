#!/usr/bin/env python3
"""Run the complete usage observer engine and service over explicit Android host boundaries.

The Java service/history/helper are compiled verbatim. Stub Android type declarations
and external permission/identity/intent-sender boundaries do not implement observer
behavior. This does not replace framework.jar/ART or real Binder/PendingIntent testing.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, default=Path(os.environ["JAVA_HOME"]) if "JAVA_HOME" in os.environ else None)
    args = parser.parse_args()
    javac = str(args.java_home / "bin/javac") if args.java_home else shutil.which("javac")
    java = str(args.java_home / "bin/java") if args.java_home else shutil.which("java")
    if not javac or not java:
        parser.error("JDK 21 is required; supply --java-home")
    fixture = ROOT / "tests/activity-usage"
    spec = importlib.util.spec_from_file_location("observer_host_boundaries", fixture / "observer_host_boundaries.py")
    boundaries = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(boundaries)
    service = ROOT / "app/java/com/gesher/server/DesktopUsageStatsService.java"
    service_text = service.read_text()
    # Type-only IUsageStatsManager boundary. All method bodies remain the exact
    # production source; real Android AIDL compatibility is checked by framework CI.
    signatures = re.findall(r"@Override public ([^{]+)\{", service_text)
    if len(signatures) < 30:
        raise ValueError("Usage service declaration boundary changed")
    declarations = "\n".join("public abstract " + signature.strip() + ";" for signature in signatures)
    imports = "\n".join(re.findall(r"^import .+;$", service_text, re.MULTILINE))
    with tempfile.TemporaryDirectory(prefix="gesher-usage-observer-") as directory:
        work = Path(directory)
        sources = []
        for relative, content in boundaries.SOURCES.items():
            path = work / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            sources.append(str(path))
        interface = work / "android/app/usage/IUsageStatsManager.java"
        interface.write_text("package android.app.usage;\n" + imports
                             + "\npublic interface IUsageStatsManager { abstract class Default {\n"
                             + declarations + "\n} }\n")
        sources.append(str(interface))
        for name in ("ActivityUsageHistory", "UsageLimitObservers", "DesktopUsageStatsService"):
            path = ROOT / "app/java/com/gesher/server" / (name + ".java")
            print(str(path.relative_to(ROOT)) + " SHA256 " + hashlib.sha256(path.read_bytes()).hexdigest(), flush=True)
            sources.append(str(path))
        for name in ("UsageLimitObserversTest", "UsageObserverServiceTest"):
            sources.append(str(fixture / (name + ".java")))
        manager = (ROOT / "app/java/com/gesher/server/ActivityTaskManagerService.java").read_text()
        signature = "private void recordUsage(ActivityRecord record, ActivityUsageHistory.State state)"
        if manager.count(signature) != 1:
            raise ValueError("Task-manager usage hook boundary changed")
        start = manager.index(signature)
        end = manager.index("{", start) + 1
        depth = 1
        while depth:
            depth += (manager[end] == "{") - (manager[end] == "}")
            end += 1
        hook = work / "TaskRootUsageHookHost.java"
        template = (fixture / "TaskRootUsageHookHost.java.in").read_text()
        hook.write_text(template.replace("@PRODUCTION_RECORD_USAGE@", manager[start:end]))
        sources.append(str(hook))
        classes = work / "classes"
        subprocess.run([javac, "-encoding", "UTF-8", "-d", str(classes), *sources], check=True, timeout=60)
        for name in ("UsageLimitObserversTest", "UsageObserverServiceTest", "TaskRootUsageHookHost"):
            subprocess.run([java, "-XX:-UsePerfData", "-cp", str(classes), "com.gesher.server." + name],
                           check=True, timeout=60)


if __name__ == "__main__":
    main()
