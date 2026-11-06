#!/usr/bin/env python3
"""Index compatibility-related sections across repository documentation.

This is a reproducible navigation index, not a parser of semantic truth. Old
handoffs, negative statements and later supersessions can all match a marker.
Only paths, headings and marker locations are emitted; no app logs are read.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re


MARKERS = {
    "gap": r"\bgaps?\b",
    "remaining": r"\bremaining\b|\bremains? open\b|\bstill open\b",
    "unsupported": r"\bunsupported\b|\bnot supported\b",
    "unimplemented": r"\bunimplemented\b|\bnot implemented\b|\bnot yet done\b|\bnot done\b|\bunstarted\b",
    "unverified": r"\bunverified\b|\bnot verified\b|\bnot yet verified\b|\bnot yet CI-verified\b",
    "limits": r"\blimits\b|\blimitations\b",
    "todo": r"\bTODO\b|\[ \]",
}
PATTERNS = {key: re.compile(value, re.IGNORECASE) for key, value in MARKERS.items()}
EXCLUDED = {"docs/compatibility-audit.md", "docs/research/systematic-compatibility-2026-11-05.md"}
MAX_FILES = 2048
MAX_FILE_BYTES = 4 * 1024 * 1024
MAX_TOTAL_BYTES = 64 * 1024 * 1024


def index_markdown(path: str, text: str) -> list[dict]:
    sections: dict[tuple[int, str], dict] = {}
    current = (1, "(document introduction)")
    fenced: str | None = None
    for number, line in enumerate(text.splitlines(), 1):
        fence = re.match(r"^\s{0,3}(`{3,}|~{3,})", line)
        if fence:
            kind = fence[1][0]
            if fenced is None:
                fenced = kind
            elif fenced == kind:
                fenced = None
            continue
        if fenced is not None:
            continue
        heading = re.match(r"^(#{1,6})\s+(.+?)\s*#*\s*$", line)
        if heading:
            current = (number, heading[2])
        indicators = [key for key, pattern in PATTERNS.items() if pattern.search(line)]
        if indicators:
            row = sections.setdefault(current, {
                "path": path, "heading": current[1], "heading_line": current[0],
                "status": "unreviewed_navigation_candidate", "markers": [],
            })
            row["markers"].append({"line": number, "indicators": indicators})
    return list(sections.values())


def tier(path: str) -> str:
    if path.startswith("docs/handoff/"):
        return "handoff_or_historical_evidence"
    if path.startswith("docs/decisions/"):
        return "architecture_decision"
    if path.startswith("docs/research/"):
        return "investigation"
    return "subsystem_or_roadmap"


def scan(repo: Path) -> dict:
    documents, candidates, skipped, errors = [], [], [], []
    total = 0
    for base, dirs, files in os.walk(repo / "docs", followlinks=False):
        dirs[:] = sorted(name for name in dirs if not (Path(base) / name).is_symlink())
        for name in sorted(files):
            if not name.endswith(".md"):
                continue
            file = Path(base) / name
            relative = file.relative_to(repo).as_posix()
            if relative in EXCLUDED:
                skipped.append({"path": relative, "reason": "generated_audit_description"})
                continue
            if file.is_symlink() or file.is_junction():
                skipped.append({"path": relative, "reason": "link_or_reparse_path"})
                continue
            if len(documents) >= MAX_FILES:
                errors.append({"path": relative, "reason": "file_cap"})
                continue
            try:
                size = file.stat().st_size
                if size > MAX_FILE_BYTES or size + total > MAX_TOTAL_BYTES:
                    errors.append({"path": relative, "reason": "byte_cap"})
                    continue
                raw = file.read_bytes()
                if len(raw) != size:
                    raise ValueError("file changed during read")
                text = raw.decode("utf-8-sig", errors="strict")
                rows = index_markdown(relative, text)
                documents.append({"path": relative, "sha256": hashlib.sha256(raw).hexdigest(),
                                  "bytes": size, "tier": tier(relative), "candidate_sections": len(rows)})
                for row in rows:
                    row["tier"] = tier(relative)
                candidates.extend(rows)
                total += size
            except (OSError, UnicodeError, ValueError) as error:
                errors.append({"path": relative, "reason": type(error).__name__})
    return {
        "schema_version": 1,
        "purpose": "Full documentation navigation; candidates are not current defect assertions",
        "source_tool_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "summary": {"documents": len(documents), "bytes": total,
                    "candidate_sections": len(candidates),
                    "documents_by_tier": dict(Counter(item["tier"] for item in documents)),
                    "errors": len(errors)},
        "documents": documents, "candidate_sections": candidates,
        "excluded": skipped, "errors": errors,
    }


def render(report: dict) -> str:
    summary = report["summary"]
    lines = ["# Compatibility contract source index", "",
             f"Scanned {summary['documents']} Markdown files; found {summary['candidate_sections']} sections with navigation markers.",
             "These are **unreviewed candidates, not a defect count**. Historical and superseded statements are retained.",
             "", "| Source | Section | Marker lines | Source type |", "| --- | --- | --- | --- |"]
    for row in report["candidate_sections"]:
        markers = ", ".join(str(marker["line"]) for marker in row["markers"])
        heading = row["heading"].replace("|", "\\|")
        lines.append(f"| {row['path']}:{row['heading_line']} | {heading} | {markers} | {row['tier']} |")
    lines.extend(["", f"Read/cap errors: {len(report['errors'])}. Explicit exclusions: {len(report['excluded'])}."])
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out-dir", type=Path, default=Path("out/compat-audit"))
    args = parser.parse_args()
    report = scan(args.repo.resolve())
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "doc-contract-index.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.out_dir / "doc-contract-index.md").write_text(render(report), encoding="utf-8")
    print(json.dumps(report["summary"]))
    if report["errors"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
