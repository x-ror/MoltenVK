#!/bin/bash
#
# Measures pipeline creation at app launch in separate processes, to show how much Metal's on-disk
# shader cache and a saved VkPipelineCache each save. For every run and library it uses a new seed,
# so the shaders are new to Metal, and runs these phases, each in its own process:
#
#   cold      first launch: nothing cached
#   syscache  second launch: Metal's shader cache is warm, no VkPipelineCache
#   (prime)   third launch: saves a VkPipelineCache to a file, not reported
#   vkcache   fourth launch: restores the VkPipelineCache, Metal's shader cache is warm
#   nosys     fifth launch: restores the VkPipelineCache after clearing Metal's shader cache
#             for mvkbench, as if the OS had evicted it
#
# Results use run_benchmarks.sh's layout, with the phase appended to the variant, and are
# summarized by compare_results.py. See README.md.
#
# Usage: launch_benchmarks.sh [-n RUNS] [-b MVKBENCH] [-o OUTDIR] [-- MVKBENCH_ARGS...] LABEL=LIBRARY [LABEL=LIBRARY...]
#
# Example:
#   ./launch_benchmarks.sh -n 3 -- --pipelines 64 --threads 8 main=/path/to/libMoltenVK.dylib
#
# Metal's shader cache for mvkbench is looked up under $(getconf DARWIN_USER_CACHE_DIR). If it is
# somewhere else, set METAL_CACHE_DIRS to the directories to delete before the nosys phase.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
runs=3
bench="$here/build/mvkbench"
outdir="$here/results/launch-$(date +%Y%m%d-%H%M%S)"
bench_args=()
libs=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		-n) runs="$2"; shift 2 ;;
		-b) bench="$2"; shift 2 ;;
		-o) outdir="$2"; shift 2 ;;
		--) shift; while [[ $# -gt 0 && "$1" != *=* ]]; do bench_args+=("$1"); shift; done ;;
		-h|--help) sed -n '3,24p' "$0"; exit 0 ;;
		*=*) libs+=("$1"); shift ;;
		*) echo "Unknown argument: $1" >&2; exit 1 ;;
	esac
done

if [[ ${#libs[@]} -eq 0 ]]; then echo "At least one LABEL=LIBRARY is required. See -h." >&2; exit 1; fi
if [[ ! -x "$bench" ]]; then echo "mvkbench not found at $bench. Build it first (see README.md) or pass -b." >&2; exit 1; fi

mkdir -p "$outdir/raw"
echo "Writing logs to $outdir"

# Deletes Metal's on-disk shader cache for mvkbench. Returns 1 if none was found.
clear_metal_cache() {
	local dirs=()
	if [[ -n "${METAL_CACHE_DIRS:-}" ]]; then
		for d in $METAL_CACHE_DIRS; do dirs+=("$d"); done
	else
		local base
		base="$(getconf DARWIN_USER_CACHE_DIR 2>/dev/null || true)"
		if [[ -z "$base" || ! -d "$base" ]]; then return 1; fi
		while IFS= read -r d; do dirs+=("$d"); done < <(find "$base" -maxdepth 4 -type d -path "*com.apple.metal*" -iname "*mvkbench*" -prune 2>/dev/null)
	fi
	if [[ ${#dirs[@]} -eq 0 ]]; then return 1; fi
	for d in "${dirs[@]}"; do echo "  clearing Metal shader cache: $d"; rm -rf "$d"; done
	return 0
}

# Runs one phase and appends its results, with the phase appended to the variant, to the run's log.
run_phase() {
	local label="$1" lib="$2" run="$3" phase="$4"; shift 4
	local raw="$outdir/raw/$label.run$run.$phase.log"
	"$bench" --lib "$lib" "${bench_args[@]+"${bench_args[@]}"}" "$@" launch > "$raw" 2>&1 || { echo "mvkbench failed for $label ($phase), see $raw" >&2; exit 1; }
	if [[ "$phase" != "prime" ]]; then
		sed -nE "s/^RESULT launch ([^ ]+) /RESULT launch \1_$phase /p" "$raw" >> "$outdir/$label.run$run.log"
	fi
}

warned=0
for ((r = 1; r <= runs; r++)); do
	for entry in "${libs[@]}"; do
		label="${entry%%=*}"
		lib="${entry#*=}"
		seed=$(( (RANDOM * 32768 + RANDOM) % 7000000 + 1 ))
		cache_file="$outdir/raw/$label.run$r.pipelinecache"
		echo "Run $r/$runs: $label, seed $seed ($lib)"
		rm -f "$cache_file"
		: > "$outdir/$label.run$r.log"
		run_phase "$label" "$lib" "$r" cold     --seed "$seed"
		run_phase "$label" "$lib" "$r" syscache --seed "$seed"
		run_phase "$label" "$lib" "$r" prime    --seed "$seed" --cache-file "$cache_file"
		run_phase "$label" "$lib" "$r" vkcache  --seed "$seed" --cache-file "$cache_file"
		if clear_metal_cache; then
			run_phase "$label" "$lib" "$r" nosys --seed "$seed" --cache-file "$cache_file"
		elif [[ $warned -eq 0 ]]; then
			echo "  warning: Metal's shader cache for mvkbench was not found, so the nosys phase is skipped." >&2
			echo "  Find it with: find \"\$(getconf DARWIN_USER_CACHE_DIR)\" -maxdepth 4 -iname '*mvkbench*'" >&2
			echo "  and pass it in METAL_CACHE_DIRS." >&2
			warned=1
		fi
	done
done

labels=()
for entry in "${libs[@]}"; do labels+=("${entry%%=*}"); done
python3 "$here/compare_results.py" "$outdir" "${labels[@]}" | tee "$outdir/summary.md"
