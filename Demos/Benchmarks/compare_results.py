#!/usr/bin/env python3
#
# Aggregates the RESULT lines from mvkbench logs written by run_benchmarks.sh, and prints a
# Markdown table with, per library, the median of each metric across runs and its spread
# (max - min, relative to the median), and the change of each library from the first one.
#
# A change is labeled "better" or "worse" only when it exceeds the combined run-to-run spread of
# the two libraries; smaller changes are indistinguishable from noise and are labeled "noise".
#
# Usage: compare_results.py OUTDIR LABEL [LABEL...]

import glob
import os
import statistics
import sys
from collections import defaultdict

# Metrics shown in the summary, and whether a lower value is better.
METRICS = {
	"submit_ms_median": True,
	"submit_us_per_draw": True,
	"record_ms": True,
	"wall_ms": True,
	"speedup": False,
	"create_cache_ms": True,
	"first_pipeline_ms": True,
	"remaining_pipelines_ms": True,
	"total_ms": True,
	"save_cache_ms": True,
	"spirv_to_msl_ms": True,
	"msl_compile_ms": True,
	"library_from_cache_ms": True,
	"function_ms": True,
	"specialization_ms": True,
	"pipeline_compile_ms": True,
}

def load(outdir, label):
	values = defaultdict(list)
	for path in sorted(glob.glob(os.path.join(outdir, f"{label}.run*.log"))):
		with open(path) as f:
			for line in f:
				parts = line.split()
				if len(parts) == 5 and parts[0] == "RESULT":
					_, scenario, variant, metric, value = parts
					values[(scenario, variant, metric)].append(float(value))
	stats = {}
	for key, v in values.items():
		med = statistics.median(v)
		spread = (max(v) - min(v)) / med * 100.0 if med else 0.0
		stats[key] = (med, spread, len(v))
	return stats

def main():
	if len(sys.argv) < 3:
		print("usage: compare_results.py OUTDIR LABEL [LABEL...]", file=sys.stderr)
		sys.exit(1)
	outdir, labels = sys.argv[1], sys.argv[2:]
	data = {label: load(outdir, label) for label in labels}
	keys = sorted({k for d in data.values() for k in d if k[2] in METRICS})
	if not keys:
		print("No results found.", file=sys.stderr)
		sys.exit(1)

	runs = max((s[2] for d in data.values() for s in d.values()), default=0)
	print(f"Median of {runs} run(s) per library; ± is the run-to-run spread (max - min) relative to the median.\n")
	base = labels[0]
	header = ["scenario", "variant", "metric"] + labels + [f"{l} vs {base}" for l in labels[1:]]
	print("| " + " | ".join(header) + " |")
	print("|" + "|".join(["---"] * 3 + ["---:"] * (len(header) - 3)) + "|")
	for key in keys:
		row = list(key)
		stats = [data[l].get(key) for l in labels]
		row += ["" if s is None else f"{s[0]:.3f} ±{s[1]:.0f}%" for s in stats]
		b = stats[0]
		for s in stats[1:]:
			if s is None or b is None or b[0] == 0:
				row.append("")
				continue
			change = (s[0] - b[0]) / b[0] * 100.0
			if abs(change) <= b[1] + s[1]:
				verdict = "noise"
			else:
				lower_is_better = METRICS[key[2]]
				verdict = "better" if (change < 0) == lower_is_better else "worse"
			row.append(f"{change:+.1f}% ({verdict})")
		print("| " + " | ".join(row) + " |")

if __name__ == "__main__":
	main()
