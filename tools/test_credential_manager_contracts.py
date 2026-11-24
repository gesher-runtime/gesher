#!/usr/bin/env python3
"""Run the production Credential Manager (ICredentialManager) passkey contracts on a host JVM.

The Android boundary is stand-ins with android-16.0.0_r4's and androidx.credentials'
constant values; CredentialManagerService, PasskeyRequests, PasskeyJson, AssetLinks,
PasskeyBackend and WindowsWebAuthn are compiled unchanged apart from package, imports and class
visibility (docs/passkeys.md, ADR 0093). Tests cover AOSP's answers without a provider
(production without Windows' WebAuthn API) and, with an isolated software authenticator
and a local asset-links server only, request and response mapping, the security refusals,
Windows' errors, cancellation, client death and concurrent requests.
Use --source-ref to run the contracts against another revision of the sources.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = "app/java/com/gesher/server/"
SOURCES = {
    "// PRODUCTION_JSON": (SERVER + "PasskeyJson.java", "final class PasskeyJson", None),
    "// PRODUCTION_BACKEND": (SERVER + "PasskeyBackend.java",
                              "public interface PasskeyBackend", "interface PasskeyBackend"),
    "// PRODUCTION_NATIVE": (SERVER + "WindowsWebAuthn.java", "final class WindowsWebAuthn", None),
    "// PRODUCTION_REQUESTS": (SERVER + "PasskeyRequests.java", "final class PasskeyRequests", None),
    "// PRODUCTION_ASSET_LINKS": (SERVER + "AssetLinks.java", "final class AssetLinks", None),
    "// PRODUCTION_SERVICE": (SERVER + "CredentialManagerService.java",
                              "public final class CredentialManagerService",
                              "final class CredentialManagerService"),
}


def read(relative: str, ref: str | None) -> str:
    if ref:
        return subprocess.check_output(["git", "show", ref + ":" + relative], cwd=ROOT, text=True)
    return (ROOT / relative).read_text()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--java-home", default=os.environ.get("JAVA_HOME"))
    args = parser.parse_args()
    harness = (ROOT / "tests/credentials/CredentialManagerContracts.java.in").read_text()
    for marker, (relative, declaration, replacement) in SOURCES.items():
        source = read(relative, args.source_ref)
        print(Path(relative).name, "SHA256", hashlib.sha256(source.encode()).hexdigest(), flush=True)
        # The harness declares the imports once; Android types are its stand-ins.
        source = re.sub(r"^package .*;\n|^import .*;\n", "", source, flags=re.MULTILINE)
        if declaration not in source:
            raise SystemExit(f"{relative}: expected '{declaration}'")
        if replacement:
            source = source.replace(declaration, replacement, 1)
        if marker not in harness:
            raise SystemExit(f"harness: expected '{marker}'")
        harness = harness.replace(marker, source)
    executable = lambda name: str(Path(args.java_home) / "bin" / name) if args.java_home else name
    with tempfile.TemporaryDirectory(prefix="gesher-credentials-") as temporary:
        work = Path(temporary)
        java = work / "CredentialManagerContracts.java"
        java.write_text(harness)
        subprocess.run([executable("javac"), "-Xlint:all", "-Werror", "-d", str(work), str(java)],
                       check=True, timeout=120)
        subprocess.run([executable("java"), "-cp", str(work), "CredentialManagerContracts"],
                       check=True, timeout=120)


if __name__ == "__main__":
    main()
