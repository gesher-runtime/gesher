"""Small source-bound Windows xattr gate; compile actual production POSIX code."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
COMPILER_SHA256 = "5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4"
INPUTS = [
    {
        "path": "bionic/libc/kernel/uapi/linux/android/binder.h",
        "url": "https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/kernel/uapi/linux/android/binder.h?format=TEXT",
        "sha256": "ccdccf5df0f7aa3006b465fc774738fab7b351fcde5025e035ce569237fceca7",
    },
    {
        "path": "external/icu/icu4c/source/data/misc/windowsZones.txt",
        "url": "https://android.googlesource.com/platform/external/icu/+/042af546d67c5dde78d6e7c43d17e6e219462409/icu4c/source/data/misc/windowsZones.txt?format=TEXT",
        "sha256": "eb376f901e8cf07b3e6f1c674d04968d9994c610c7bad72e30cff7444b4dc5f9",
    },
]
SCOPES = [
    "compat/posix", "compat/bionic/extra_sys_bionic.cc", "compat/bionic/syscall_bionic.cc",
    "compat/arm64/dll/guest_libc.cc", "tests/xattr", "tests/native/xattr_fd_host_test.cc",
    "tests/native/xattr_fd_contract_test.inc", "tests/native/xattr_fd_contract_test.c",
    "tests/native/filesystem_contract_guest.c", "tests/windows/run-xattr-fd-component.ps1",
    ".github/workflows/xattr-fd-component.yml",
]
PAYLOADS = {
    "xattr_fd_host_x86_64.exe", "xattr_fd_host_aarch64.exe",
    "compile-x86_64.json", "compile-aarch64.json", "linux-reference.log", "security-policy.log", "inputs.json",
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def identity():
    if git("status", "--porcelain", "--untracked-files=no"):
        raise RuntimeError("Tracked source must be clean at the build and verification boundaries")
    checkout = git("rev-parse", "HEAD")
    expected = os.environ.get("GITHUB_SHA", checkout)
    if checkout != expected:
        raise RuntimeError("Checkout does not match the current workflow SHA")
    files = git("ls-files", "--", *SCOPES).splitlines()
    return {
        "checkout": checkout,
        "tree": git("rev-parse", "HEAD^{tree}"),
        "repository": os.environ.get("GITHUB_REPOSITORY", "local"),
        "run": os.environ.get("GITHUB_RUN_ID", "local"),
        "attempt": os.environ.get("GITHUB_RUN_ATTEMPT", "local"),
        "source_files": {name: sha((ROOT / name).read_bytes()) for name in files},
    }


def verify_inputs(directory):
    for item in INPUTS:
        if sha((directory / item["path"]).read_bytes()) != item["sha256"]:
            raise RuntimeError(f"Wrong source prerequisite: {item['path']}")


def fetch(directory):
    for item in INPUTS:
        destination = directory / item["path"]
        with urllib.request.urlopen(item["url"], timeout=60) as response:
            data = base64.b64decode(response.read(), validate=True)
        if sha(data) != item["sha256"]:
            raise RuntimeError(f"Source prerequisite digest mismatch: {item['path']}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    verify_inputs(directory)
    (directory / "inputs.json").write_text(json.dumps(INPUTS, indent=2) + "\n")


def seal(work, compiler_archive, compiler):
    verify_inputs(work / "aosp")
    if sha(compiler_archive.read_bytes()) != COMPILER_SHA256:
        raise RuntimeError("Compiler archive mismatch")
    payload = work / "payload"
    payload.mkdir(exist_ok=True)
    for arch in ("x86_64", "aarch64"):
        build = work / f"build-{arch}"
        commands = json.loads((build / "compile_commands.json").read_text())
        source = (ROOT / "compat/posix/src/xattr.cc").resolve()
        matches = [entry for entry in commands if Path(entry["file"]).resolve() == source]
        if len(matches) != 1 or "-Werror" not in matches[0].get("command", ""):
            raise RuntimeError(f"Missing current production xattr compile on {arch}")
        shutil.copyfile(build / "xattr_fd_host_test.exe", payload / f"xattr_fd_host_{arch}.exe")
        shutil.copyfile(build / "compile_commands.json", payload / f"compile-{arch}.json")
    shutil.copyfile(work / "linux-reference.log", payload / "linux-reference.log")
    shutil.copyfile(work / "security-policy.log", payload / "security-policy.log")
    shutil.copyfile(work / "aosp/inputs.json", payload / "inputs.json")
    receipt = {
        "format": 1,
        "identity": identity(),
        "compiler_archive_sha256": COMPILER_SHA256,
        "compiler_version": subprocess.check_output([str(compiler), "--version"], text=True),
        "compiler_binary_sha256": sha(compiler.resolve().read_bytes()),
        "payload": {name: sha((payload / name).read_bytes()) for name in sorted(PAYLOADS)},
    }
    (payload / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    verify(payload)


def verify(payload):
    receipt = json.loads((payload / "receipt.json").read_text())
    if receipt["format"] != 1 or receipt["identity"] != identity():
        raise RuntimeError("Source/run/attempt identity mismatch")
    if receipt["compiler_archive_sha256"] != COMPILER_SHA256:
        raise RuntimeError("Unrecognized compiler supplier")
    if set(receipt["payload"]) != PAYLOADS or {p.name for p in payload.iterdir()} != PAYLOADS | {"receipt.json"}:
        raise RuntimeError("Unexpected or missing artifact member")
    for name, digest in receipt["payload"].items():
        if sha((payload / name).read_bytes()) != digest:
            raise RuntimeError(f"Artifact payload mismatch: {name}")
    if json.loads((payload / "inputs.json").read_text()) != INPUTS:
        raise RuntimeError("AOSP source prerequisite identity mismatch")
    for arch, machine in (("x86_64", 0x8664), ("aarch64", 0xAA64)):
        binary = (payload / f"xattr_fd_host_{arch}.exe").read_bytes()
        offset = struct.unpack_from("<I", binary, 0x3C)[0]
        if binary[:2] != b"MZ" or binary[offset:offset+4] != b"PE\0\0" or struct.unpack_from("<H", binary, offset+4)[0] != machine:
            raise RuntimeError(f"Wrong executable architecture: {arch}")
    print("XATTR_COMPONENT_VERIFIED " + json.dumps({k: v for k, v in receipt["identity"].items() if k != "source_files"}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("fetch", "seal", "verify"))
    parser.add_argument("directory", type=Path)
    parser.add_argument("--compiler-archive", type=Path)
    parser.add_argument("--compiler", type=Path)
    args = parser.parse_args()
    if args.action == "fetch":
        fetch(args.directory)
    elif args.action == "seal":
        if args.compiler_archive is None or args.compiler is None:
            parser.error("seal requires --compiler-archive and --compiler")
        seal(args.directory, args.compiler_archive, args.compiler)
    else:
        verify(args.directory)
