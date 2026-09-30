#!/bin/bash
#
# Builds MoltenVK at each stage of the performance review's suggested execution order
# (Docs/Performance_Review.md, section 9), then measures and tests every stage on this Mac:
#
#   1. Builds each stage in its own git worktree. The stages share one External directory,
#      so ./fetchDependencies runs only once.
#   2. Runs the mvkbench scenarios against all stages with run_benchmarks.sh, which interleaves
#      the runs. Then it runs the launch scenario with launch_benchmarks.sh.
#   3. If a CTS build is given, runs a CTS case list against each stage with Scripts/runcts.
#      Then it lists the tests that fail at a stage but passed at the stage before it.
#
# Usage: verify_review_stages.sh [options] [LABEL=REVISION ...]
#
#   -w DIR          Directory for the worktrees and results (default: ../mvk-review-stages
#                   beside the repository)
#   -n RUNS         Benchmark runs per stage (default 5)
#   --cts DIR       Directory containing the built deqp-vk. Enables the CTS step.
#   --caselist FILE CTS case list (default: review_cts_caselist.txt beside this script)
#   --skip-build    Use the existing builds in the worktrees
#   --skip-bench    Do not run the benchmarks
#   --dry-run       Print the commands instead of running them
#
# Without LABEL=REVISION arguments, the stages are the commits of the review branch, in the
# order of section 9. Pass your own list to compare fewer stages, for example
#   ./verify_review_stages.sh base=d84f1a3 head=HEAD
#
# The benchmarks take several minutes per stage, and the default CTS case list takes a few hours
# per stage. Run it on a plugged-in Mac with other apps closed, under caffeinate:
#   caffeinate -is ./verify_review_stages.sh --cts ../VK-GL-CTS/build/external/vulkancts/modules/vulkan/Release

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
workdir="$(cd "$repo/.." && pwd)/mvk-review-stages"
runs=5
cts_dir=""
caselist="$here/review_cts_caselist.txt"
skip_build=""
skip_bench=""
dry_run=""
stages=()

# The review branch, one commit per section, in the order of section 9.
default_stages=(
	base=d84f1a3     # The review report only: the code before any change.
	s0=dc3327c       # Section 0: correctness fixes.
	s1=a9112ca       # Section 1: draw, submit and lookup hot paths. Measure with draw.
	s2=b5c67d1       # Section 2: pipeline creation and cache load. Measure with pipelines, cache, launch.
	s3=a192c4a       # Section 3: memory per command pool, resource and device.
	s4=2f19fa6       # Section 4: MVKSmallVector. Run the full CTS.
	s5=4586a88       # Section 5: C++20 adoption.
	s6=1fd53ed       # Section 6: style and dead code, including the draw encoder refactor.
	s7=517b4f3       # Section 7: GPU helper kernels.
	s8=207daf0       # Section 8: extension list and render pass check.
)

while [[ $# -gt 0 ]]; do
	case "$1" in
		-w) workdir="$2"; shift 2 ;;
		-n) runs="$2"; shift 2 ;;
		--cts) cts_dir="$(cd "$2" && pwd)"; shift 2 ;;
		--caselist) caselist="$2"; shift 2 ;;
		--skip-build) skip_build=1; shift ;;
		--skip-bench) skip_bench=1; shift ;;
		--dry-run) dry_run=1; shift ;;
		-h|--help) sed -n '3,31p' "$0"; exit 0 ;;
		*=*) stages+=("$1"); shift ;;
		*) echo "Unknown argument: $1" >&2; exit 1 ;;
	esac
done

if [[ ${#stages[@]} -eq 0 ]]; then stages=("${default_stages[@]}"); fi
if [[ -n "$cts_dir" && ! -f "$caselist" ]]; then echo "CTS case list not found: $caselist" >&2; exit 1; fi
if [[ -n "$cts_dir" ]]; then caselist="$(cd "$(dirname "$caselist")" && pwd)/$(basename "$caselist")"; fi

# Runs a command, or prints it in a dry run.
run() {
	if [[ -n "$dry_run" ]]; then printf '+'; printf ' %q' "$@"; printf '\n'; else "$@"; fi
}

# Runs a command in a directory, or prints it in a dry run.
run_in() {
	local dir="$1"; shift
	if [[ -n "$dry_run" ]]; then printf '+ (cd %q &&' "$dir"; printf ' %q' "$@"; printf ')\n'; else (cd "$dir" && "$@"); fi
}

lib_dir_of() { echo "$workdir/mvk-$1/Package/Release/MoltenVK/dynamic/dylib/macOS"; }

results="$workdir/results/$(date +%Y%m%d-%H%M%S)"
run mkdir -p "$results"
echo "Stages: ${stages[*]}"
echo "Results: $results"

# ---------- Build ----------

if [[ -z "$skip_build" ]]; then
	shared_external=""
	first_rev=""
	for entry in "${stages[@]}"; do
		label="${entry%%=*}"
		rev="${entry#*=}"
		tree="$workdir/mvk-$label"
		if [[ ! -d "$tree" ]]; then
			run git -C "$repo" worktree add --detach "$tree" "$rev"
		else
			run git -C "$tree" checkout --detach "$rev"
		fi

		# Share the dependencies when a stage uses the same external revisions as the first stage.
		if [[ -z "$shared_external" ]]; then
			if [[ ! -d "$tree/External/build" ]]; then
				run_in "$tree" ./fetchDependencies --macos
			fi
			shared_external="$tree/External"
			first_rev="$rev"
		elif git -C "$repo" diff --quiet "$first_rev" "$rev" -- ExternalRevisions fetchDependencies; then
			if [[ ! -e "$tree/External" ]]; then run ln -s "$shared_external" "$tree/External"; fi
		elif [[ ! -d "$tree/External/build" ]]; then
			run_in "$tree" ./fetchDependencies --macos
		fi

		run_in "$tree" make macos
	done
fi

libs=()
for entry in "${stages[@]}"; do
	label="${entry%%=*}"
	lib="$(lib_dir_of "$label")/libMoltenVK.dylib"
	if [[ -z "$dry_run" && ! -f "$lib" ]]; then echo "No build for $label at $lib" >&2; exit 1; fi
	libs+=("$label=$lib")
done

# ---------- Benchmarks ----------

if [[ -z "$skip_bench" ]]; then
	if [[ ! -x "$here/build/mvkbench" ]]; then
		run cmake -S "$here" -B "$here/build"
		run cmake --build "$here/build"
	fi
	# The draw, pipelines and cache scenarios, then the launch scenario in separate processes.
	run "$here/run_benchmarks.sh" -n "$runs" -o "$results/bench" -- all "${libs[@]}"
	run "$here/launch_benchmarks.sh" -n "$(( runs < 3 ? runs : 3 ))" -o "$results/launch" -- --pipelines 64 --threads 8 "${libs[@]}"
fi

# ---------- CTS ----------

if [[ -n "$cts_dir" ]]; then
	prev_label=""
	for entry in "${stages[@]}"; do
		label="${entry%%=*}"
		out="$results/cts/$label"
		run mkdir -p "$out"
		# runcts writes its temporary files and runs its helper from its own directory.
		run cp "$repo/Scripts/runcts" "$repo/Scripts/get_failing_cts_tests.py" "$out/"
		run_in "$out" env VK_ICD_FILENAMES="$(lib_dir_of "$label")/MoltenVK_icd.json" \
			./runcts --cts "$cts_dir" -o "$out/results.txt" -fo "$out/fails.txt" "$caselist"

		if [[ -n "$prev_label" && -z "$dry_run" ]]; then
			prev_fails="$results/cts/$prev_label/fails.txt"
			new_fails="$out/new-fails-since-$prev_label.txt"
			comm -13 <(sort "$prev_fails") <(sort "$out/fails.txt") > "$new_fails"
			echo "$label: $(wc -l < "$out/fails.txt" | tr -d ' ') failures, $(wc -l < "$new_fails" | tr -d ' ') new since $prev_label (see $new_fails)"
		fi
		prev_label="$label"
	done
fi

echo "Done. Results are in $results"
