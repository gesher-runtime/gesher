#!/usr/bin/env python3
"""Run production bionic key APIs and exit rounds with real host pthread TLS.

The Windows thread-record boundary is modeled; key state, API registration,
cleanup order and host-exit arming are extracted verbatim. This does not replace
the Windows bionic_key_exit_test integration gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("production key section boundaries changed: " + first)
    return source[source.index(first):source.index(following, source.index(first))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/pthread_bionic.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--reference", action="store_true",
                        help="Observe the old read-lock behavior without enforcing the new work budget")
    parser.add_argument("--reference-set", action="store_true",
                        help="Observe the #384 locked setter while retaining its getter progress gate")
    parser.add_argument("--case", choices=("public", "mutations", "exit_order", "host_exit",
                        "generation_limit", "readers", "read_progress", "writers", "set_arming", "set_progress"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--negative-controls", action="store_true")
    args = parser.parse_args()
    if args.negative_controls and (args.reference or args.reference_set or args.case):
        parser.error("negative controls require the complete current-source contract gate")
    source = args.source.read_text(encoding="utf-8")
    parts = {
        "@KEY_STATE_AND_EXIT@": section(source, "constexpr int kKeysMax", "// --- threads"),
        "@KEY_HOST_EXIT@": section(source, "pthread_key_t g_host_exit_key;", "struct StartArg"),
        "@KEY_API@": section(source, "int BIONIC_SYSV gesher_bionic_pthread_key_create(",
                            "int BIONIC_SYSV gesher_bionic_register_atfork("),
    }
    registrations = []
    for name in ("pthread_key_create", "pthread_key_delete", "pthread_getspecific", "pthread_setspecific"):
        found = re.findall(r'^  P\(' + name + r'\);$', source, re.M)
        if len(found) != 1:
            raise ValueError("missing or ambiguous public key registration: " + name)
        registrations += found
    parts["@KEY_REGISTRATIONS@"] = "\n".join(registrations)
    template = (ROOT / "tests/native/pthread_key_contract_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in parts.items():
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    print("Production pthread source SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-bionic-keys-") as temporary:
        folder = args.output_dir.resolve() if args.output_dir else Path(temporary)
        folder.mkdir(parents=True, exist_ok=True)
        generated = folder / "pthread_key_test.cc"
        executable = folder / "pthread_key_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-fno-builtin", "-pthread", "-I", str(ROOT / "compat/bionic"),
            "-I", str(ROOT / "compat/arm64/include"), "-I", str(ROOT / "tests/native"),
            str(generated), "-o", str(executable),
        ]
        if "uint64_t g_key_generations[kKeysMax]" in source:
            command += ["-DHAVE_KEY_GENERATIONS"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        receipt = {"source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                   "generated_sha256": hashlib.sha256(template.encode()).hexdigest(),
                   "template_sha256": hashlib.sha256((ROOT / "tests/native/pthread_key_contract_host_test.cc.in").read_bytes()).hexdigest(),
                   "public_fixture_sha256": hashlib.sha256((ROOT / "tests/native/pthread_key_contract_test.inc").read_bytes()).hexdigest(),
                   "command": command, "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                   "reference": args.reference, "reference_set": args.reference_set}
        (folder / "source.json").write_text(json.dumps(receipt, indent=2) + "\n")
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        failed = []
        cases = [args.case] if args.case else ["public", "mutations", "exit_order", "host_exit", "generation_limit", "readers",
                     "observe_read_progress" if args.reference else "read_progress", "writers", "set_arming",
                     "observe_set_progress" if args.reference or args.reference_set else "set_progress"]
        for case in cases:
            result = subprocess.run([str(executable), case], env=env, cwd=folder, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            (folder / (case + ".log")).write_text(result.stdout + result.stderr)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        if failed:
            return 1
        overlap_template = None
        if not (args.case or args.reference or args.reference_set):
            prefix = ("int BIONIC_SYSV gesher_bionic_pthread_setspecific(bionic_pthread_key_t key, const void* value) {\n"
                      "  if (key < 0 || key >= kKeysMax) return kEINVAL;\n"
                      "  const uint64_t generation = g_key_live_generations[key].load(std::memory_order_acquire);")
            if template.count(prefix) != 1:
                raise ValueError("setter snapshot injection boundary changed")
            overlap_template = template.replace(prefix, prefix + "\n  ObserveSetSnapshot();")
            overlap_source = folder / "set-snapshot.cc"
            overlap_executable = folder / "set-snapshot"
            overlap_source.write_text(overlap_template)
            overlap_command = [str(overlap_source) if item == str(generated) else
                               str(overlap_executable) if item == str(executable) else item for item in command]
            overlap_command += ["-DKEY_SET_SNAPSHOT_PROBE"]
            subprocess.run(overlap_command, check=True, timeout=120)
            result = subprocess.run([str(overlap_executable), "set_snapshot"], env=env, cwd=folder, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            (folder / "set-snapshot.log").write_text(result.stdout + result.stderr)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                print(result.stderr, flush=True)
                return 1
            (folder / "set-snapshot.json").write_text(json.dumps({
                "boundary": "actual generation load followed by an injected scheduling barrier; no ordinary-source hook",
                "insertion_after": prefix, "command": overlap_command,
                "generated_sha256": hashlib.sha256(overlap_template.encode()).hexdigest(),
                "executable_sha256": hashlib.sha256(overlap_executable.read_bytes()).hexdigest()}, indent=2) + "\n")
            print("PASS set_snapshot", flush=True)
        if args.negative_controls:
            mutants = [
                ("global-read-lock", "void* BIONIC_SYSV gesher_bionic_pthread_getspecific(bionic_pthread_key_t key) {",
                 "void* BIONIC_SYSV gesher_bionic_pthread_getspecific(bionic_pthread_key_t key) {\n  std::lock_guard<std::mutex> lock(g_keys_mutex);",
                 "read_progress", "pthread keys: local read proceeds while key registry is locked"),
                ("stale-generation", "generation == 0 || t_key_generations[key] != generation", "generation == 0",
                 "public", "pthread keys: another thread sees empty replacement"),
                ("deleted-publication", "  g_key_live_generations[key].store(0, std::memory_order_release);", "  // mutation: retain deleted publication",
                 "public", "pthread keys: deleted key reads empty"),
                ("wrong-publication", "g_key_live_generations[i].store(g_key_generations[i],", "g_key_live_generations[i].store(g_key_generations[i] + 1,",
                 "public", "pthread keys: new assignment gets its destructor"),
                ("generation-wrap", "g_key_generations[i] != UINT64_MAX", "true",
                 "generation_limit", "pthread keys: exhausted generation retires without wrap"),
                ("obsolete-destructor", "t_key_generations[k] == g_key_generations[k]", "true",
                 "public", "pthread keys: obsolete values never reach replacement destructor"),
                ("global-set-lock", "int BIONIC_SYSV gesher_bionic_pthread_setspecific(bionic_pthread_key_t key, const void* value) {",
                 "int BIONIC_SYSV gesher_bionic_pthread_setspecific(bionic_pthread_key_t key, const void* value) {\n  std::lock_guard<std::mutex> lock(g_keys_mutex);",
                 "set_progress", "pthread sets: local assignment proceeds while key registry is locked"),
                ("set-reloads-generation", "t_key_generations[key] = generation;",
                 "t_key_generations[key] = g_key_live_generations[key].load(std::memory_order_acquire);",
                 "set_snapshot", "pthread sets: overlapping stale set cannot become replacement value"),
                ("set-stamp-omitted", "t_key_generations[key] = generation;", "// mutation: omit assignment generation",
                 "writers", "pthread sets: overwrite/clear stays local and only final value reaches exit"),
                ("set-clear-omitted", "  t_key_values[key] = const_cast<void*>(value);",
                 "  if (value != nullptr) t_key_values[key] = const_cast<void*>(value);",
                 "writers", "pthread sets: overwrite/clear stays local and only final value reaches exit"),
                ("set-arm-omitted", "  if (value != nullptr) ArmHostExit();", "  // mutation: omit native-exit arming",
                 "set_arming", "pthread sets: first nonnull assignment preserves real exit arming"),
                ("set-invalid-arms", "  if (generation == 0) return kEINVAL;",
                 "  if (generation == 0) return kEINVAL;\n  if (value == nullptr) ArmHostExit();",
                 "set_arming", "pthread sets: invalid/null assignments do not arm native exit"),
            ]
            observations = []
            for name, before, after, case, label in mutants:
                original = overlap_template if case == "set_snapshot" else template
                if original is None or original.count(before) != 1:
                    raise ValueError("mutation boundary changed: " + name)
                mutated = original.replace(before, after)
                mutation_source = folder / (name + ".cc")
                mutation_executable = folder / name
                mutation_source.write_text(mutated)
                mutation_command = [str(mutation_source) if item == str(generated) else
                                    str(mutation_executable) if item == str(executable) else item for item in command]
                if case == "set_snapshot":
                    mutation_command += ["-DKEY_SET_SNAPSHOT_PROBE"]
                subprocess.run(mutation_command, check=True, timeout=120)
                result = subprocess.run([str(mutation_executable), case], env=env, cwd=folder, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
                (folder / (name + ".log")).write_text(result.stdout + result.stderr)
                exact_failure = any(line.startswith(label) and line.endswith("FAIL") for line in result.stdout.splitlines())
                if result.returncode != 1 or not exact_failure or "Sanitizer" in result.stderr or "runtime error:" in result.stderr:
                    raise RuntimeError(f"negative control did not fail its exact assertion: {name}: "
                                       f"{result.returncode}\n{result.stdout}\n{result.stderr}")
                observations.append({"name": name, "case": case, "assertion": label, "returncode": result.returncode,
                    "mutation": [before, after], "command": mutation_command,
                    "generated_sha256": hashlib.sha256(mutated.encode()).hexdigest()})
                print("NEGATIVE_PASS " + name, flush=True)
            (folder / "negative-controls.json").write_text(json.dumps(observations, indent=2) + "\n")
        return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
