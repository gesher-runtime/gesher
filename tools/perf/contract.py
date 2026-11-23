"""Evidence/statistical contracts for PERF-00. No Windows imports or app execution."""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import random
import statistics
from typing import Any


def _pairs(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"Duplicate JSON key: {key}")
        value[key] = item
    return value


def parse_json(text: str) -> Any:
    def invalid(value):
        raise ValueError(f"Nonfinite JSON value: {value}")
    return json.loads(text, object_pairs_hook=_pairs, parse_constant=invalid)


def read_json(path: Path) -> Any:
    if path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError("JSON input exceeds the 4 MiB evidence budget")
    return parse_json(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def number(value: Any, name: str, *, positive: bool = False) -> float:
    try:
        finite = type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        finite = False
    if not finite or value < 0 or (positive and value == 0):
        raise ValueError(f"{name} must be a finite {'positive' if positive else 'nonnegative'} number")
    return value


def integer(value: Any, name: str, minimum: int = 0, maximum: int = 100000) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer in [{minimum}, {maximum}]")
    return value


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact(path: Path) -> dict:
    """Freeze actual bytes; directories must not escape via symlinks/reparse points."""
    path = path.absolute()
    if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
        raise ValueError(f"Artifact is a link: {path}")
    if path.is_file():
        return {"path": str(path), "kind": "file", "size": path.stat().st_size,
                "sha256": hash_file(path)}
    if not path.is_dir():
        raise ValueError(f"Missing artifact: {path}")
    files = []
    for item in sorted(path.rglob("*")):
        if item.is_symlink() or (hasattr(item, "is_junction") and item.is_junction()):
            raise ValueError(f"Artifact contains a link: {item}")
        if item.is_file():
            files.append({"path": item.relative_to(path).as_posix(), "size": item.stat().st_size,
                          "sha256": hash_file(item)})
    if not files:
        raise ValueError(f"Empty artifact directory: {path}")
    digest = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return {"path": str(path), "kind": "directory", "sha256": digest, "files": files}


def verify_artifacts(frozen: dict[str, dict]) -> None:
    for role, saved in frozen.items():
        if artifact(Path(saved["path"])) != saved:
            raise ValueError(f"Frozen artifact changed: {role}")


def schedule(variants: list[str], rounds: int, warmups: int) -> list[dict]:
    """Reverse adjacent blocks; rotate 3-way blocks to balance each position."""
    rows = []
    for phase, count in (("warmup", warmups), ("measured", rounds)):
        for block in range(count):
            offset = (block // 2) % len(variants) if len(variants) > 2 else 0
            order = variants[offset:] + variants[:offset]
            if block % 2:
                order = list(reversed(order))
            for variant in order:
                rows.append({"sequence": len(rows), "phase": phase, "block": block,
                             "variant": variant, "status": "not_run"})
    return rows


def aggregate_snapshot(processes: list[dict]) -> dict:
    """Only aggregate contemporaneous private bytes, never unique shared RAM."""
    identities = set()
    pids = set()
    for process in processes:
        identity = (integer(process["pid"], "pid", 1, 2**32 - 1),
                    integer(process["created_100ns"], "creation time", 1, 2**64 - 1))
        if identity in identities or identity[0] in pids:
            raise ValueError("Duplicate/reused PID within one snapshot")
        identities.add(identity)
        pids.add(identity[0])
        private, resident = process.get("private_resident_bytes"), process.get("working_set_bytes")
        if private is not None and resident is not None and number(private, "private residency") > number(resident, "residency"):
            raise ValueError("Private residency exceeds working set")
    totals = {}
    for key in ("private_commit_bytes", "private_resident_bytes", "handles", "threads"):
        values = [process.get(key) for process in processes]
        totals[key] = None if any(value is None for value in values) else sum(number(v, key) for v in values)
    totals["processes"] = len(processes)
    # This includes duplicate physical pages. The name deliberately retains that fact.
    values = [process.get("working_set_bytes") for process in processes]
    totals["working_set_sum_including_shared_duplicates_bytes"] = (
        None if any(v is None for v in values) else sum(number(v, "working set") for v in values))
    totals["unique_shared_resident_bytes"] = None
    totals["unique_total_resident_bytes"] = None
    return totals


def validate_result(result: dict, *, invocation: str, variant: str, workload: str,
                    measured_ms: float, required_metrics: list[str]) -> dict[str, float | None]:
    if not isinstance(result, dict) or type(result.get("schema_version")) is not int or result["schema_version"] != 1:
        raise ValueError("Unsupported workload result")
    for key, wanted in (("invocation_id", invocation), ("variant", variant), ("workload_id", workload)):
        if result.get(key) != wanted:
            raise ValueError(f"Workload result {key} mismatch")
    if result.get("completed") is not True or result.get("success") is not True or result.get("problems"):
        raise ValueError("Workload failed or did not complete")
    if result.get("coverage_complete") is not True:
        raise ValueError("Workload process coverage is incomplete")
    metrics = result.get("metrics")
    if not isinstance(metrics, dict):
        raise ValueError("Missing metrics")
    for name, value in metrics.items():
        if value is not None:
            number(value, name)
    elapsed = number(result.get("workload_elapsed_ms"), "workload elapsed")
    if elapsed > measured_ms + 1:
        raise ValueError("Workload elapsed exceeds collector measurement window")
    events = result.get("events", [])
    if not isinstance(events, list):
        raise ValueError("Invalid event list")
    previous = 0
    names = set()
    for event in events:
        if not isinstance(event, dict) or not isinstance(event.get("name"), str) or event["name"] in names:
            raise ValueError("Invalid/duplicate event")
        instant = number(event.get("at_ms"), "event timestamp")
        if instant < previous or instant > elapsed:
            raise ValueError("Out-of-order/out-of-window event")
        previous = instant
        names.add(event["name"])
    for metric in required_metrics:
        if metrics.get(metric) is None:
            raise ValueError(f"Required metric unavailable: {metric}")
    return metrics


def quantile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    at = (len(ordered) - 1) * fraction
    lo, hi = math.floor(at), math.ceil(at)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (at - lo)


def distributions(values: list[float]) -> dict:
    if not values:
        return {"count": 0, "p50": None, "p95": None, "p99": None, "min": None, "max": None}
    return {"count": len(values), "p50": statistics.median(values), "p95": quantile(values, .95),
            "p99": quantile(values, .99), "min": min(values), "max": max(values)}


def paired_ratio_interval(pairs: list[tuple[float, float]], fraction: float, seed: int) -> dict:
    """Paired block bootstrap, descriptive until the configured sample gate is met."""
    baseline, candidate = zip(*pairs)
    base = quantile(list(baseline), fraction)
    if base <= 0:
        return {"ratio": None, "ci95": None, "reason": "nonpositive_baseline"}
    ratio = quantile(list(candidate), fraction) / base
    rng = random.Random(seed)
    ratios = []
    for _ in range(1000):
        sample = [pairs[rng.randrange(len(pairs))] for _ in pairs]
        a, b = zip(*sample)
        denominator = quantile(list(a), fraction)
        if denominator <= 0:
            return {"ratio": ratio, "ci95": None, "reason": "bootstrap_nonpositive_baseline"}
        ratios.append(quantile(list(b), fraction) / denominator)
    return {"ratio": ratio, "ci95": [quantile(ratios, .025), quantile(ratios, .975)],
            "method": "1000 deterministic paired block bootstrap resamples; percentile interval"}


def summarize(rows: list[dict], variants: list[str], required_metrics: list[str],
              min_tail_samples: int = 100, max_regression_percent: float = 5) -> dict:
    measured = [row for row in rows if row["phase"] == "measured"]
    all_complete = all(row["status"] == "passed" for row in rows)
    summaries, comparisons = {}, []
    for variant in variants:
        selected = [row for row in measured if row["variant"] == variant]
        successful = [row for row in selected if row["status"] == "passed"]
        summaries[variant] = {"planned": len(selected), "successful": len(successful),
                              "failed_or_incomplete": len(selected) - len(successful), "metrics": {}}
        for metric in required_metrics:
            values = [row["metrics"][metric] for row in successful if row["metrics"].get(metric) is not None]
            summaries[variant]["metrics"][metric] = distributions(values)
    status = "passed" if all_complete else "failed"
    baseline = {row["block"]: row for row in measured if row["variant"] == variants[0] and row["status"] == "passed"}
    limit = 1 + max_regression_percent / 100
    for variant in variants[1:]:
        candidates = {row["block"]: row for row in measured if row["variant"] == variant and row["status"] == "passed"}
        for metric in required_metrics:
            pairs = [(baseline[block]["metrics"].get(metric), candidates[block]["metrics"].get(metric))
                     for block in sorted(baseline.keys() & candidates.keys())]
            pairs = [(a, b) for a, b in pairs if a is not None and b is not None]
            item = {"baseline": variants[0], "candidate": variant, "metric": metric,
                    "paired_samples": len(pairs), "status": "insufficient_data"}
            if pairs:
                seed = int(hashlib.sha256(f"{variant}/{metric}".encode()).hexdigest()[:16], 16)
                item["p50"] = paired_ratio_interval(pairs, .5, seed)
                item["p95"] = paired_ratio_interval(pairs, .95, seed)
                intervals = [item[q]["ci95"] for q in ("p50", "p95")]
                if len(pairs) >= min_tail_samples and all(intervals):
                    if any(interval[0] > limit for interval in intervals):
                        item["status"] = "regression"
                    elif all(interval[1] <= limit for interval in intervals):
                        item["status"] = "passed"
                    else:
                        item["status"] = "inconclusive"
            if item["status"] == "regression":
                status = "failed"
            elif item["status"] != "passed" and status == "passed":
                status = "insufficient_data"
            comparisons.append(item)
    return {"schema_version": 1, "status": status, "gate_passed": status == "passed",
            "planned_attempts_including_warmups": len(rows),
            "failed_or_incomplete_attempts_including_warmups": sum(r["status"] != "passed" for r in rows),
            "minimum_tail_samples": min_tail_samples, "regression_limit_percent": max_regression_percent,
            "variants": summaries, "comparisons": comparisons,
            "notes": ["Failures and missing samples never become fast successes.",
                      "Bootstrap intervals describe sampled blocks, not immunity to runner drift or systematic bias.",
                      "p99 is descriptive; this gate does not establish p99 precision."]}
