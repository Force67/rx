#!/usr/bin/env python3
"""Build and run renderer correctness regressions, rejecting missing or skipped coverage."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
PROFILES = {
    "portable": (
        "offscreen_test", "rt_slot_tracker_test", "rt_instance_cull_test", "rt_failure_test",
        "recon_temporal_test", "recon_atrous_test", "path_alpha_test",
        "cloud_lighting_test", "lens_flare_test", "post_sampling_test",
    ),
    "raytracing": ("raytracing_test", "path_sampling_test", "path_motion_test", "path_material_test"),
    "d3d12": (
        "offscreen_test_d3d12", "recon_temporal_test_d3d12", "recon_atrous_test_d3d12",
        "path_alpha_test_d3d12", "cloud_lighting_test_d3d12", "lens_flare_test_d3d12", "post_sampling_test_d3d12",
    ),
    "fsr": ("upscaler_motion_test",),
    "dlss": ("upscaler_motion_test_dlss",),
}


def selected_tests(profiles: list[str]) -> set[str]:
    return {name for profile in profiles for name in PROFILES[profile]}


def report_failures(path: Path, expected: set[str]) -> list[str]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        return [f"missing or invalid test report: {error}"]
    failures = []
    seen = set()
    for case in root.iter("testcase"):
        name = case.get("name", "")
        if name in seen:
            failures.append(f"duplicate result: {name}")
        seen.add(name)
        diagnostic = case.findtext("system-out", "")
        if (case.find("skipped") is not None or case.get("status") in ("notrun", "disabled")
                or re.search(r"\bSKIP\b|skipping \(null backend\)", diagnostic)):
            failures.append(f"required coverage skipped: {name}")
        elif case.find("failure") is not None or case.find("error") is not None:
            failures.append(f"test failed: {name}")
        elif case.get("status") != "run":
            failures.append(f"test did not report execution: {name}")
    failures.extend(f"missing result: {name}" for name in sorted(expected - seen))
    failures.extend(f"unexpected result: {name}" for name in sorted(seen - expected))
    return failures


def run_logged(command: list[str], path: Path, env: dict[str, str], timeout: int | None = None) -> int:
    print("+ " + shlex.join(command), flush=True)
    with path.open("w") as log:
        try:
            with subprocess.Popen(command, cwd=REPO, env=env, stdout=log,
                                  stderr=subprocess.STDOUT,
                                  start_new_session=os.name == "posix") as process:
                try:
                    result = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    # A wrapper can own CTest, test processes, and Xvfb. Stop
                    # the whole group rather than leaving GPU tests behind.
                    if os.name == "posix":
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    else:
                        process.kill()
                    process.wait()
                    print(f"command timed out after {timeout}s", file=log)
                    result = 1
        except OSError as error:
            print(error, file=log)
            return 1
    if result:
        print("\n".join(path.read_text(errors="replace").splitlines()[-40:]), file=sys.stderr)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=REPO / "build/linux")
    parser.add_argument("--runner", default="", help="test wrapper, for example swrun or vkrun")
    parser.add_argument("--profile", action="append", choices=(*PROFILES, "all"),
                        help="repeat for multiple profiles; default: portable")
    parser.add_argument("--out", type=Path, help="logs and JUnit report directory")
    parser.add_argument("--jobs", type=int, default=6, help="build parallelism")
    parser.add_argument("--timeout", type=int, default=120, help="timeout per test in seconds")
    parser.add_argument("--no-build", action="store_true", help="use an already rebuilt test suite")
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error("jobs and timeout must be positive")
    profiles = args.profile or ["portable"]
    profiles = list(PROFILES) if "all" in profiles else list(dict.fromkeys(profiles))
    expected = selected_tests(profiles)
    build = args.build_dir.resolve()
    output = (args.out or build / "renderer-check" / "-".join(profiles)).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / "results.xml"
    report.unlink(missing_ok=True)
    # Reproduction must not depend on local renderer debug settings. CTest sets
    # the backend and vendor selection for each test itself.
    env = {key: value for key, value in os.environ.items() if not key.startswith("RX_")}
    env["VK_LAYER_VALIDATE_SYNC"] = "1"
    if run_logged([sys.executable, str(Path(__file__).with_name("test_check.py"))],
                  output / "gate-tests.log", env, 30):
        print(f"[FAIL] gate checks failed; see {output / 'gate-tests.log'}", file=sys.stderr)
        return 1
    if not args.no_build:
        targets = sorted({name.removesuffix("_d3d12").removesuffix("_dlss") for name in expected})
        if run_logged(["cmake", "--build", str(build), "--parallel", str(args.jobs),
                       "--target", *targets], output / "build.log", env):
            print(f"[FAIL] build failed; see {output / 'build.log'}", file=sys.stderr)
            return 1
    try:
        discovery = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                                   cwd=REPO, env=env, capture_output=True, text=True,
                                   check=True, timeout=30)
        inventory = json.loads(discovery.stdout)
        available = {test["name"] for test in inventory["tests"]}
    except (OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        print(f"[FAIL] test discovery failed: {error}", file=sys.stderr)
        return 1
    (output / "selection.json").write_text(json.dumps({
        "profiles": profiles, "required_tests": sorted(expected), "inventory": inventory,
    }, indent=2) + "\n")
    missing = expected - available
    if missing:
        print("[FAIL] required tests not configured: " + ", ".join(sorted(missing)), file=sys.stderr)
        return 1
    pattern = "^(" + "|".join(re.escape(name) for name in sorted(expected)) + ")$"
    command = shlex.split(args.runner) + [
        "ctest", "--test-dir", str(build), "--no-tests=error", "--parallel", "1",
        "--timeout", str(args.timeout), "--output-on-failure", "--verbose", "-R", pattern,
        "--output-junit", str(report),
    ]
    result = run_logged(command, output / "tests.log", env, args.timeout * len(expected) + 60)
    failures = report_failures(report, expected)
    if result:
        failures.insert(0, f"test command returned {result}")
    for failure in failures:
        print(f"[FAIL] {failure}", file=sys.stderr)
    if failures:
        print(f"Logs: {output}", file=sys.stderr)
        return 1
    print(f"[PASS] {len(expected)} required tests executed, no skips. Logs: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
