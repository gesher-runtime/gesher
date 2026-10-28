#!/usr/bin/env python3
"""CI-only source/compiler/role proof for default and query-hook runtimes."""
import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SENTINEL = b"nativeRegisterFixture\0"
HOOK = "dalvikvm-web3-query-test.exe"
SHARED = (
    "compat/accessibility/query_pool.h", "compat/accessibility/query_pool.cc",
    "compat/accessibility/uia_bridge.cc", "compat/accessibility/query_fixture.inc",
    "compat/hwui/window/gesher_window.h", "compat/hwui/window/gesher_window.cc",
    "app/java/com/gesher/app/accessibility/UiaBridge.java",
    "app/java/com/gesher/app/Main.java", "app/java/android/app/GesherAppProcess.java",
    "cmake/webview.cmake", "aosp.lock", "cmake/toolchain-llvm-mingw.cmake",
)
CHANGED = {"compat/accessibility/uia_bridge.cc", "compat/accessibility/query_pool.cc",
           "compat/accessibility/uia_client.cc", "compat/net/windows_proxy.cc"}

def require(condition, message):
    if not condition:
        raise SystemExit(message)

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def context():
    require(os.environ.get("GITHUB_ACTIONS") == "true", "Host-role proof executes only in CI")
    result = {name: os.environ[variable] for name, variable in (
        ("source", "GITHUB_SHA"), ("repository", "GITHUB_REPOSITORY"),
        ("repository_id", "GITHUB_REPOSITORY_ID"), ("run", "GITHUB_RUN_ID"),
        ("attempt", "GITHUB_RUN_ATTEMPT"), ("event", "GITHUB_EVENT_NAME"))}
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    # A PR's actual checkout/compiled source is the merge SHA; API supplier
    # head metadata describes its event head. Pin BOTH, never conflate them.
    if result["event"] == "pull_request":
        result["supplier_head"] = event["pull_request"]["head"]["sha"]
        result["head_repository_id"] = str(event["pull_request"]["head"]["repo"]["id"])
    else:
        result["supplier_head"] = result["source"]
        result["head_repository_id"] = result["repository_id"]
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    require(actual == result["source"], "Actual checkout differs from compiled source SHA")
    require(subprocess.call(["git", "diff", "--quiet", "HEAD", "--"], cwd=ROOT) == 0,
            "Tracked sources changed outside the pinned checkout")
    return result

def cache(build):
    result = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key.rsplit(":", 1)[0]] = value
    return result

def commands(build):
    result = {}
    for item in json.loads((build / "compile_commands.json").read_text(encoding="utf-8")):
        args = item.get("arguments")
        if args is None:
            args = shlex.split(item["command"])
        require("-o" in args, "Compile command has no explicit output")
        output = args[args.index("-o") + 1]
        key = (item["directory"], item["file"], output)
        require(key not in result, "Duplicate compile command output")
        result[key] = args
    require(result, "Missing actual compile commands")
    return result

def compiler(build):
    # The toolchain sets an ordinary variable, so it need not appear in
    # CMakeCache.txt. Use CMake's actual generated compiler record and require
    # all four real CXX command lines to invoke that exact configured driver.
    records = list((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    require(len(records) == 1, "Expected one authoritative generated CXX compiler record")
    metadata = records[0].resolve(strict=True)
    matches = re.findall(r'^\s*set\(CMAKE_CXX_COMPILER\s+"([^"]+)"\)\s*$',
                         metadata.read_text(encoding="utf-8"), flags=re.MULTILINE)
    require(len(matches) == 1, "Missing or ambiguous configured CXX driver")
    configured = Path(matches[0])
    require(configured.is_absolute(), "Configured compiler driver is not absolute")
    path = configured.resolve(strict=True)
    settings = cache(build)
    if "CMAKE_CXX_COMPILER" in settings:
        require(Path(settings["CMAKE_CXX_COMPILER"]).resolve(strict=True) == path,
                "Cached compiler contradicts actual generated compiler")
    launcher = settings.get("CMAKE_CXX_COMPILER_LAUNCHER", "")
    require(";" not in launcher, "Unexpected multi-command compiler launcher")
    launcher_path = None
    if launcher:
        resolved = shutil.which(launcher)
        require(resolved is not None, "Configured compiler launcher cannot be resolved")
        launcher_path = Path(resolved).resolve(strict=True)
    expected = {(ROOT / name).resolve(): name for name in CHANGED}
    seen, forms = set(), set()
    for key, args in commands(build).items():
        source = expected.get(Path(key[1]).resolve())
        if source is None:
            continue
        if args[0] == str(configured):
            # Some CMake versions deliberately export compiler-only commands
            # even when a launcher is configured. The exact absolute compiler
            # still comes from authoritative metadata, never arbitrary argv.
            forms.add("compiler-first")
        else:
            require(launcher_path is not None and len(args) > 1 and args[1] == str(configured),
                    "Actual CXX command does not invoke the generated configured driver: " + source)
            actual_launcher = shutil.which(args[0])
            require(actual_launcher is not None and Path(actual_launcher).resolve(strict=True) == launcher_path,
                    "Actual CXX launcher differs from configured launcher")
            forms.add("configured-launcher-prefix")
        seen.add(source)
    require(seen == CHANGED, "Missing actual query CXX commands for configured compiler proof")
    require(len(forms) == 1, "Inconsistent CMake command export form across query objects")
    return {"path": str(path), "configured_driver": str(configured), "sha256": sha(path),
            "metadata_file": str(metadata), "metadata_sha256": sha(metadata),
            "compile_database_form": forms.pop(),
            "launcher": str(launcher_path) if launcher_path is not None else None,
            "launcher_sha256": sha(launcher_path) if launcher_path is not None else None}

def compiler_preflight(args):
    info = context()
    print(json.dumps({"source": info["source"], "compiler": compiler(Path(args.build)),
                      "actual_query_cpp_commands": sorted(CHANGED)}, sort_keys=True))

def sources():
    return {name: sha(ROOT / name) for name in SHARED}

def installer_excludes_fixture():
    data = (ROOT / "tools/package/gesher.iss").read_text(encoding="utf-8")
    entries = re.findall(r'^\s*Source:\s*"([^"]+)";', data, flags=re.MULTILINE)
    require(entries, "Missing actual installer source entries")
    for name in (HOOK, "web3-query-tests.dex"):
        target = ("{#BuildDir}/" + name).casefold()
        for pattern in entries:
            require(not fnmatch.fnmatchcase(target, pattern.replace("\\", "/").casefold()),
                    "Installer source includes CI fixture: " + pattern)

def capture_production(args):
    info = context()
    build, dist = Path(args.build), Path(args.dist)
    require(cache(build)["GESHER_WEB3_QUERY_TESTS"] == "OFF", "Production role must be explicit OFF")
    executable = dist / "dalvikvm.exe"
    require(SENTINEL not in executable.read_bytes(), "Production runtime contains test-native registration")
    installer_excludes_fixture()
    before = commands(build)
    role = dict(info, role="production", test_hook_compiled=False, compiler=compiler(build),
                shared_sources=sources(), executable="dalvikvm.exe", executable_sha256=sha(executable),
                executable_map_sha256=sha(dist / "dalvikvm.map"))
    (dist / "web3-production-role.json").write_text(json.dumps(role, indent=2) + "\n", encoding="utf-8")
    state = {"role": role, "commands": [[list(key), value] for key, value in before.items()]}
    Path(args.state).write_text(json.dumps(state) + "\n", encoding="utf-8")
    print("ACTUAL_WEB3_PRODUCTION_ROLE " + json.dumps(role, sort_keys=True))

def capture_hook(args):
    info = context()
    build, dist, out = Path(args.build), Path(args.dist), Path(args.out)
    require(cache(build)["GESHER_WEB3_QUERY_TESTS"] == "ON", "Hook role must be explicit ON")
    saved = json.loads(Path(args.state).read_text(encoding="utf-8"))
    production = saved["role"]
    require(all(production[key] == value for key, value in info.items()), "Different source/run/attempt for two roles")
    require(production["compiler"] == compiler(build), "Different compiler binary for two roles")
    require(production["shared_sources"] == sources(), "Production query source changed between builds")
    require(sha(dist / "dalvikvm.exe") == production["executable_sha256"], "Published production executable changed")
    before = {tuple(key): value for key, value in saved["commands"]}
    after = commands(build)
    require(before.keys() == after.keys(), "Build graph changed beyond the test definition")
    changed = set()
    for key, old in before.items():
        new = after[key]
        normalized = [value for value in new if value != "-DGESHER_WEB3_QUERY_TESTS=1"]
        require(old == normalized, "Other compiler flags changed: " + key[1])
        if old != new:
            changed.add(Path(key[1]).resolve().relative_to(ROOT).as_posix())
    require(changed == CHANGED, "Unexpected test-definition compilation boundary: " + repr(changed))
    require(SENTINEL in (build / "dalvikvm.exe").read_bytes(), "Hook executable lacks test-native registration")
    out.mkdir(exist_ok=True)
    shutil.copyfile(build / "dalvikvm.exe", out / HOOK)
    shutil.copyfile(build / "dalvikvm.map", out / "dalvikvm-web3-query-test.map")
    role = dict(info, role="query-hook", test_hook_compiled=True, compiler=compiler(build),
                shared_sources=sources(), executable=HOOK, executable_sha256=sha(out / HOOK),
                executable_map_sha256=sha(out / "dalvikvm-web3-query-test.map"),
                production_runtime_sha256=production["executable_sha256"],
                changed_compile_definitions=sorted(changed))
    (out / "web3-query-hook-role.json").write_text(json.dumps(role, indent=2) + "\n", encoding="utf-8")
    print("ACTUAL_WEB3_QUERY_HOOK_ROLE " + json.dumps(role, sort_keys=True))

def verify(args):
    info = context()
    dist = Path(args.dist)
    production = json.loads((dist / "web3-production-role.json").read_text(encoding="utf-8"))
    require(all(production[key] == value for key, value in info.items()), "Production supplier context mismatch")
    require(production["role"] == "production" and production["test_hook_compiled"] is False,
            "Expected default-OFF production role")
    require(sha(dist / "dalvikvm.exe") == production["executable_sha256"], "Production executable copy mismatch")
    require(SENTINEL not in (dist / "dalvikvm.exe").read_bytes(), "Test registration in normal runtime")
    require(sha(dist / "dalvikvm.map") == production["executable_map_sha256"], "Production exact map copy mismatch")
    installer_excludes_fixture()
    if args.hook:
        hook = json.loads((dist / "web3-query-hook-role.json").read_text(encoding="utf-8"))
        require(all(hook[key] == value for key, value in info.items()), "Hook supplier context mismatch")
        require(hook["role"] == "query-hook" and hook["test_hook_compiled"] is True, "Expected hook-only runtime role")
        require(hook["executable"] == HOOK and sha(dist / HOOK) == hook["executable_sha256"], "Hook executable copy mismatch")
        require(SENTINEL in (dist / HOOK).read_bytes(), "Missing actual test registration in hook runtime")
        require(sha(dist / "dalvikvm-web3-query-test.map") == hook["executable_map_sha256"], "Hook exact map copy mismatch")
        require(hook["compiler"] == production["compiler"] and hook["shared_sources"] == production["shared_sources"] == sources(),
                "Different compiler or production code for query-hook runtime")
        require(hook["production_runtime_sha256"] == production["executable_sha256"] and
                set(hook["changed_compile_definitions"]) == CHANGED, "Different role boundary")
        print("VERIFIED_WEB3_FULL_ROLES source=" + info["source"] + " production=" + production["executable_sha256"] +
              " hook=" + hook["executable_sha256"] + " shared-source-identical=True compiler-identical=True")
    else:
        require(not (dist / HOOK).exists(), "Normal suite/package must not consume query-hook artifact")
        print("VERIFIED_WEB3_PRODUCTION_ONLY source=" + info["source"] + " exe=" + production["executable_sha256"])

def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("capture-production", "capture-hook"):
        item = sub.add_parser(name)
        item.add_argument("--build", required=True)
        item.add_argument("--dist", required=True)
        item.add_argument("--state", required=True)
        if name == "capture-hook": item.add_argument("--out", required=True)
    item = sub.add_parser("compiler-preflight")
    item.add_argument("--build", required=True)
    item = sub.add_parser("verify")
    item.add_argument("--dist", required=True)
    item.add_argument("--hook", action="store_true")
    args = parser.parse_args()
    {"capture-production": capture_production, "capture-hook": capture_hook,
     "compiler-preflight": compiler_preflight, "verify": verify}[args.command](args)

if __name__ == "__main__":
    main()
