"""Reuse Linux host generators with exact build inputs and verified cached files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
INPUTS = (
    "aosp.lock", "patches", "tools/host", "tools/host-tools-ci.cmake",
    "tools/framework", "tools/gen", "tools/fetch-aosp.sh", "tools/sparse-bp.sh",
    "tools/aosp_cache.py", ".github/workflows/framework.yml",
    "tools/ci/host_tools_cache.py",
)
FILES = (
    "aidl", "hidl-gen", "stats-log-api-gen", "sysprop_java",
    "expresscatalog-codegen", "protoc-gen-javanano", "protoc-gen-javastream",
    "protobuf/protoc",
)
CACHE = "out/host-tools-cache"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def key(root, system):
    names = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "-z", "--", *INPUTS]).decode().split("\0")
    sources = {}
    for name in filter(None, names):
        path = root / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("Host tool input is not a regular file: " + name)
        sources[name] = digest(path)
    if not all(name in sources for name in
               ("aosp.lock", "tools/host/CMakeLists.txt", ".github/workflows/framework.yml")):
        raise ValueError("Incomplete host tool source checkout")
    payload = json.dumps({"sources": sources, "system": system}, sort_keys=True).encode()
    return "host-generators-v1-" + hashlib.sha256(payload).hexdigest()


def system_identity():
    tools = {}
    for name in ("clang", "clang++", "cmake", "ninja", "bison", "flex"):
        executable = shutil.which(name)
        if executable is None:
            raise ValueError("Missing host compiler dependency: " + name)
        tools[name] = digest(Path(executable))
    # Versions cover libc++, libc, the linker, generated-parser support and headers.
    packages = subprocess.check_output(
        ["dpkg-query", "-W", "-f", "\u0024{Package} \u0024{Version} \u0024{Architecture}\n"],
        text=True)
    environment = {name: os.environ.get(name, "") for name in (
        "CFLAGS", "CXXFLAGS", "LDFLAGS", "CPATH", "CPLUS_INCLUDE_PATH",
        "C_INCLUDE_PATH", "LIBRARY_PATH", "LD_LIBRARY_PATH", "PKG_CONFIG_PATH",
        "CMAKE_PREFIX_PATH",
    )}
    return {"tools": tools, "packages": sorted(packages.splitlines()),
            "environment": environment, "machine": os.uname().machine}


def inventory(folder):
    result = {}
    for path in folder.rglob("*"):
        if path.is_symlink():
            raise ValueError("Symlink in host tool cache")
        if path.is_file():
            name = path.relative_to(folder).as_posix()
            if name not in FILES:
                raise ValueError("Unexpected host tool cache member: " + name)
            result[name] = digest(path)
    if set(result) != set(FILES):
        raise ValueError("Incomplete host tool cache")
    return result


def safe_folder(root, relative):
    folder = root / relative
    if folder.is_symlink() or folder.resolve().relative_to(root.resolve()) != Path(relative):
        raise ValueError("Host tool output path escapes workspace")
    return folder


def restore(root, cache_key, repository):
    cache = safe_folder(root, CACHE)
    receipt_path = cache / "receipt.json"
    if receipt_path.is_symlink():
        raise ValueError("Symlink in host tool receipt")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if (set(receipt) != {"key", "repository", "ref", "source", "run", "files"}
            or receipt["key"] != cache_key or receipt["repository"] != repository
            or receipt["ref"] != "refs/heads/main"
            or not isinstance(receipt["source"], str)
            or not re.fullmatch("[0-9a-f]{40}", receipt["source"])
            or not isinstance(receipt["run"], str)
            or not re.fullmatch("[1-9][0-9]*", receipt["run"])):
        raise ValueError("Host tool cache provenance mismatch")
    files = safe_folder(root, CACHE + "/files")
    if inventory(files) != receipt["files"]:
        raise ValueError("Host tool cache digest mismatch")
    destination = safe_folder(root, "dist-host")
    if destination.exists():
        raise ValueError("Host tool destination already exists")
    with tempfile.TemporaryDirectory(prefix="host-generators-", dir=root / "out") as temporary:
        staged = Path(temporary) / "dist-host"
        shutil.copytree(files, staged)
        staged.rename(destination)
    return receipt


def seal(root, cache_key, environment):
    if environment.get("GITHUB_REF") != "refs/heads/main":
        raise ValueError("Only main publishes reusable host tools")
    source = safe_folder(root, "dist-host")
    files = inventory(source)
    cache = safe_folder(root, CACHE)
    # Discard only our dedicated validated workspace output directory.
    if cache.exists():
        shutil.rmtree(cache)
    cache.mkdir(parents=True)
    shutil.copytree(source, cache / "files")
    receipt = {"key": cache_key, "repository": environment["GITHUB_REPOSITORY"],
               "ref": environment["GITHUB_REF"], "source": environment["GITHUB_SHA"],
               "run": environment["GITHUB_RUN_ID"], "files": files}
    (cache / "receipt.json").write_text(json.dumps(receipt, sort_keys=True), encoding="utf-8")


def output(name, value):
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as stream:
        print(name + "=" + value, file=stream)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("key", "restore", "seal"))
    parser.add_argument("--key")
    args = parser.parse_args()
    if args.operation == "key":
        output("key", key(ROOT, system_identity()))
    elif args.operation == "seal":
        seal(ROOT, args.key, os.environ)
    else:
        try:
            receipt = restore(ROOT, args.key, os.environ["GITHUB_REPOSITORY"])
        except (OSError, ValueError, KeyError) as error:
            print("Host tool cache miss: " + str(error))
            output("hit", "false")
        else:
            print("Verified host generators from main source=" + receipt["source"]
                  + " run=" + receipt["run"])
            output("hit", "true")


if __name__ == "__main__":
    main()
