#!/usr/bin/env python3
"""Run the AndroidKeyStore user-authentication contracts of ADR 0095 on a host JVM.

The production classes are compiled unchanged, in their own packages, against
stand-ins of the Android boundary (tests/keystore/standins, android-16.0.0_r4
values): the app's provider (AndroidKeyStore, AndroidKeyStoreAuth), the system
server's authorization (KeystoreAuthorization, its binder service and client,
KeyBoundHello) and the backend interfaces. The only test double is the harness's
Windows Hello key store (real RSA keys in memory); JCA provider selection, the
binder glue and all cryptography are the production code's.
Use --source-ref to run the contracts against another revision of the sources.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PRODUCTION = [
    "app/java/com/gesher/app/AndroidKeyStore.java",
    "app/java/com/gesher/app/AndroidKeyStoreAuth.java",
    "app/java/com/gesher/server/HelloKeyBackend.java",
    "app/java/com/gesher/server/KeyBoundHello.java",
    "app/java/com/gesher/server/KeystoreAuthorization.java",
    "app/java/com/gesher/server/KeystoreAuthorizationService.java",
    "app/java/com/gesher/server/KeystoreOperations.java",
    "app/java/com/gesher/server/WindowsHelloBackend.java",
]
STANDINS = ROOT / "tests/keystore/standins"
HARNESS = ROOT / "tests/keystore/KeystoreAuthContracts.java.in"


def read(relative: str, ref: str | None) -> str:
    if ref:
        return subprocess.check_output(["git", "show", ref + ":" + relative], cwd=ROOT, text=True)
    return (ROOT / relative).read_text()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--java-home", default=os.environ.get("JAVA_HOME"))
    args = parser.parse_args()
    executable = lambda name: str(Path(args.java_home) / "bin" / name) if args.java_home else name
    with tempfile.TemporaryDirectory(prefix="gesher-keystore-") as temporary:
        work = Path(temporary)
        sources = []
        for relative in PRODUCTION:
            source = read(relative, args.source_ref)
            print(Path(relative).name, "SHA256", hashlib.sha256(source.encode()).hexdigest(),
                  flush=True)
            target = work / "src" / Path(relative).relative_to("app/java")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(source)
            sources.append(target)
        for standin in sorted(STANDINS.rglob("*.java.in")):
            target = (work / "src" / standin.relative_to(STANDINS)).with_suffix("")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(standin.read_text())
            sources.append(target)
        harness = work / "src/com/gesher/server/KeystoreAuthContracts.java"
        harness.write_text(HARNESS.read_text())
        sources.append(harness)
        classes = work / "classes"
        # AndroidKeyStore predates these contracts with a deprecated Provider constructor
        # and no serialVersionUID; every other lint category is an error.
        subprocess.run([executable("javac"), "-Xlint:all,-deprecation,-serial", "-Werror",
                        "-encoding", "UTF-8",
                        "-d", str(classes)] + [str(s) for s in sources], check=True, timeout=120)
        data = work / "data"
        data.mkdir()
        # The harness reads the Spi JCA selected, as AndroidKeyStoreProvider's
        # getKeyStoreOperationHandle does with libcore's getCurrentSpi().
        subprocess.run([executable("java"),
                        "--add-opens", "java.base/javax.crypto=ALL-UNNAMED",
                        "--add-opens", "java.base/java.security=ALL-UNNAMED",
                        "-cp", str(classes), "com.gesher.server.KeystoreAuthContracts", str(data)],
                       check=True, timeout=300)


if __name__ == "__main__":
    main()
