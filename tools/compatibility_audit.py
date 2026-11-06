#!/usr/bin/env python3
"""Bounded, offline audit of technical compatibility diagnostics in Gesher logs.

Input logs are read only. Reports contain allowlisted identifiers and evidence
locations, never raw lines. This is observed runtime evidence, not API coverage
or a statement about the current source tree. See docs/compatibility-audit.md.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys


SEGMENT = r"[A-Za-z_$][A-Za-z0-9_$]*"
CLASS = rf"{SEGMENT}(?:[./]{SEGMENT})+"
VALUE_TYPE = rf"(?:boolean|byte|char|short|int|long|float|double|{CLASS})(?:\[\])*"
TYPE = rf"(?:void|{VALUE_TYPE})"
SIGNATURE = re.compile(rf"({TYPE})\s+({CLASS})\.({SEGMENT})\(([^()]*)\)")
CLASS_MARKERS = (
    re.compile(rf"(?:NoClassDefFoundError|ClassNotFoundException):\s*"
               rf"(?:Failed resolution of:\s*)?[\"']?(\[*L{CLASS};|{CLASS})(?=[\s\"':;]|$)"),
    re.compile(rf"ClassNotFoundException:\s*Didn't find class\s+[\"']({CLASS})[\"']"),
)
STUBBED = re.compile(r"gesher elf:\s*([A-Za-z0-9_.+-]+\.so):\s*'([^']+)' "
                     r"is not implemented \(stubbed\)")
CALLED = re.compile(r"gesher elf:\s*called unresolved function '([^']+)'")
DATA_ACCESSED = re.compile(r"gesher elf:\s*loaded code accessed data symbol '([^']+)'")
STAND_IN = re.compile(rf"\bstand-in\s+(I[A-Z][A-Za-z0-9_$]*)\.({SEGMENT}) "
                      r"(?:is )?not implemented\b")
SETTINGS = re.compile(r"\bact=(android\.settings\.[A-Z][A-Z0-9_]{0,127})(?=[\s}]|$)")
UNRESOLVED_INTENT = re.compile(r"ActivityNotFoundException|No [Aa]ctivity found|"
                               r"no activity (?:found|handles)|nothing handles|"
                               r"START_INTENT_NOT_RESOLVED")
SYMBOL = re.compile(r"[A-Za-z_][A-Za-z0-9_.$@]{0,239}\Z")
PACKAGE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+\Z")
SHARED_LOGS = frozenset(("system_server.log", "binderd.log", "servicemanager.log",
                        "dexopt.log", "apk-icons.log", "run-system_server.log",
                        "run-binderd.log", "run-servicemanager.log"))
EVIDENCE_LEVEL = {
    "native_import_stubbed": "import_only",
    "native_unresolved_function_called": "invocation_failure",
    "native_unresolved_data_accessed": "invocation_failure",
    "java_class_resolution_failed": "resolution_failure",
    "jni_implementation_missing": "invocation_failure",
    "stand_in_unimplemented": "unimplemented_service_call",
    "settings_intent_unresolved": "resolution_failure",
}


@dataclass(frozen=True)
class Bounds:
    max_files: int = 128
    max_file_bytes: int = 32 * 1024 * 1024
    max_total_bytes: int = 128 * 1024 * 1024
    max_line_bytes: int = 65536
    max_lines: int = 500_000
    max_gaps: int = 2048
    max_observations: int = 100_000
    max_evidence_per_gap: int = 12


@dataclass(frozen=True)
class Observation:
    category: str
    identifier: str
    kind: str
    library: str | None = None


def safe_identifier(value: str) -> bool:
    # These token formats must never be interpreted as technical identifiers.
    return len(value) <= 512 and not re.search(
        r"(?i)(?:AIza[0-9A-Za-z_-]{20}|(?:gh[pousr]_|github_pat_)[0-9A-Za-z_]{12}|"
        r"sk[-_][0-9A-Za-z_-]{12}|bearer\s)", value)


def normalize_class(value: str) -> str:
    value = value.lstrip("[")
    if value.startswith("L") and value.endswith(";"):
        value = value[1:-1]
    return value.replace("/", ".")


def jni_signature(value: str) -> str | None:
    match = SIGNATURE.fullmatch(value.strip())
    if match is None:
        return None
    result, owner, method, arguments = match.groups()
    args = [] if not arguments.strip() else [arg.strip() for arg in arguments.split(",")]
    if len(args) > 64 or any(re.fullmatch(VALUE_TYPE, arg) is None for arg in args):
        return None
    normalized = f"{result} {owner}.{method}({','.join(args)})".replace("/", ".")
    return normalized if safe_identifier(normalized) else None


def parse_line(line: str) -> list[Observation]:
    """Extract only grammar-validated identifiers; do not retain `line`."""
    if "gesher-ime:" in line:
        return []  # Typed text must never become evidence, even if it looks like a diagnostic.
    found: set[Observation] = set()
    for pattern in CLASS_MARKERS:
        for match in pattern.finditer(line):
            identifier = normalize_class(match.group(1))
            if safe_identifier(identifier):
                found.add(Observation("java_class", identifier, "java_class_resolution_failed"))
    if "UnsatisfiedLinkError" in line:
        tail = line.split("UnsatisfiedLinkError:", 1)[-1].strip()
        if tail.startswith("No implementation found for "):
            value = tail[len("No implementation found for "):].split(" (tried ", 1)[0]
        elif len(tail) >= 2 and tail[0] in "\"'":
            value = tail[1:].split(tail[0], 1)[0]
        else:
            value = ""
        identifier = jni_signature(value)
        if identifier:
            found.add(Observation("jni_method", identifier, "jni_implementation_missing"))
    for pattern, kind in ((STUBBED, "native_import_stubbed"),
                          (CALLED, "native_unresolved_function_called"),
                          (DATA_ACCESSED, "native_unresolved_data_accessed")):
        for match in pattern.finditer(line):
            library, symbol = match.groups() if pattern is STUBBED else (None, match.group(1))
            if SYMBOL.fullmatch(symbol) and safe_identifier(symbol):
                found.add(Observation("native_symbol", symbol, kind,
                                      library if library and safe_identifier(library) else None))
    for match in STAND_IN.finditer(line):
        identifier = ".".join(match.groups())
        if safe_identifier(identifier):
            found.add(Observation("stand_in_method", identifier, "stand_in_unimplemented"))
    if UNRESOLVED_INTENT.search(line):
        for match in SETTINGS.finditer(line):
            found.add(Observation("settings_intent", match.group(1), "settings_intent_unresolved"))
    return sorted(found, key=lambda item: (item.category, item.identifier, item.kind, item.library or ""))


def source_identity(name: str, index: int) -> tuple[str, str | None]:
    stem = name[:-4]  # Only top-level .log files are selected.
    if len(stem) <= 160 and PACKAGE.fullmatch(stem) and safe_identifier(stem):
        return name, stem
    if name in SHARED_LOGS:
        return name, None
    return f"redacted-log-{index:04d}.log", None


def audit_directory(directory: Path, bounds: Bounds = Bounds()) -> dict:
    if any(value <= 0 for value in asdict(bounds).values()):
        raise ValueError("audit bounds must be positive")
    candidates = []
    file_limit_reached = False
    with os.scandir(directory) as entries:
        for entry in entries:
            if not entry.name.endswith(".log"):
                continue
            info = entry.stat(follow_symlinks=False)
            if not stat.S_ISREG(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                continue
            if len(candidates) >= bounds.max_files:
                file_limit_reached = True
                break
            candidates.append(Path(entry.path))
    candidates.sort(key=lambda item: item.name)
    gaps: dict[str, dict] = {}
    sources = []
    total_bytes = total_lines = observations = omitted = 0
    stop_reason = None
    for index, path in enumerate(candidates, 1):
        if total_bytes >= bounds.max_total_bytes or total_lines >= bounds.max_lines:
            stop_reason = "total_bytes_or_lines_limit"
            break
        source_id = f"log-{index:04d}"
        filename, package = source_identity(path.name, index)
        manifest = {"source_id": source_id, "file": filename, "package": package,
                    "scanned_bytes": 0, "scanned_lines": 0, "oversized_lines_skipped": 0,
                    "truncated": False}
        digest = hashlib.sha256()
        sources.append(manifest)
        try:
            with path.open("rb") as stream:
                original_size = os.fstat(stream.fileno()).st_size
                budget = min(original_size, bounds.max_file_bytes, bounds.max_total_bytes - total_bytes)
                manifest["input_size_bytes"] = original_size
                if stream.read(2) in (b"\xff\xfe", b"\xfe\xff"):
                    manifest["error"] = "unsupported_utf16_encoding"
                    continue
                stream.seek(0)
                while manifest["scanned_bytes"] < budget and total_lines < bounds.max_lines:
                    line_number = manifest["scanned_lines"] + 1
                    chunks = []
                    length = 0
                    complete = False
                    while manifest["scanned_bytes"] < budget:
                        piece = stream.readline(min(bounds.max_line_bytes + 1,
                                                    budget - manifest["scanned_bytes"]))
                        if not piece:
                            break
                        digest.update(piece)
                        manifest["scanned_bytes"] += len(piece)
                        total_bytes += len(piece)
                        length += len(piece)
                        if length <= bounds.max_line_bytes:
                            chunks.append(piece)
                        else:
                            chunks.clear()
                        if piece.endswith(b"\n"):
                            complete = True
                            break
                    manifest["scanned_lines"] += 1
                    total_lines += 1
                    if length > bounds.max_line_bytes:
                        manifest["oversized_lines_skipped"] += 1
                        continue
                    if not complete and manifest["scanned_bytes"] < original_size:
                        break  # The byte cap cut a diagnostic in half.
                    if not length:
                        break
                    line = b"".join(chunks).decode("utf-8-sig", errors="replace")
                    for observation in parse_line(line):
                        if observations >= bounds.max_observations:
                            stop_reason = "observation_limit"
                            break
                        gap_id = f"{observation.category}:{observation.identifier}"
                        if gap_id not in gaps:
                            if len(gaps) >= bounds.max_gaps:
                                omitted += 1
                                continue
                            gaps[gap_id] = {"gap_id": gap_id, "category": observation.category,
                                            "identifier": observation.identifier,
                                            "current_source_status": "not_assessed",
                                            "observation_count": 0, "evidence_kinds": Counter(),
                                            "affected_packages": set(), "evidence": []}
                        gap = gaps[gap_id]
                        observations += 1
                        gap["observation_count"] += 1
                        gap["evidence_kinds"][observation.kind] += 1
                        if package:
                            gap["affected_packages"].add(package)
                        if len(gap["evidence"]) < bounds.max_evidence_per_gap:
                            evidence = {"source_id": source_id, "file": filename,
                                        "line": line_number, "kind": observation.kind}
                            if observation.library:
                                evidence["native_library"] = observation.library
                            gap["evidence"].append(evidence)
                    if stop_reason:
                        break
                manifest["truncated"] = manifest["scanned_bytes"] < original_size
        except OSError as error:
            manifest["error"] = type(error).__name__  # Never serialize exception text/paths.
        finally:
            manifest["scanned_sha256"] = digest.hexdigest()
        if stop_reason:
            break
    result = []
    for gap in sorted(gaps.values(), key=lambda item: item["gap_id"]):
        gap["affected_packages"] = sorted(gap["affected_packages"])
        gap["affected_app_count"] = len(gap["affected_packages"])
        gap["evidence_kinds"] = dict(sorted(gap["evidence_kinds"].items()))
        gap["evidence_levels"] = sorted({EVIDENCE_LEVEL[kind] for kind in gap["evidence_kinds"]})
        gap["evidence_samples_omitted"] = gap["observation_count"] - len(gap["evidence"])
        result.append(gap)
    source_bytes = Path(__file__).read_bytes()
    return {
        "schema_version": 1,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "generator": {"file": "tools/compatibility_audit.py",
                      "sha256": hashlib.sha256(source_bytes).hexdigest()},
        "scope": "historical_runtime_log_observations; current source and runtime identity not assessed",
        "counts_mean": "observations and affected package log filenames, not semantic defects or API coverage",
        "bounds": asdict(bounds),
        "summary": {"files_scanned": len(sources), "bytes_scanned": total_bytes,
                    "lines_scanned": total_lines, "gap_identifiers": len(result),
                    "observations": observations,
                    "affected_apps": len({pkg for gap in result for pkg in gap["affected_packages"]}),
                    "observations_not_aggregated": omitted,
                    "file_limit_reached": file_limit_reached, "stop_reason": stop_reason,
                    "files_truncated": sum(source["truncated"] for source in sources),
                    "files_with_errors": sum("error" in source for source in sources)},
        "sources": sources,
        "gaps": result,
    }


def markdown(report: dict) -> str:
    summary = report["summary"]
    lines = ["# Gesher compatibility observations", "", report["scope"], "",
             f"{summary['gap_identifiers']} identifiers; {summary['observations']} observations; "
             f"{summary['affected_apps']} affected package log names; {summary['files_scanned']} files scanned.",
             "", "Counts are observations/apps, not semantic defect counts. Current source status is not assessed.",
             "Import-only evidence does not prove a function was called. An invocation failure does not by itself prove an uncaught app crash.",
             "", "| Gap identifier | Observations | Apps | Evidence kinds | Sample locations |",
             "| --- | ---: | ---: | --- | --- |"]
    for gap in report["gaps"]:
        kinds = ", ".join(f"{kind}={count}" for kind, count in gap["evidence_kinds"].items())
        locations = ", ".join(f"{item['file']}:{item['line']}" for item in gap["evidence"][:3])
        lines.append(f"| `{gap['gap_id']}` | {gap['observation_count']} | "
                     f"{gap['affected_app_count']} | {kinds} | {locations} |")
    lines.extend(("", "See the JSON for bounded evidence samples, source hashes, truncation and read errors.", ""))
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log_directory", type=Path)
    parser.add_argument("--out-dir", type=Path, default=Path("out/compat-audit"))
    for name, value in asdict(Bounds()).items():
        parser.add_argument("--" + name.replace("_", "-"), type=int, default=value)
    args = parser.parse_args(argv)
    bounds = Bounds(**{name: getattr(args, name) for name in asdict(Bounds())})
    try:
        input_root, output_root = args.log_directory.resolve(), args.out_dir.resolve()
        if output_root == input_root or input_root in output_root.parents:
            raise ValueError("reports must be outside the read-only input directory")
        report = audit_directory(input_root, bounds)
        output_root.mkdir(parents=True, exist_ok=True)
        for name, content in (("compatibility.json", json.dumps(report, indent=2, ensure_ascii=True) + "\n"),
                              ("compatibility.md", markdown(report))):
            target = output_root / name
            if target.is_symlink():
                raise ValueError("report destination must not be a symlink")
            target.write_text(content, encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"Compatibility audit failed ({type(error).__name__}); input logs were not modified.", file=sys.stderr)
        return 1
    print(json.dumps(report["summary"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
