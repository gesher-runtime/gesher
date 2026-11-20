#!/usr/bin/env python3
"""Run the same explicit fixture queue on a caller-selected, owned Android device."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import subprocess
from notification_delegation_queue import build_request, parse_result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", type=Path, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apks", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    request = build_request(args.apks)
    raw = {"results": [], "requested_steps": len(request["steps"])}
    try:
        for step in request["steps"]:
            try:
                result = subprocess.run([str(args.adb), "-s", args.serial, *step["adb"]],
                                        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=step["timeout"])
                event = {"argv": step["adb"], "exit_code": result.returncode, "output": result.stdout}
            except subprocess.TimeoutExpired as failure:
                output = failure.stdout or ""
                if isinstance(output, bytes): output = output.decode(errors="replace")
                event = {"argv": step["adb"], "exit_code": 124, "output": output, "timed_out": True}
            raw["results"].append(event)
            if event["exit_code"] != 0 and not step.get("allow_failure", False): break
    finally:
        observations = parse_result(request, raw)
        observations["serial"] = args.serial
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(observations, indent=2) + "\n")
        print(args.output)
    if not observations["completed"]: raise SystemExit(1)


if __name__ == "__main__": main()
