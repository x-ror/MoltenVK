#!/bin/bash
# Regenerates ../shaders.h from the GLSL sources in this directory.
# Requires glslangValidator (from the Vulkan SDK or https://github.com/KhronosGroup/glslang) on PATH.
set -euo pipefail
cd "$(dirname "$0")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
out=../shaders.h
{
	echo "// Generated from shaders/*.vert and shaders/*.frag with glslangValidator -V --target-env vulkan1.1."
	echo "// Regenerate with shaders/compile_shaders.sh after editing the GLSL sources."
	echo
	echo "#pragma once"
	echo
	echo "#include <cstdint>"
	echo
} > "$out"
for f in draw.vert draw_drawid.vert draw.frag pipeline.vert pipeline.frag; do
	glslangValidator -V --target-env vulkan1.1 -o "$tmp/$f.spv" "$f" > /dev/null
	name="kSPIRV_${f//./_}"
	echo "static const uint32_t ${name}[] = {" >> "$out"
	od -An -v -tx4 -w32 "$tmp/$f.spv" | sed -E 's/^ +//; s/ +/, 0x/g; s/^/\t0x/; s/$/,/' >> "$out"
	echo "};" >> "$out"
	echo >> "$out"
done
