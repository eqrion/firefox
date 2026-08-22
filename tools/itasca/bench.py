#!/usr/bin/env python3
"""Compare wasm validate/compile performance between two js shell builds.

Usage:
    tools/itasca/bench.py <before-js> <after-js> [--corpus DIR] [--iterations N]
"""
import argparse
import csv
import io
import math
import subprocess
import sys
from pathlib import Path

HARNESS = Path(__file__).parent / "bench.js"
MODES = [
	("validate", None),
	("baseline", "baseline"),
	("ion", "ion"),
]


def run_js(js_bin, mode, corpus, iterations, warmup, wasm_compiler):
	js_mode = "validate" if mode == "validate" else "compile"
	cmd = [js_bin]
	if wasm_compiler:
		cmd.append(f"--wasm-compiler={wasm_compiler}")
	cmd += [str(HARNESS), js_mode, str(corpus), str(iterations), str(warmup)]
	result = subprocess.run(cmd, capture_output=True, text=True)
	if result.returncode != 0:
		sys.exit(f"{' '.join(cmd)} failed:\n{result.stderr}")
	return {row["file"]: row for row in csv.DictReader(io.StringIO(result.stdout))}


def write_csv(path, rows):
	with open(path, "w", newline="") as f:
		w = csv.DictWriter(f, fieldnames=["file", "bytes", "ok", "median_ms", "min_ms", "max_ms", "iterations", "error"])
		w.writeheader()
		for row in rows.values():
			w.writerow(row)


def geomean(values):
	return math.exp(sum(math.log(v) for v in values) / len(values))


def classify(before, after, files):
	compared, feature_gap, both_rejected, after_only = [], [], [], []
	for f in files:
		b_ok, a_ok = before[f]["ok"] == "true", after[f]["ok"] == "true"
		if b_ok and a_ok:
			compared.append(f)
		elif b_ok and not a_ok:
			feature_gap.append(f)
		elif not b_ok and a_ok:
			after_only.append(f)
		else:
			both_rejected.append(f)
	return compared, feature_gap, both_rejected, after_only


def ratios(before, after, files):
	return [float(after[f]["median_ms"]) / float(before[f]["median_ms"]) for f in files]


def print_summary(mode, before, after, files):
	compared, feature_gap, both_rejected, after_only = classify(before, after, files)
	print(f"\n== {mode} ==")
	if compared:
		r = ratios(before, after, compared)
		print(f"  compared:      n={len(compared):<3d} geomean={geomean(r):5.2f}x  min={min(r):5.2f}x  max={max(r):5.2f}x")
	else:
		print("  compared:      n=0")
	timed_rejects = [f for f in both_rejected if before[f]["median_ms"] and after[f]["median_ms"]]
	if timed_rejects:
		r = ratios(before, after, timed_rejects)
		print(f"  both-rejected: n={len(timed_rejects):<3d} geomean={geomean(r):5.2f}x  min={min(r):5.2f}x  max={max(r):5.2f}x  (reject-path timing only, not full validation)")
	elif both_rejected:
		print(f"  both-rejected: n={len(both_rejected):<3d} (no timing available, e.g. both fail to compile)")
	if feature_gap:
		print(f"  after rejects, before accepts: {len(feature_gap)} file(s): {', '.join(feature_gap)}")
	if after_only:
		print(f"  after accepts, before rejects: {len(after_only)} file(s): {', '.join(after_only)}")
	return compared, feature_gap, both_rejected, after_only


def print_details(mode, before, after, files, feature_gap, after_only):
	print(f"\n-- {mode} detail --")
	print(f"{'file':38s} {'before':>10s} {'after':>10s} {'ratio':>8s}")
	for f in files:
		b, a = before[f], after[f]
		tag = " [1.0-only]" if f in feature_gap else (" [after-only]" if f in after_only else "")
		if b["median_ms"] and a["median_ms"]:
			ratio = float(a["median_ms"]) / float(b["median_ms"])
			print(f"{f:38s} {float(b['median_ms']):10.3f} {float(a['median_ms']):10.3f} {ratio:7.2f}x{tag}")
		else:
			bm = b["median_ms"] or "--"
			am = a["median_ms"] or "--"
			print(f"{f:38s} {bm:>10s} {am:>10s} {'--':>8s}{tag}")


def main():
	p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	p.add_argument("before", help="path to the baseline js shell binary")
	p.add_argument("after", help="path to the js shell binary being compared against baseline")
	p.add_argument("--corpus", default=str(Path.home() / "src/wasm-corpus"), help="directory of .wasm files")
	p.add_argument("--iterations", type=int, default=8)
	p.add_argument("--warmup", type=int, default=2)
	p.add_argument("--details", action="store_true", help="also print the full per-file table")
	p.add_argument("--csv-dir", help="write raw per-mode CSVs to this directory")
	args = p.parse_args()

	corpus = Path(args.corpus)
	if not corpus.is_dir():
		sys.exit(f"corpus dir not found: {corpus}")
	for label, path in [("before", args.before), ("after", args.after)]:
		if not Path(path).is_file():
			sys.exit(f"{label} js binary not found: {path}")

	if args.csv_dir:
		Path(args.csv_dir).mkdir(parents=True, exist_ok=True)

	for mode, tier in MODES:
		before = run_js(args.before, mode, corpus, args.iterations, args.warmup, tier)
		after = run_js(args.after, mode, corpus, args.iterations, args.warmup, tier)
		if args.csv_dir:
			write_csv(Path(args.csv_dir) / f"before-{mode}.csv", before)
			write_csv(Path(args.csv_dir) / f"after-{mode}.csv", after)

		files = sorted(before.keys())
		compared, feature_gap, both_rejected, after_only = print_summary(mode, before, after, files)
		if args.details:
			print_details(mode, before, after, files, feature_gap, after_only)


if __name__ == "__main__":
	main()
