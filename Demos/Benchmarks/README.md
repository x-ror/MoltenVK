# MoltenVK CPU benchmarks

`mvkbench` is a headless tool that measures the CPU cost of the MoltenVK paths that most affect
frame time and loading time. It loads the Vulkan implementation at runtime from a library path,
so one build of the tool can compare several builds of MoltenVK.

It draws no window and needs no Vulkan SDK or loader to run. The Vulkan headers are needed only
to build it.

## Scenarios

| Scenario | What it measures | Main MoltenVK code involved |
|---|---|---|
| `draw` | `vkQueueSubmit()` of one command buffer with many small draws. With the default `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0`, MoltenVK encodes Metal commands inside `vkQueueSubmit()`, so this is the encoding cost. | `MVKCommandEncoder`, `MVKCommandEncoderState` |
| `pipelines` | Wall-clock time to create many graphics pipelines from 1..N threads that share one `VkPipelineCache`, and the speedup over the first thread count. | `MVKPipelineCache`, `MVKShaderLibraryCache`, `MVKShaderLibrary` |
| `cache` | `vkCreatePipelineCache()` with a populated cache blob, and creating the pipelines from it. | `MVKPipelineCache::readData()` |

The `draw` scenario records the command buffer with three patterns:

- `static` binds the pipeline and descriptor sets once, then changes only push constants between draws.
- `rebind` binds the descriptor sets again before every draw.
- `switch` alternates between two pipelines on every draw.
- `drawid` is `static` with a vertex shader that reads `gl_DrawID`. MoltenVK passes the draw index
  to such shaders separately for every draw. Needs the `shaderDrawParameters` feature.
- `passes` is `static` split into render passes of `--draws-per-pass` draws (default 8), begun
  with `vkCmdBeginRenderPass()` and a `VkFramebuffer` created once.
- `dynpasses` is `passes` with `vkCmdBeginRenderingKHR()` instead. MoltenVK creates its internal
  render pass and framebuffer objects for every dynamic render pass it encodes, so the difference
  from `passes` is mostly that cost. Needs `VK_KHR_dynamic_rendering`.

A pattern whose feature the device lacks is reported as skipped.

The draw fragment shader statically uses 16 combined image samplers and 8 uniform buffers, to
make the per-draw descriptor work in the driver visible. Each draw is a tiny triangle, so GPU time
stays small and does not hide the CPU cost.

The `pipelines` and `cache` scenarios patch a constant in the SPIR-V of every pipeline, starting
from a random value on each run. Every pipeline therefore has distinct MSL, and Metal's on-disk
shader cache cannot serve libraries compiled by an earlier run. `--mode` selects the variety:

- `unique` gives every pipeline its own shader modules.
- `spec` shares one pair of modules, with a distinct specialization constant per pipeline, which
  MoltenVK compiles as a Metal function constant.
- `same` creates identical pipelines.

## Building

Build it once, on the Mac where you will run it. The build looks for the Vulkan headers in
MoltenVK's `External/Vulkan-Headers` (created by `./fetchDependencies`), then in `$VULKAN_SDK`.

```
cd Demos/Benchmarks
cmake -S . -B build
cmake --build build
```

To use other headers, add `-DVULKAN_HEADERS_DIR=<dir containing vulkan/vulkan.h>`.

## Building the MoltenVK versions to compare

Use a separate worktree per version, so the builds don't overwrite each other:

```
git worktree add ../mvk-base 52aa21f      # before the performance changes
git worktree add ../mvk-stageB f059bbb    # after the pipeline creation changes
git worktree add ../mvk-stageC perf/stage-c-resource-dirty-tracking

for d in ../mvk-base ../mvk-stageB ../mvk-stageC; do
    (cd "$d" && ./fetchDependencies --macos && make macos)
done
```

Each build's library is then at `Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib`
in its worktree.

## Running

Run a single library directly:

```
./build/mvkbench --lib ../mvk-stageC/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib all
```

Compare several libraries. The runs are interleaved, so thermal and background drift affects all
of them equally, and the summary shows the median of the runs:

```
./run_benchmarks.sh -n 5 \
    base=../mvk-base/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib \
    stageB=../mvk-stageB/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib \
    stageC=../mvk-stageC/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib
```

To pass options to `mvkbench`, put them after `--` and before the libraries, for example
`./run_benchmarks.sh -n 5 -- pipelines --pipelines 128 --mode spec base=... stageB=...`.

Logs and `summary.md` go to `results/<timestamp>/`. In the summary, `±` is the run-to-run spread,
and a change is labeled `better` or `worse` only when it exceeds the combined spread of the two
libraries. Otherwise it is labeled `noise`.

### Options

```
--lib PATH          Vulkan library to load
--validation        Enable VK_LAYER_KHRONOS_validation (needs a Vulkan loader, not a bare ICD)
--draws N           Draws per command buffer in the draw scenario (default 20000)
--iterations N      Measured submits in the draw scenario (default 20)
--draws-per-pass N  Draws per render pass in the passes and dynpasses draw patterns (default 8)
--pipelines N       Pipelines per measurement in the pipelines and cache scenarios (default 64)
--threads LIST      Thread counts for the pipelines scenario (default 1,2,4,8)
--mode MODE         unique, spec or same (default unique)
--no-cache          Create pipelines without a VkPipelineCache
```

MoltenVK's own configuration applies as usual. For example, `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`
measures the per-slot binding mode, and `MVK_CONFIG_PERFORMANCE_TRACKING=1` with
`MVK_CONFIG_ACTIVITY_PERFORMANCE_LOGGING_STYLE=1` logs MoltenVK's internal timings, such as
`commandBufferEncoding` and `mslCompile`.

## Getting reliable numbers

- Start with an A/A run: pass the same library twice under two labels. Every row should read
  `noise`. If some don't, the machine is too noisy for the change you want to detect, so add runs
  or reduce background load.
- Plug in the Mac, close other apps, and let it cool between long sessions. Apple Silicon changes
  clocks with temperature.
- Use at least 5 runs, so one slow run (for example the first one after a build) does not move the median.
- In the `pipelines` scenario, most of the time is the Metal compiler. The effect of MoltenVK's
  locking shows up in the speedup column rather than in single-thread time.

## Checking correctness

`mvkbench` was validated with `VK_LAYER_KHRONOS_validation` on Mesa's lavapipe, with no validation
messages in any scenario or mode. To validate against MoltenVK, load it through the Vulkan
loader with the validation layer installed, for example from the Vulkan SDK:

```
VK_ICD_FILENAMES=<path to MoltenVK_icd.json> ./build/mvkbench --lib libvulkan.dylib --validation all
```

## Shaders

The GLSL sources are in `shaders/`, and their SPIR-V is embedded in `shaders.h`. After editing a
shader, run `shaders/compile_shaders.sh`, which needs `glslangValidator` on `PATH`. The
`pipeline.*` shaders must each contain exactly one instance of their `kPatchValue` constant,
which `mvkbench` checks at startup.
