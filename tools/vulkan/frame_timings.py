"""Summarize shared CPU presentation-boundary traces; these are not display timings.

Capture identical combat sequences/settings for both backends. Compositor
and GPU timings are additionally required for headset performance acceptance.
"""
import argparse
import csv
import json
import math
from pathlib import Path


def read_trace(path, first_frame=None, last_frame=None):
    metadata = {}
    lines = []
    for line in Path(path).read_text().splitlines():
        if line.startswith("# "):
            key, value = line[2:].split("=", 1)
            metadata[key] = value
        elif line:
            lines.append(line)
    hz = int(metadata.get("clock_hz", "0"))
    if hz <= 0 or metadata.get("boundary") != "cpu_present_return":
        raise ValueError("missing/invalid clock or CPU-boundary metadata")
    rows = [(int(r["frame"]), int(r["present_ticks"])) for r in csv.DictReader(lines)]
    if first_frame is not None or last_frame is not None:
        rows = [(frame,ticks) for frame,ticks in rows
                if (first_frame is None or frame >= first_frame) and (last_frame is None or frame <= last_frame)]
        if not rows or (first_frame is not None and rows[0][0] != first_frame) or (last_frame is not None and rows[-1][0] != last_frame):
            raise ValueError("requested analysis window was not fully captured")
    if len(rows) < 2:
        raise ValueError("at least two presentation records required")
    intervals = []
    for (frame, ticks), (next_frame, next_ticks) in zip(rows, rows[1:]):
        if next_frame != frame + 1 or next_ticks <= ticks:
            raise ValueError("noncontiguous frames or nonmonotonic clock")
        intervals.append((next_ticks - ticks) * 1000.0 / hz)
    return metadata, rows, intervals


def summarize(path, target_fps=60, slack_ms=0.5, first_frame=None, last_frame=None):
    if not math.isfinite(target_fps) or not math.isfinite(slack_ms) or target_fps <= 0 or slack_ms < 0:
        raise ValueError("invalid target FPS or late-frame slack")
    metadata, rows, intervals = read_trace(path, first_frame, last_frame)
    ordered = sorted(intervals)
    def percentile(q):
        return ordered[max(0, math.ceil(q * len(ordered)) - 1)]
    threshold = 1000.0 / target_fps + slack_ms
    late = [value > threshold for value in intervals]
    longest = current = 0
    for value in late:
        current = current + 1 if value else 0
        longest = max(longest, current)
    return {
        "source": str(path), "metadata": metadata,
        "measurement": "CPU presentation return intervals; not GPU/compositor/display timing",
        "first_frame": rows[0][0], "last_frame": rows[-1][0], "intervals": len(intervals),
        "duration_seconds": sum(intervals) / 1000.0,
        "average_cpu_cadence_fps": 1000.0 * len(intervals) / sum(intervals),
        "cpu_interval_ms": {"mean": sum(intervals) / len(intervals), "p50": percentile(.5),
                            "p95": percentile(.95), "p99": percentile(.99), "p999": percentile(.999),
                            "max": ordered[-1]},
        "late_threshold_ms": threshold, "late_intervals": sum(late),
        "late_percent": 100.0 * sum(late) / len(late),
        "over_25_ms": sum(v > 25 for v in intervals),
        "over_33_333_ms": sum(v > 1000.0/30 for v in intervals),
        "longest_late_burst": longest,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--target-fps", type=float, default=60)
    parser.add_argument("--slack-ms", type=float, default=.5)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--from-frame", type=int)
    parser.add_argument("--to-frame", type=int)
    args = parser.parse_args()
    result = {"candidate": summarize(args.trace, args.target_fps, args.slack_ms, args.from_frame, args.to_frame)}
    if args.baseline:
        baseline = summarize(args.baseline, args.target_fps, args.slack_ms, args.from_frame, args.to_frame)
        candidate = result["candidate"]
        if (candidate["first_frame"], candidate["last_frame"]) != (baseline["first_frame"], baseline["last_frame"]):
            raise ValueError("baseline/candidate frame windows differ")
        result["baseline"] = baseline
        result["comparison_requires"] = "same game, combat input, resolution, refresh, quality and foveation; CPU data alone cannot establish Quest performance"
    output = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(output)
    print(output, end="")


if __name__ == "__main__":
    main()
