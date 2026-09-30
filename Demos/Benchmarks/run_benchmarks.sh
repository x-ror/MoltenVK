#!/bin/bash
#
# Runs mvkbench against several Vulkan libraries (typically different MoltenVK builds),
# interleaving the runs to spread thermal and background noise evenly, and prints a
# comparison table of the median results. See README.md.
#
# Usage: run_benchmarks.sh [-n RUNS] [-b MVKBENCH] [-o OUTDIR] [-- MVKBENCH_ARGS...] LABEL=LIBRARY [LABEL=LIBRARY...]
#
# Example:
#   ./run_benchmarks.sh -n 5 \
#       base=/path/to/base/libMoltenVK.dylib \
#       stageB=/path/to/stageB/libMoltenVK.dylib \
#       stageC=/path/to/stageC/libMoltenVK.dylib

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
runs=3
bench="$here/build/mvkbench"
outdir="$here/results/$(date +%Y%m%d-%H%M%S)"
bench_args=()
libs=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		-n) runs="$2"; shift 2 ;;
		-b) bench="$2"; shift 2 ;;
		-o) outdir="$2"; shift 2 ;;
		--) shift; while [[ $# -gt 0 && "$1" != *=* ]]; do bench_args+=("$1"); shift; done ;;
		-h|--help) sed -n '3,15p' "$0"; exit 0 ;;
		*=*) libs+=("$1"); shift ;;
		*) echo "Unknown argument: $1" >&2; exit 1 ;;
	esac
done

if [[ ${#libs[@]} -eq 0 ]]; then echo "At least one LABEL=LIBRARY is required. See -h." >&2; exit 1; fi
if [[ ! -x "$bench" ]]; then echo "mvkbench not found at $bench. Build it first (see README.md) or pass -b." >&2; exit 1; fi
if [[ ${#bench_args[@]} -eq 0 ]]; then bench_args=(all); fi

mkdir -p "$outdir"
echo "Writing logs to $outdir"

for ((r = 1; r <= runs; r++)); do
	for entry in "${libs[@]}"; do
		label="${entry%%=*}"
		lib="${entry#*=}"
		log="$outdir/$label.run$r.log"
		echo "Run $r/$runs: $label ($lib)"
		"$bench" --lib "$lib" "${bench_args[@]}" > "$log" 2>&1 || { echo "mvkbench failed for $label, see $log" >&2; exit 1; }
	done
done

labels=()
for entry in "${libs[@]}"; do labels+=("${entry%%=*}"); done
python3 "$here/compare_results.py" "$outdir" "${labels[@]}" | tee "$outdir/summary.md"
