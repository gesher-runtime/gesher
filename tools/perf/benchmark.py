#!/usr/bin/env python3
"""Run frozen A/B workloads and preserve every attempt (PERF-00 foundation).

python tools/perf/benchmark.py plan.json --out NEW_DIRECTORY
Exit 0: measured gates passed; 1: failure/regression; 2: insufficient evidence.
See docs/performance-benchmarks.md. No runtime build or download is implicit.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid

from contract import (artifact, hash_file, integer, number, read_json,
                      schedule, summarize, validate_result, verify_artifacts, write_json)
import protocol_io

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IDENTIFIER = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,79}$")
RESOURCE_METRICS = ("cpu_ms", "read_bytes", "write_bytes", "other_bytes", "page_faults",
                    "private_commit_sampled_peak_bytes", "private_resident_sampled_peak_bytes",
                    "handles_sampled_peak", "threads_sampled_peak")
UNAVAILABLE = ("gpu_memory_bytes", "context_switches", "wakeups", "allocated_bytes", "copied_bytes", "gc_pause_ms")


def identifier(value, name):
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value):
        raise ValueError(f"Invalid {name}")
    return value


def validate_plan(plan):
    if not isinstance(plan, dict) or type(plan.get("schema_version")) is not int or plan["schema_version"] != 1:
        raise ValueError("Unsupported plan schema")
    workload, variants = plan.get("workload"), plan.get("variants")
    if not isinstance(workload, dict) or not isinstance(variants, list) or len(variants) not in (2, 3):
        raise ValueError("Expected one workload and baseline/candidate[/integrated] variants")
    identifier(workload.get("id"), "workload ID")
    if workload.get("kind") not in ("command", "e2e"):
        raise ValueError("Workload kind must be command or e2e")
    if any(not isinstance(variant, dict) for variant in variants):
        raise ValueError("Each variant must be an object")
    names = [identifier(variant.get("id"), "variant ID") for variant in variants]
    if len(set(names)) != len(names):
        raise ValueError("Duplicate variant ID")
    for variant in variants:
        if not isinstance(variant.get("source_revision"), str) or not re.fullmatch(r"[0-9a-f]{40}", variant["source_revision"]):
            raise ValueError("Each variant requires a full declared source revision")
        if not isinstance(variant.get("artifacts"), dict) or not variant["artifacts"]:
            raise ValueError("Each variant requires named artifact paths")
    for paths in [workload.get("artifacts", {})] + [variant["artifacts"] for variant in variants]:
        if not isinstance(paths, dict):
            raise ValueError("Artifacts must be named paths")
        for role, path in paths.items():
            identifier(role, "artifact role")
            if not isinstance(path, str) or not path or "\0" in path:
                raise ValueError("Artifact paths must be nonempty strings")
    if workload["kind"] == "command":
        if (not isinstance(workload.get("command"), list) or not workload["command"] or
                any(not isinstance(arg, str) or "\0" in arg for arg in workload["command"])):
            raise ValueError("Command workload requires an argv array")
    else:
        if workload.get("scenario") not in ("cold-device", "warm-device"):
            raise ValueError("E2E supports explicit cold-device or warm-device scenarios")
        if workload.get("abi") not in ("x86_64", "arm64-v8a"):
            raise ValueError("E2E requires an explicit ABI")
        for field in ("apk", "manifest", "package"):
            if not isinstance(workload.get(field), str) or not workload[field]:
                raise ValueError(f"Missing workload {field}")
        for variant in variants:
            if "runtime" not in variant["artifacts"]:
                raise ValueError("E2E requires a complete runtime directory artifact")
    measurement = plan.setdefault("measurement", {})
    if not isinstance(measurement, dict):
        raise ValueError("measurement must be an object")
    defaults = {"rounds": 30, "warmups": 1, "timeout_seconds": 600, "sample_interval_ms": 100,
                "minimum_tail_samples": 100, "max_regression_percent": 5,
                "collect_resources": True, "collect_residency": False}
    for name, default in defaults.items():
        measurement.setdefault(name, default)
    integer(measurement["rounds"], "rounds", 1, 10000)
    integer(measurement["warmups"], "warmups", 0, 100)
    number(measurement["timeout_seconds"], "timeout", positive=True)
    integer(measurement["sample_interval_ms"], "sample interval", 20, 10000)
    integer(measurement["minimum_tail_samples"], "minimum tail samples", 20, 10000)
    number(measurement["max_regression_percent"], "regression limit")
    for key in ("collect_resources", "collect_residency"):
        if type(measurement[key]) is not bool:
            raise ValueError(f"{key} must be Boolean")
    metrics = measurement.setdefault("required_metrics", ["command_to_first_frame_observed_ms",
        "first_input_to_change_observed_ms", "cpu_ms", "private_commit_sampled_peak_bytes"]
        if workload["kind"] == "e2e" else ["workload_ms"])
    if not isinstance(metrics, list) or not metrics or any(not isinstance(name, str) for name in metrics):
        raise ValueError("required_metrics must be a nonempty distinct list")
    if len(set(metrics)) != len(metrics):
        raise ValueError("required_metrics must be a nonempty distinct list")
    for name in metrics:
        identifier(name, "metric")
    if workload["kind"] == "e2e" and (os.name != "nt" or measurement["minimum_tail_samples"] < 100):
        raise ValueError("E2E needs Windows and at least 100 samples for tail acceptance")
    return plan


def host_identity():
    result = {"os": platform.platform(), "machine": platform.machine(), "logical_cpus": os.cpu_count(),
              "python": sys.version, "python_executable_sha256": hash_file(Path(sys.executable)),
              "driver_binary_hashes": None, "hardware": None, "hardware_error": None}
    if os.name == "nt":
        # Outside timed/job scope. No command text contains plan/user values.
        command = """$ErrorActionPreference='Stop'; [ordered]@{
          processors=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);
          system=Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer,Model,TotalPhysicalMemory;
          video=@(Get-CimInstance Win32_VideoController | Select-Object Name,PNPDeviceID,DriverVersion,DriverDate);
          os=Get-CimInstance Win32_OperatingSystem | Select-Object Version,BuildNumber
        } | ConvertTo-Json -Depth 6 -Compress"""
        try:
            completed = subprocess.run(["powershell.exe", "-NoProfile", "-Command", command],
                                       capture_output=True, timeout=30, check=True)
            result["hardware"] = json.loads(completed.stdout.decode("utf-8-sig"))
            canonical = json.dumps(result["hardware"], sort_keys=True).encode()
            result["hardware_driver_metadata_sha256"] = hashlib.sha256(canonical).hexdigest()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            result["hardware_error"] = str(error)
    return result


class PortableProcess:
    """Fixture/workload execution on POSIX; no Windows resource claims."""
    def __init__(self, command, cwd, env, output):
        self.stdout = (output / "stdout.log").open("wb")
        self.stderr = (output / "stderr.log").open("wb")
        try:
            self.process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                            stdout=self.stdout, stderr=self.stderr, start_new_session=True)
        except BaseException:
            self.stdout.close()
            self.stderr.close()
            raise

    def poll(self):
        return self.process.poll()

    def close(self):
        try:
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait(timeout=10)
        finally:
            self.stdout.close()
            self.stderr.close()


def resource_summary(samples):
    metrics = {key: None for key in RESOURCE_METRICS + UNAVAILABLE}
    if len(samples) < 2:
        return metrics
    first, last = samples[0]["job"], samples[-1]["job"]
    deltas, lower_deltas = {}, {}
    for key in ("read_bytes", "write_bytes", "other_bytes", "page_faults", "user_cpu_ms", "kernel_cpu_ms"):
        inclusive = last[key] - first[key]
        if inclusive < 0:
            raise ValueError("Job counter decreased during measurement")
        if samples[0].get("exclude_root_observer"):
            before_a, after_a = samples[0]["observer_before"], samples[0]["observer_after"]
            before_b, after_b = samples[-1]["observer_before"], samples[-1]["observer_after"]
            if any(not value for value in (before_a, after_a, before_b, after_b)):
                raise ValueError("Missing E2E observer counters")
            observer_min = max(0, before_b[key] - after_a[key])
            observer_max = after_b[key] - before_a[key]
            if observer_max < observer_min:
                raise ValueError("Observer counter decreased during measurement")
            deltas[key] = max(0, inclusive - observer_min)
            lower_deltas[key] = max(0, inclusive - observer_max)
        else:
            deltas[key] = lower_deltas[key] = inclusive
    metrics.update({key: deltas[key] for key in ("read_bytes", "write_bytes", "other_bytes", "page_faults")})
    metrics["cpu_ms"] = deltas["user_cpu_ms"] + deltas["kernel_cpu_ms"]
    metrics["cpu_ms_lower_bound"] = lower_deltas["user_cpu_ms"] + lower_deltas["kernel_cpu_ms"]
    metrics["observer_subtraction_cpu_uncertainty_ms"] = metrics["cpu_ms"] - metrics["cpu_ms_lower_bound"]
    number(metrics["cpu_ms"], "job CPU delta")
    complete = [sample for sample in samples if sample["live_snapshot_complete"]]
    for output, field in (("private_commit_sampled_peak_bytes", "private_commit_bytes"),
                          ("private_resident_sampled_peak_bytes", "private_resident_bytes"),
                          ("handles_sampled_peak", "handles"), ("threads_sampled_peak", "threads")):
        values = [sample["totals"][field] for sample in complete if sample["totals"][field] is not None]
        if values:
            metrics[output] = max(values)
    return metrics


def _command(plan, variant, output):
    work = plan["workload"]
    if work["kind"] == "e2e":
        return ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                str(HERE / "e2e-workload.ps1"), "-Request", str(output / "request.json")]
    substitutions = {"{attempt}": str(output), "{variant}": variant["id"], "{python}": sys.executable}
    substitutions.update({"{artifact:" + role + "}": path for role, path in variant["artifacts"].items()})
    command = []
    for argument in work["command"]:
        for key, value in substitutions.items():
            argument = argument.replace(key, value)
        command.append(argument)
    return command


def register_participants(message, *, invocation, previous, phase, process):
    """Authenticate live participants once per ordered request, before UI work."""
    if not isinstance(message, dict) or message.get("invocation_id") != invocation:
        raise ValueError("Participant registration invocation mismatch")
    sequence = integer(message.get("sequence"), "participant registration sequence", 1, 100000)
    if previous and sequence == previous["sequence"]:
        if message != previous:
            raise ValueError("Participant registration changed without a new sequence")
        return previous
    if sequence != (previous["sequence"] + 1 if previous else 1) or phase != "start":
        raise ValueError("Invalid participant registration order/phase")
    participants = message.get("participants")
    if not isinstance(participants, list) or not participants:
        raise ValueError("Participant registration requires live identities")
    if os.name != "nt":
        raise ValueError("Live participant registration requires Windows")
    process.verify_participants(participants)
    return message


def run_attempt(plan, variant, output, frozen, invocation):
    measurement = plan["measurement"]
    request = {"schema_version": 1, "invocation_id": invocation, "variant": variant["id"],
               "workload": plan["workload"], "artifacts": variant["artifacts"],
               "artifact_identities": frozen, "output": str(output)}
    write_json(output / "request.json", request)
    process = None
    samples = []
    started = time.monotonic()
    deadline = started + measurement["timeout_seconds"]
    phase, measured_start, measured_end = None, None, None
    result = {"status": "failed", "metrics": {}, "problems": [], "collection": {}}
    collector_cpu_start = time.process_time()
    last_sample = 0
    registration = None
    verified_registrations = []
    try:
        command = _command(plan, variant, output)
        env = os.environ.copy()
        env["GESHER_PERF_ATTEMPT"] = str(output)
        if os.name == "nt":
            from windows import WindowsJob
            process = WindowsJob(command, ROOT, env, output, residency=measurement["collect_residency"],
                                 exclude_root=plan["workload"]["kind"] == "e2e")
        else:
            process = PortableProcess(command, ROOT, env, output)

        def sample():
            if os.name == "nt" and measurement["collect_resources"]:
                samples.append(process.sample())

        while True:
            now = time.monotonic()
            if now - started > measurement["timeout_seconds"]:
                result["status"] = "timed_out"
                raise TimeoutError("Workload exceeded its whole-attempt deadline (including preparation)")
            signal_path = output / "phase.json"
            if signal_path.exists():
                signal_data = protocol_io.read_json(signal_path, deadline=deadline)
                if signal_data.get("invocation_id") != invocation:
                    raise ValueError("Measurement phase invocation mismatch")
                wanted = signal_data.get("phase")
                if wanted != phase:
                    if wanted == "start" and phase is None:
                        if os.name == "nt":
                            process.verify_participants(signal_data.get("participants", []))
                        sample()
                        measured_start = time.monotonic()
                        last_sample = measured_start
                    elif wanted == "stop" and phase == "start":
                        sample()
                        measured_end = time.monotonic()
                    else:
                        raise ValueError("Invalid measurement phase order")
                    phase = wanted
                    protocol_io.write_json(output / "ack.json", {"invocation_id": invocation, "phase": phase},
                                           deadline=deadline)
            registration_path = output / "participants.json"
            if registration_path.exists():
                registered = register_participants(protocol_io.read_json(registration_path, deadline=deadline), invocation=invocation,
                    previous=registration, phase=phase, process=process)
                if registered is not registration:
                    registration = registered
                    verified_registrations.append({"request": registration,
                        "verified_at_attempt_ms": (time.monotonic() - started) * 1000})
                    protocol_io.write_json(output / "participants-ack.json", {"invocation_id": invocation,
                        "sequence": registration["sequence"], "verified": True}, deadline=deadline)
            if phase == "start" and now - last_sample >= measurement["sample_interval_ms"] / 1000:
                sample()
                last_sample = time.monotonic()
            if time.monotonic() >= deadline:
                raise TimeoutError("Workload exceeded its whole-attempt deadline (including preparation)")
            code = process.poll()
            if code is not None:
                result["exit_code"] = code
                if code != 0:
                    raise ValueError(f"Workload exited with code {code}")
                break
            time.sleep(.01)
        if phase != "stop" or measured_start is None or measured_end is None:
            raise ValueError("Missing measured-phase start/stop handshake")
        payload = read_json(output / "result.json")
        measured_ms = (measured_end - measured_start) * 1000
        # Workload cannot overwrite measurements collected outside its process.
        raw_metrics = validate_result(payload, invocation=invocation, variant=variant["id"],
                                      workload=plan["workload"]["id"], measured_ms=measured_ms,
                                      required_metrics=[])
        if os.name == "nt":
            process.verify_participants(payload.get("participants", []), allow_observed_exit=True)
        if plan["workload"]["kind"] == "e2e":
            validate_e2e(payload, request, registered_participants=[item for registration in verified_registrations
                for item in registration["request"]["participants"]])
        metrics = dict(raw_metrics)
        metrics.update(resource_summary(samples))
        for key in measurement["required_metrics"]:
            if metrics.get(key) is None:
                raise ValueError(f"Required metric unavailable: {key}")
        result.update(status="passed", metrics=metrics, workload_result_sha256=hash_file(output / "result.json"),
                      measured_window_ms=measured_ms, preparation_ms=(measured_start - started) * 1000)
    except (OSError, ValueError, RuntimeError, TimeoutError, KeyError, TypeError) as error:
        if isinstance(error, TimeoutError):
            result["status"] = "timed_out"
        result["problems"].append(str(error))
    finally:
        result["collection"] = {"supported": os.name == "nt", "enabled": measurement["collect_resources"],
            "sampler_cpu_ms": (time.process_time() - collector_cpu_start) * 1000,
            "sample_interval_ms": measurement["sample_interval_ms"], "samples": samples,
            "verified_registrations": verified_registrations,
            "incomplete_live_snapshots": sum(not sample["live_snapshot_complete"] for sample in samples),
            "job_lifetime_peak_commit_bytes": samples[-1]["job"]["lifetime_peak_commit_bytes"] if samples else None,
            "notes": ["Job-lifetime peak includes preparation/observer; measured private peak is sampled, not exact.",
                      "E2E observer root counters are bracketed/subtracted; CPU/I/O/fault metrics use the conservative upper bound; memory/handles/threads exclude it.",
                      "Sampling is non-atomic; incomplete live snapshots are excluded from sampled peaks.",
                      "Sampler CPU alone does not measure observer perturbation; compare collection on/off."]}
        if process is not None:
            try:
                process.close()
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                result["status"] = "failed"
                result["problems"].append(f"Owned workload cleanup failed: {error}")
        result["attempt_wall_ms"] = (time.monotonic() - started) * 1000
        write_json(output / "attempt.json", result)
    return result


def validate_e2e(payload, request, *, registered_participants):
    work = request["workload"]
    expected = {"manifest_sha256": request["artifact_identities"]["manifest"]["sha256"],
                "apk_sha256": request["artifact_identities"]["apk"]["sha256"],
                "runtime_sha256": request["artifact_identities"]["runtime"]["sha256"],
                "package": work["package"], "scenario": work["scenario"], "abi": work["abi"]}
    evidence = payload.get("evidence", {})
    for key, value in expected.items():
        if evidence.get(key) != value:
            raise ValueError(f"E2E evidence {key} mismatch")
    for key in ("first_frame_verified", "nonblank_verified", "input_change_verified", "lifecycle_verified",
                "fresh_profile", "owned_device_verified"):
        if evidence.get(key) is not True:
            raise ValueError(f"Missing E2E evidence: {key}")
    participants = payload.get("participants", [])
    required_roles = {"application", "binderd", "servicemanager", "system_server"}
    if (not isinstance(participants, list) or any(not isinstance(item, dict) for item in participants) or
            not required_roles <= {item.get("role") for item in participants} or
            len({(item.get("pid"), item.get("created_100ns")) for item in participants
                 if item.get("role") in required_roles}) < 4):
        raise ValueError("Missing distinct application/device process coverage")
    authenticated = {(item["pid"], item["created_100ns"]) for item in registered_participants}
    if not {(item["pid"], item["created_100ns"]) for item in participants} <= authenticated:
        raise ValueError("E2E participant lacks acknowledged live registration")
    names = [event["name"] for event in payload.get("events", [])]
    required = ["command_start", "first_frame_observed", "first_input", "first_input_change_observed", "completed"]
    if any(name not in names for name in required) or [names.index(name) for name in required] != sorted(names.index(name) for name in required):
        raise ValueError("Missing/out-of-order E2E event boundaries")
    events = {event["name"]: event["at_ms"] for event in payload["events"]}
    metrics = payload["metrics"]
    if (abs(metrics["command_to_first_frame_observed_ms"] - (events["first_frame_observed"] - events["command_start"])) > .01 or
            abs(metrics["first_input_to_change_observed_ms"] - (events["first_input_change_observed"] - events["first_input"])) > .01):
        raise ValueError("E2E metrics do not match recorded boundaries")


def run(plan_path, output):
    plan_path, output = plan_path.resolve(), output.absolute()
    plan = validate_plan(read_json(plan_path))
    # The caller selects an unused directory. There is no reuse/overwrite mode.
    output.mkdir(parents=True, exist_ok=False)
    base = plan_path.parent
    common_paths = dict(plan["workload"].get("artifacts", {}))
    if plan["workload"]["kind"] == "e2e":
        for field in ("apk", "manifest"):
            plan["workload"][field] = str((base / plan["workload"][field]).resolve())
            common_paths[field] = plan["workload"][field]
    common = {role: artifact(base / path) for role, path in common_paths.items()}
    variant_artifacts = {}
    for variant in plan["variants"]:
        variant["artifacts"] = {role: str((base / path).resolve()) for role, path in variant["artifacts"].items()}
        variant_artifacts[variant["id"]] = {role: artifact(Path(path)) for role, path in variant["artifacts"].items()}
        if plan["workload"]["kind"] == "e2e" and variant_artifacts[variant["id"]]["runtime"]["kind"] != "directory":
            raise ValueError("E2E runtime must be an unpacked complete runtime directory")
    harness_paths = list(HERE.glob("*.py")) + list(HERE.glob("*.ps1"))
    if plan["workload"]["kind"] == "e2e":
        harness_paths += [ROOT / "tests/e2e/window-workload.ps1", ROOT / "tests/windows/graphics/WindowChecks.ps1"]
    frozen_harness = {str(path.relative_to(ROOT)): artifact(path) for path in harness_paths}
    host = host_identity()
    frozen = {"plan_sha256": hash_file(plan_path), "common": common, "variants": variant_artifacts,
              "harness": frozen_harness, "host": host, "plan": plan,
              "source_revision_status": "Declared by the caller; artifact hashes are measured, not proof of source provenance."}
    write_json(output / "frozen.json", frozen)
    variants = [variant["id"] for variant in plan["variants"]]
    by_id = {variant["id"]: variant for variant in plan["variants"]}
    measurement = plan["measurement"]
    rows = schedule(variants, measurement["rounds"], measurement["warmups"])
    ledger = {"schema_version": 1, "invocation_id": uuid.uuid4().hex, "completed": False,
              "started_utc": datetime.now(timezone.utc).isoformat(), "frozen_sha256": hash_file(output / "frozen.json"),
              "attempts": rows}
    write_json(output / "runs.json", ledger)
    for row in rows:
        row["invocation_id"] = uuid.uuid4().hex
        row["directory"] = f"{row['sequence']:05d}-{row['variant']}-{row['invocation_id']}"
        folder = output / row["directory"]
        folder.mkdir()
        row["status"] = "running"
        write_json(output / "runs.json", ledger)
        try:
            verify_artifacts(common)
            verify_artifacts(variant_artifacts[row["variant"]])
            verify_artifacts(frozen_harness)
            evidence = {**common, **variant_artifacts[row["variant"]]}
            result = run_attempt(plan, by_id[row["variant"]], folder, evidence, row["invocation_id"])
            verify_artifacts(variant_artifacts[row["variant"]])
            verify_artifacts(common)
            verify_artifacts(frozen_harness)
            row.update({key: value for key, value in result.items() if key != "collection"})
            row["attempt_sha256"] = hash_file(folder / "attempt.json")
        except (OSError, ValueError, RuntimeError) as error:
            row.update(status="failed", metrics={}, problems=[str(error)])
            # Preserve the raw collection even when post-run identity checks fail.
            if (folder / "attempt.json").exists():
                row["attempt_sha256"] = hash_file(folder / "attempt.json")
                write_json(folder / "validation-error.json", {"problems": [str(error)]})
            else:
                write_json(folder / "attempt.json", row)
                row["attempt_sha256"] = hash_file(folder / "attempt.json")
        write_json(output / "runs.json", ledger)
        print(f"{row['sequence'] + 1}/{len(rows)} {row['phase']} {row['variant']}: {row['status']}", flush=True)
    ledger["completed"] = True
    write_json(output / "runs.json", ledger)
    report = summarize(rows, variants, measurement["required_metrics"],
                       measurement["minimum_tail_samples"], measurement["max_regression_percent"])
    report["runs_sha256"] = hash_file(output / "runs.json")
    report["frozen_sha256"] = hash_file(output / "frozen.json")
    write_json(output / "summary.json", report)
    return 0 if report["gate_passed"] else (1 if report["status"] == "failed" else 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        return run(args.plan, args.out)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"benchmark: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
