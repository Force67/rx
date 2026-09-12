#!/usr/bin/env python3
"""Summarize RX_GPU_TIMINGS_FILE samples after discarding warmup frames."""

import argparse
import collections
import csv
import math
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path")
    parser.add_argument("--warmup", type=int, default=60)
    parser.add_argument("--top", type=int, default=15)
    args = parser.parse_args()
    if args.warmup < 0 or args.top < 1:
        parser.error("warmup must be nonnegative and top must be positive")

    frames = collections.defaultdict(lambda: collections.defaultdict(float))
    with open(args.path, newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            frame = int(row["frame"])
            if frame >= args.warmup:
                frames[frame][row["pass"]] += float(row["ms"])
    if not frames:
        parser.error("no samples remain after warmup")

    passes = collections.defaultdict(list)
    totals = []
    for frame in frames.values():
        totals.append(sum(frame.values()))
        for name, ms in frame.items():
            passes[name].append(ms)

    def report(name, values):
        ordered = sorted(values)
        p95 = ordered[math.ceil(len(ordered) * 0.95) - 1]
        print(f"{statistics.mean(values):9.3f} {statistics.median(values):9.3f} "
              f"{p95:9.3f} {len(values):7d}  {name}")

    print(f"{len(frames)} resolved frames, warmup {args.warmup}, milliseconds")
    print(f"{'mean':>9} {'median':>9} {'p95':>9} {'samples':>7}  pass")
    report("TOTAL (sum of recorded passes)", totals)
    for name, values in sorted(passes.items(), key=lambda item: -statistics.mean(item[1]))[:args.top]:
        report(name, values)


if __name__ == "__main__":
    main()
