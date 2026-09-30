# MoltenVK performance, memory and C++20 review

Review of the `x-ror/MoltenVK` fork at commit `670cc37`, on top of the 1.4.3 work already
recorded in `Whats_New.md` (parallel shader conversion, `MTLFunction` specialization caching,
descriptor bind-script skipping, `MVKPointerMap`, pooled temporary `MTLBuffer`s, inline draw
index). Nothing below duplicates that work.

Every finding was checked against the source. Line numbers refer to commit `670cc37`. Each
item states impact (H/M/L) and risk (H/M/L). "Risk" is the chance of a behavioural regression
if the change is made as described.

Paths are relative to `MoltenVK/MoltenVK/` unless they start with `MoltenVKShaderConverter/`,
`Common/` or `Vulkan/`.

---

## 0. Correctness bugs found on the way (fix before anything else)

These are not optimizations, but they were found while reading the hot paths and each is a
small fix.

**Status:** all items except 0.11 are fixed on this branch. 0.2, 0.6 and 0.7 are fixed by
always taking the lock for map-backed lookups (a shared lock on the read path of the encoding
pool) or by adding a non-inserting `find()` to `MVKInflectionMap`, because an unlocked
`find()` racing a locked insert into a `std::unordered_map` is still a data race. 0.11 is left
unchanged: the same code is in upstream MoltenVK, adding the `break` changes which layered
render passes get `renderTargetArrayLength` set, and that needs a CTS run on real hardware.

| # | Where | Problem | Fix | Impact / Risk |
|---|---|---|---|---|
| 0.1 | `GPUObjects/MVKQueryPool.mm:36-51` and `:185-187` | `endQuery()` holds `_availabilityLock` while calling `encodeCopyResults()`, whose compute path re-locks the same non-recursive `std::mutex`. Reached whenever a `vkCmdCopyQueryPoolResults` with `VK_QUERY_RESULT_WAIT_BIT` was deferred and the copy is not the 64-bit packed direct-copy case. Self-deadlock. | In `endQuery()`, move the ready `DeferredCopy` entries into a local `MVKSmallVector<DeferredCopy,4>` under the locks, release both locks, then encode them. | H / L |
| 0.2 | `Commands/MVKCommandEncodingPool.mm:42-56` | `MVK_ENC_REZ_ACCESS` reads `rezAccess` "without locking", but for the map-backed members (`_cmdClearMTLRenderPipelineStates[attKey]`, `_mtlDepthStencilStates[dsData]`, etc.) that expression is `unordered_map::operator[]`, which inserts and may rehash. Immediate-prefill encoding on the app thread and queue-thread encoding share one pool, so this is a real data race. | Split the macro: unlocked step uses `find()`, locked step uses `try_emplace()`. Also saves one hash per hit. | H / L |
| 0.3 | `Vulkan/vulkan.mm:164-165` | `MVKAddCmdFrom5Thresholds` second branch expands to `##arg1Threshold1 ##arg2Threshold1` instead of `##arg1Threshold2 ##arg2Threshold1`. A `vkCmdBeginRenderPass` with 2 clear values lands in the `<1,0>` command pool and heap-allocates. | Fix the token paste. | L / L |
| 0.4 | `GPUObjects/MVKDescriptorSet.h:502` | `MVKDescriptorPoolFreeList::_freeSize` has no initializer and the pool is built with placement-new, so `allocate()` and `pickOOMError()` read an indeterminate value until the first `vkResetDescriptorPool`. | `size_t _freeSize = 0;` | L-M / L |
| 0.5 | `Utility/MVKEnvironment.cpp:99-105,144` | `_mvkGlobalConfigInitialized = true` is set before `_globalMVKConfig` is filled. Every API entry point reads the config through the trace macros, so a second thread can observe a zeroed config during init. | `static const bool init = (mvkInitGlobalConfigFromEnvVars(), true);` magic static. Same pattern for `getPlatformPixelFormats()` (`Vulkan/mvk_datatypes.mm:35-40`), `mvkOSVersion()` (`Common/MVKOSExtensions.mm:37-44`) and `MVKLayerManager::globalManager()` (`Layers/MVKLayers.mm:100-113`, hand-rolled double-checked locking on a plain pointer). | M / L |
| 0.6 | `GPUObjects/MVKImage.mm:131` | `MVKImagePlane::getMTLTexture(MTLPixelFormat)` does `_mtlTextureViews[mtlPixFmt]` before taking `_image->_lock`: an unlocked map insert. | `find()` on the fast path. | M / L |
| 0.7 | `Utility/MVKInflectionMap.h:94-101` | `getValue()` inserts on a miss, so every `MVKPixelFormats` lookup of an unknown `VkFormat` mutates a table shared per physical device while other threads hold references into it. | Add a non-inserting const lookup returning slot 0 (`VK_FORMAT_UNDEFINED`) on miss; keep the inserting overload for the build phase. | M / L |
| 0.8 | `Utility/MVKSmallVector.h:197,220-257,642,665-698` | The user-declared move constructor deletes the implicit copy assignment, and the deleted non-template beats the `template<typename U> operator=(const U&)`, so `b = a;` fails to compile for any `MVKSmallVector` (verified with clang 20 in `-std=c++20`). Also `rend()` of the pointer specialization (`:739`) returns `reverse_iterator(rbegin())`, so reverse iteration is empty. Both are latent (no caller today). | Real `operator=(const MVKSmallVectorImpl&)`; `rend()` returns `reverse_iterator(begin())`. Mark move ctor/assign/`swap` `noexcept`. | M / L |
| 0.9 | `GPUObjects/MVKDeviceMemory.mm:371-374` | `case VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT` falls through into `MEMORY_ALLOCATE_FLAGS_INFO` and reads `flags` from the wrong struct. | Add `break`. | L / L |
| 0.10 | `Commands/MVKCmdRendering.mm:144` | `for (auto caAtt : _colorAttachments) caAtt.pNext = nullptr;` iterates by value; the stored copies keep dangling application `pNext` pointers. | `for (auto& caAtt : ...)`. | L / L |
| 0.11 | `Commands/MVKCommandBuffer.mm:811-819` | `switch (textureType) { case 3D: found3D = true; default: found2D = true; }` has no `break`, so `found2D` is always set and the 3D/2D mix check reduces to `!found3D`, while still issuing up to 24 ObjC calls per Metal render pass. | Confirm intent, then either add `break` (behaviour change, needs CTS) or compute the flag once from the subpass attachment types. | L / M |
| 0.12 | `GPUObjects/MVKQueue.mm:347-353` | `_mtlCmdBuffLabelCopyImageToMemory` is the only one of eight labels never released. | Release it. | L / L |
| 0.13 | `Vulkan/mvk_datatypes.mm:737-742` | `mvkMTLIndexTypeSizeInBytes` has no `default:` and falls off the end for an out-of-range enum. | Add `default`. | L / L |

---

## 1. Hot-path performance (per draw, per submit, per frame)

**Status:** 1.1 through 1.10 are implemented on this branch, with these scope notes. 1.4 caches
the command buffer descriptors but keeps the separate error-checking completion handler, since
folding it into the other handlers would have to cover every caller of `getMTLCommandBuffer()`.
1.10 adds the early-out before formatting in `reportResult`; the log macros still evaluate
their arguments. In 1.11, the temp-buffer move, the atomic ordering, the device threadgroup
limit, the namespace-scope binder tables, the hoisted first-view index, and the templated
rendering attachment iterator are done; the multiview pipeline-state map and the view-class
capability precompute are not, as neither is on a per-draw path.

### 1.1 `getMVKConfig()` is a three-level virtual chain on the per-draw path (M / L)
`Utility/MVKBaseObject.mm:34-38` resolves the config through `getVulkanAPIObject()` (virtual),
`getInstance()` (virtual) and `MVKInstance::getMVKConfig()` (virtual). It is called twice per
draw in `bindMetalResources()` (`Commands/MVKCommandEncoderState.mm:688`, on the fast path the
1.4.3 skip added), in `bindStateData()` (`:1250`), `bindState()` (`:1419`),
`prepareHelperDraw()` (`:1585`), `setMetalObjectLabel()` (`GPUObjects/MVKVulkanAPIObject.mm:42`,
on every Metal encoder creation), and 18 more sites in `MVKQueue`, `MVKSwapchain` and
`MVKImage`.

Change: cache `const MVKConfiguration* _pMVKConfig` in `MVKDevice` at creation (the config is
immutable after instance creation) and give `MVKDeviceTrackingMixin` and `MVKCommandEncoder` a
non-virtual accessor. Hoist `debugMode`, `useMetalPrivateAPI` and the capture-queue indices
into plain `bool` members where they are read per draw or per submit.

### 1.2 `MVKDevice::_resources` is a flat vector with O(N) erase per destroy (H for large apps / L)
`GPUObjects/MVKDevice.h:1099`, `MVKDevice.mm:4620-4667`, via `mvkRemoveFirstOccurance`
(`Utility/MVKFoundation.h:587-594`). Every buffer and image goes into one device-wide vector
under `_rezLock`; destroy is a linear find plus tail shift, so destroying N resources is O(N²)
under a global mutex. The list only serves `applyMemoryBarrier()` host-read sync
(`MVKDevice.mm:4689-4698`), which is a no-op on unified-memory GPUs.

Change: store an intrusive `uint32_t _deviceResourceIndex` in `MVKResource` and remove with
swap-with-last (O(1)), or skip registration entirely when `isUnifiedMemoryGPU()`.

### 1.3 Timeline semaphore host waits allocate a listener and dispatch queue per wait (M-H / L)
`GPUObjects/MVKSync.mm:311-315`: `MVKFenceSitter` creates `[MTLSharedEventListener new]`
(which creates a private serial dispatch queue) for every unsatisfied `vkWaitSemaphores`, and
the sitter is a stack temporary. The `// TODO: Use dispatch queue from device?` is already
there.

Change: one lazily created `MTLSharedEventListener` owned by `MVKDevice`.

### 1.4 Per-submit allocations and lock ping-pong in `MVKQueue` (M / L)
- `GPUObjects/MVKQueue.h:224,272`: `_waitSemaphores` / `_signalSemaphores` are
  `MVKSmallVector<..., 0>`, so the common one-wait-one-signal submit does two mallocs. Use
  inline capacity 2.
- `MVKQueue.mm:87-97,152-157`: `_execQueueJobCount` is guarded by a mutex plus condvar that
  the app thread and the dispatch thread bounce on every submit. C++20
  `std::atomic<uint32_t>` with `wait()`/`notify_all()` replaces both.
- `MVKQueue.mm:166-185,548-552`: every `MTLCommandBuffer` gets a fresh
  `MTLCommandBufferDescriptor` and two completion-handler blocks. Cache two descriptors on the
  queue and fold the error check into the single completion handler.

### 1.5 `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` allocate per lookup (M at startup / L)
`GPUObjects/MVKInstance.h:214` keys `_entryPoints` by `std::string`; `MVKInstance.mm:36-39`
builds a temporary string per lookup (most Vulkan names exceed the 22-byte SSO). `isEnabled`
then does up to four linear `strcmp` scans over all 160 extensions
(`Layers/MVKExtensions.mm:134-146`). The 370-entry table is rebuilt per instance.

Change: C++20 heterogeneous lookup (`std::hash<std::string_view>` + `std::equal_to<>`), store
the extension ordinal in `MVKEntryPoint` instead of a name, and build the table once as a
function-local static.

### 1.6 Bit loops on the temporary-buffer allocation path (M / L)
`Utility/MVKFoundation.h:161-186`: `mvkEnsurePowerOfTwo` and `mvkPowerOfTwoExponent` loop up
to 64 times and are called for every temp `MTLBuffer` acquisition
(`Commands/MVKMTLBufferAllocation.mm:149,158`) and in `MVKImage.mm:1263`. The header already
includes `<bit>`.

Change: `std::bit_ceil`, `std::bit_width(v - 1)`, `std::has_single_bit`; keep the documented
"0 and 1 give 0" behaviour with a `static_assert`. Also `mvkMipmapLevels`
(`Vulkan/mvk_datatypes.mm:286-295`) and `mvkGetNextViewMaskGroup`
(`GPUObjects/MVKRenderPass.mm:1230-1247`, which has a `TODO: make this faster` and a UB shift
for a zero mask) become `std::countr_zero` / `std::countr_one`.

### 1.7 `MVKGraphicsPipeline::getStages()` fills a small vector on every draw (L-M / L)
`GPUObjects/MVKPipeline.mm:500-506`, called at `Commands/MVKCmdDraw.mm:191,460,964,1339`.
Return `MVKArrayRef<const MVKGraphicsStage>` over one of two `static constexpr` arrays.

### 1.8 `executeBindOps()` re-derives set and layout per op (M for descriptor-heavy content / L)
`Commands/MVKCommandEncoderState.mm:553-561` looks up `_descriptorSets[op.set]`,
`getDescriptorSetLayout(op.set)`, `bindings()[op.bindingIdx]` and a `descriptorCPUSize` switch
for every op, although ops are emitted grouped by set. Track the last set across iterations
and refetch only on change; optionally precompute the CPU stride into the free byte of
`MVKDescriptorBindOperation`.

### 1.9 Trace macros cost two config loads and switches per API call when tracing is off (L-M / L)
`Vulkan/vulkan.mm:50-104`, used by all 318 entry points. Split into an inline `[[likely]]`
early-return guard and a `__attribute__((cold, noinline))` body, and have Start return a mode
token so End does not re-read the config.

### 1.10 Logging evaluates arguments and formats before the level check (L-M / L)
`Utility/MVKBaseObject.mm:55-63,112-127`: `reportResult` does `strlen`, a VLA and `snprintf`
before `reportMessage` checks the level, and the level check itself costs two virtual calls.
Add an inline `shouldLog(level)` and make the `MVKLog*` macros `if (shouldLog(l)) [[unlikely]]
reportMessage(...)` so arguments such as `.UTF8String` are not evaluated.

### 1.11 Smaller per-draw / per-pass items (L / L)
- `Commands/MVKCmdDraw.mm:406-407`, `MVKCmdTransfer.mm:1725-1726`: two `objc_msgSend`s (one
  returning `MTLSize`) per UINT8-indexed draw and per fill; cache the threadgroup width next to
  the pipeline state in `MVKCommandEncodingPool`.
- `Commands/MVKCommandEncoderState.mm:175-183`: function-local static tables cost a guard
  check per call, three per draw; hoist to namespace scope or template on the binder type.
- `Commands/MVKCommandBuffer.mm:1211`: `new MVKSmallVector(...)(_tempMTLBufferAllocations)`
  copies; `std::move` it.
- `Commands/MVKCommandBuffer.mm:241,318,332`: `std::atomic_flag` with default `seq_cst`; use
  acquire/release (removes a `dmb ish` on arm64).
- `GPUObjects/MVKPipeline.h:260-262,386`: multiview pipeline states in an `unordered_map` with
  1-3 keys; a `MVKSmallVector<pair,4>` with linear scan is cheaper and saves an allocation.
- `GPUObjects/MVKRenderPass.mm:188-277`: `getFirstViewIndexInMetalPass()` is recomputed per
  attachment; hoist once per pass.
- `GPUObjects/MVKRenderPass.h:386-417`: `MVKRenderingAttachmentIterator` takes `std::function`
  by value per attachment and inherits `MVKBaseObject` for nothing; make `iterate` a template
  over the callable. This is the only `std::function` on the encode path.
- `GPUObjects/MVKPixelFormats.mm:339-348`: `getCapabilities(mtl, isExtended=true)` scans all
  ~260 descriptors per image/view creation; precompute per-view-class OR'd caps.

---

## 2. Pipeline creation and cache load

**Status:** 2.1 through 2.4 are implemented on this branch. 2.4 keeps the `std::string` keys of
the reflection maps, since reflection runs once per stage per pipeline and the map lookup is
not measurable next to the conversion; the copy into the small vector is now reserved. 2.5
is deliberately not done: it changes the on-disk pipeline cache format and needs a
`pipelineCacheUUID` bump, which is a maintainer decision.

### 2.1 Whole conversion config deep-copied on every shader lookup, including cache hits (M-H / L)
`GPUObjects/MVKShaderModule.mm:450-451` copies `SPIRVToMSLConversionConfiguration` (five
vectors and a string, `resourceBindings` is stage-count × bindings entries of ~136 B with
argument buffers) before the first `findShaderLibrary()`. Make it a `std::optional` populated
only after the first miss.

### 2.2 Pipeline-cache load copies every config, result and compressed MSL twice (M / L)
`GPUObjects/MVKPipeline.mm:2695-2707` → `MVKShaderModule.mm:541-548` → constructor
`:304-320`: `readData` builds locals, passes them by `const&`, the constructor copy-assigns,
and `emplace_back(*pShaderConfig, shLib)` copies the config again. Add rvalue overloads and
`std::move` through. Same for `MVKMTLFunction` (`MVKShaderModule.h:51`: result info taken
**by value** then copy-assigned; no implicit move because copy ops are user-declared; copied
three times per stage).

### 2.3 `matches()` / `alignWith()` are O(n·m) over a stage-inflated binding list (M / L)
`MoltenVKShaderConverter/MoltenVKShaderConverter/SPIRVToMSLConverter.cpp:191-246`,
`MVKPipeline.mm:186-213`. With argument buffers every binding is pushed once per stage, and
`findShaderLibrary` compares against every cached library with a linear scan and `memcmp` per
binding. Skip entries whose stage differs before comparing, try index-aligned comparison first
(configs from the same layout are in identical order), `break` after the first match in
`alignWith`, and `reserve()` in `populateShaderConversionConfig`.

### 2.4 Specialization and reflection micro-costs (L-M / L)
- `MVKShaderModule.mm:81-84,99-113`: `getMacroSpecializedVariant` builds a vector and does map
  lookups for every specialized fetch even when `specializationMacros` is empty. Early-out.
- `MVKShaderModule.mm:171-195`: `getSpecializedMTLFunction` copies a `std::string` under the
  lock and does an ObjC-property nested loop; cache a sorted vector of constant index/type and
  `reserve` the key.
- `MVKShaderModule.h:393-399`, `.mm:657,671`: interface reflection is copied per stage into a
  `MVKSmallVector` (no `reserve`) and keyed by a freshly allocated `std::string`. Return
  `MVKArrayRef<const ...>` for the three read-only consumers and use `std::map<Key,V,std::less<>>`
  with a `string_view` comparator.
- `MVKPipeline.mm:2505-2512`: `_shaderCache[smKey]` twice; use `try_emplace`.
- `SPIRVToMSLConverter.cpp:252-258`: `setSPIRV` push_back loop → `assign`.

### 2.5 Cereal serializes a ~100 B `MSLConstexprSampler` for every binding of every stage (M / M)
`MVKPipeline.mm:2925-2931`. Writing it only when `requiresConstExprSampler` is set would shrink
caches substantially, but changes the on-disk format and needs a `pipelineCacheUUID` bump.
Flagged for a deliberate decision rather than recommended outright.

---

## 3. Memory

**Status:** implemented on this branch: 3.1, 3.2, 3.3, 3.4, 3.7, 3.8, the `MVKPipelineBarrier`
reordering and the by-reference device capabilities from 3.5, and the barrier fences and
environment variable lookup from 3.6. The fences are skipped unless the device uses Metal
argument buffers and a residency set, which are the only paths that read them. Rewriting 3.8
found that the data of a pushed inline uniform block was never copied, so it is now copied too.
Not done: the `MVKPointerMap` slot reordering, because reordering alone keeps the slot at
24 bytes; the `MVKVkFormatDesc` and device-capability bitmask restructuring, and the
`constexpr` extension table, which are larger refactors of startup-only data.

### 3.1 Every `VkCommandPool` creates ~90 `MVKMTLBufferAllocationPool`s (~100 KB) up front (M / L)
`Commands/MVKCommandEncodingPool.mm:183-187`, `MVKMTLBufferAllocation.mm:158-166`: one pool per
power-of-two exponent up to `maxBufferLength` (exponent 34-36 on Apple silicon), each with a
1 KB inline tracker vector and a 64 B mutex. Exponents below the buffer alignment can never be
used. Create pools lazily, shrink the inline tracker to 4, and skip the unusable exponents.

### 3.2 112 `std::mutex`es per `VkCommandPool` that nobody locks (M / L)
`Utility/MVKObjectPool.h:184` embeds a 64 B mutex in every pool; `Commands/MVKCommandPool.h:160-161`
instantiates 112 pools. `acquireObjectSafely`/`returnObjectSafely` have no callers anywhere.
Move the lock into a `MVKThreadSafeObjectPool` subclass used only by
`MVKMTLBufferAllocationPool`. Saves ~13 KB and 112 `pthread_mutex_init` per pool.

### 3.3 64 B `std::mutex` in every buffer, image, view and device memory (M memory / L)
`GPUObjects/MVKBuffer.h:98,140`, `MVKImage.h:391,433,514,662`, `MVKDeviceMemory.h:167`,
`MVKSync.h:344,398`. These guard lazy creation or short list edits. Hoist the
`os_unfair_lock` wrapper already in `MVKDevice.h:554-561` into `Utility/` and use it (4 B).

### 3.4 Small containers used for one or two elements (L-M / L)
- `MVKSync.h:345,399`: `std::unordered_set` of fence sitters (≤2 elements) with a heap node per
  `vkWaitForFences`; use `MVKSmallVector<..., 2>`.
- `MVKImage.mm` `_mtlTextureViews`: `unordered_map<NSUInteger,id>` for 1-2 view formats.
- `MVKDescriptorSet.h:495-512`: free-list `std::vector<size_t>` and `pair<size_t,size_t>` where
  every consumer is `uint32_t`.
- `MVKDescriptorSet.mm:2263-2265`, `MVKPipeline.mm:81-145,2150-2240`: push_back loops with
  known counts and no `reserve`.

### 3.5 Layout and padding (L / L)
- `Commands/MVKMTLResourceBindings.h:59-80`: `MVKPipelineBarrier` is 72 B with 8 B of padding
  from `uint8_t` fields placed first; reordering gives 64 B and shrinks the
  `MVKCmdPipelineBarrierMulti` inline storage from 2304 to 2048 B.
- `Utility/MVKPointerMap.h:103-107`: `Slot` is 24 B with 8 B padding; put `generation` after
  `value`.
- `GPUObjects/MVKPixelFormats.h:133-163`: `MVKVkFormatDesc` carries unused `sType`/`pNext`
  (16 B) and a `bool` after a pointer; ~28 KB per physical device plus a second copy for the
  `mvk_datatypes` static instance. Store the three feature-flag words directly and move the
  immutable columns to a `static constexpr` table.
- `GPUObjects/MVKDevice.h:104-132`: `MVKMTLDeviceCapabilities` (21 B of bools) is returned
  **by value** from `getMTLDeviceCapabilities()` (`:439`); make it `const&` and consider a
  bitmask so `getHighestAppleGPU()` becomes `std::bit_width`.

### 3.6 Eager per-device objects (L / L)
- `MVKDevice.mm:5195-5203`: 256 `MTLFence`s and 256 formatted `NSString` labels per `VkDevice`,
  only consumed on the argument-buffer path; create lazily. The declaration
  `getBarrierStageFence(id<MTLCommandBuffer>, MVKBarrierStage)` at `MVKDevice.h:1046` has no
  definition.
- `Layers/MVKExtensions.mm:47-106`: extension list construction is O(N²) over 160 entries and
  runs for the layer, each instance, each physical device, each device, and once more just to
  log. Index a `static constexpr` platform table by ordinal.
- `Common/MVKOSExtensions.mm:81-97`: `mvkGetEnvVar` builds an `NSDictionary` from `environ` for
  each of ~45 config lookups; `getenv()` does the job.

### 3.7 Unbounded VLAs on the stack (L / L)
`Commands/MVKCmdTransfer.mm:139-140,486-487,812-827,1387` (clear-attachment vertices can reach
196 KB for a 2048-layer image, then get copied again into a temp buffer),
`GPUObjects/MVKDevice.mm:2155,2297`, `Utility/MVKFoundation.cpp:148`. VLAs are a non-standard
extension in C++20. Use `MVKSmallVector<T, N>` or write directly into the temp buffer.

### 3.8 Push descriptor recording allocates up to four arrays per write (M for push-heavy engines / L-M)
`Commands/MVKCmdPipeline.mm:404-438,453-473`: pooled command reuse frees and reallocates. Use
a single `MVKSmallVector<uint8_t,256>` arena sized in a first pass, then patch the info
pointers into it.

---

## 4. `MVKSmallVector` and the utility containers

**Status:** 4.1 through 4.7 are implemented on this branch; 4.8 is left for incremental
migration. The iterators are now pointers, but a `const` vector still hands out mutable
iterators as before, since tightening that could break callers that can only be checked by an
Xcode build. Testing the container against `std::vector` found that erasing an empty range
from the middle moved every following element onto itself, which empties self-moved strings;
an empty range is now a no-op.

`MVKSmallVector` is used in about 116 places, most of them on the encode path, so it deserves
its own section.

| # | Where | Problem | Change | Impact / Risk |
|---|---|---|---|---|
| 4.1 | `Utility/MVKSmallVector.h:66-117,530-580` | Iterator is an (owner pointer, index) pair: every `*it` reloads `alc.ptr` through the owner, and `==` compares two words. Range-for over any small vector pays this. | `using iterator = Type*;` (`begin()` returns `alc.ptr`). `erase`/`insert` use `it - alc.ptr` internally; nobody outside the header uses `is_valid()`/`get_position()`. Also fixes 0.8's `rend()`. | M / M (compile-time only) |
| 4.2 | `Utility/MVKSmallVectorAllocator.h:158-177,290-307`, `MVKSmallVector.h:377-418` | Reallocation, copy and stack-swap move element by element; `swap_stack` for trivial types swaps **byte by byte**; the `if constexpr` fast paths are commented out. | `if constexpr (std::is_trivially_copyable_v<T>) memcpy(...)`; trivial `resize` shrink just sets the count. | M / L |
| 4.3 | `MVKSmallVectorAllocator.h:87-167` | Six `std::enable_if` overload pairs for construct/destruct/swap. | Single functions with `if constexpr` (C++20 style, fewer instantiations). | L / L |
| 4.4 | `MVKSmallVector.h:504` | `std::forward<Type>(t)` on an rvalue-reference parameter. | `std::move(t)`. | L / L |
| 4.5 | `Utility/MVKBitArray.h:370-399` | No move constructor; copies always `malloc`+`memcpy`; `resizeAndClear` `calloc`s then may `memset 0xff`. | `noexcept` move ops (re-point the inline case at own `_capacity`); `malloc` + one `memset`. | L / L |
| 4.6 | `Utility/MVKFoundation.h:620-624` | `mvkClear` fast path for one arithmetic value falls through to `memset` anyway. | Add `return`; `static_assert(std::is_trivially_copyable_v<T>)` in `mvkClear`, `mvkCopy`, `mvkAreEqual`. | L / L |
| 4.7 | `MVKFoundation.h:441-444` | `mvkClamp` returns `const T&` to possibly-temporary arguments. | `std::clamp`. | L / L |
| 4.8 | `MVKFoundation.h:523-548` | `MVKArrayRef` already converts to and from `std::span`. | Migrate new code to `std::span`; give `mvkHash` a `std::span<const N>` overload. Full replacement is large and not worth the churn now. | L / M |

---

## 5. C++20 features worth adopting

**Status:** implemented on this branch, with these scope notes. `std::span`/`MVKArrayRef` is
adopted for the encoder-state descriptor binding and push descriptor functions, whose callers
already hold containers; the fence, queue, extension and timestamp functions stay as they are,
because each is a thin wrapper with one caller passing Vulkan's own count and pointer through.
For `std::bit_cast`, the UUID writes were real strict-aliasing violations and now store bytes
with `memcpy`; the two reads in `MVKCommandEncoderState.mm` go through pointers to objects of
the right type and are well-defined, so they stay. `mvkAreEqual` keeps its `constexpr`, since
removing it could break `constexpr` callers. Not done: designated initializers for the constant
subsets of `supportedProps12/13/14`, a larger restructuring of device property setup.
Converting `mvk::trim()` to `std::string_view` found that it kept trailing whitespace whenever
the string also had leading whitespace; that is fixed.

The project already compiles as C++20 and uses `requires`, `std::span` (3 sites),
`std::popcount`/`std::countr_zero` (3 sites), `[[likely]]` (6 sites) and designated
initializers (a few). The list below names the concrete places where each feature pays for
itself. None changes behaviour.

**`<bit>`** (also a performance win): 1.6 above; `mvkIsPowerOfTwo` → `std::has_single_bit`;
`MVKDevice.mm:1334` `mvkPowerOfTwoExponent` → `std::countr_zero`.

**`[[likely]]` / `[[unlikely]]`** on lazy-init and degenerate-input guards:
`MVKCmdDraw.mm:174,183,211,443,452,491,882,1034,1255,1420`,
`MVKCommandEncoderState.mm:687-694,1475,1639,1670`, `MVKCommandBuffer.mm:285,1109,1133`,
`MVKBuffer.mm:152`, `MVKImage.mm:59,1892`, `MVKQueue.mm:487`, `MVKDevice.mm:4936`,
`MVKPipeline.mm:1001,1014`, `MVKShaderModule.mm:245`, `MVKObjectPool.h` `acquireObject`.

**Heterogeneous lookup** (`std::hash<std::string_view>` + `std::equal_to<>`,
`std::map<K,V,std::less<>>`): 1.5 and 2.4 above.

**`std::atomic<T>::wait` / `notify_all`**: `MVKQueue` job counter (1.4). Do not use it where a
timeout is needed (fences keep their condvar).

**Defaulted comparisons (`= default`, `<=>`)**: `MVKCommandEncoderState.h:255-256` (`Buffer`
via `std::make_pair`), `MVKCommandResourceFactory.h:51-60,266-268`, `MVKStateTracking.h:47-48`,
`MVKShaderModule.h:274-283` (`MVKShaderModuleKey`), the reflection keys at
`MVKShaderModule.mm:657,671`.

**`using enum`**: the bind-op switches at `MVKCommandEncoderState.mm:427-436,457-539,572-596`
(removes the `CASE(x)` macro), `bindState()` `:1280-1448`, `prepareHelperDraw()` `:1512-1531`.

**Designated initializers**: `MVKCmdTransfer.mm:65-70,351-355,713-718,757-762,990-993,1034-1037,
1083-1087,1440-1445` (positional `Vk*2` aggregates and declare-then-assign),
`MVKCommandEncoderState.mm:916,930`, `MVKCommandBuffer.mm:991-994`, the constant subsets of
`supportedProps12/13/14` in `MVKDevice.mm:851-983`, `MVKDescriptorBindOperation` pushes at
`MVKPipeline.mm:288-333`.

**`constexpr`** on pure switch helpers: `MVKCommandBuffer.mm:611,1081,1090,1099`,
`MVKCommandEncoderState.mm:41,260,278,287,612-630,901,1089`, `MVKPipeline.mm:54-79,220-251`,
`MVKDescriptorSet.mm:126-163`, `MVKCmdDraw.mm:638,1149`, `MVKCommandResourceFactory.h:116-120`;
`MVKFoundation.h:39-50` (`KIBI/MEBI/GIBI`, `kHalfFloat1` macros → `inline constexpr`),
`:368-374,704-717,822-825`. Conversely, `mvkAreEqual` and `mvkStringsAreEqual` (`:660,674`)
are marked `constexpr` but call `memcmp`/`strcmp`; use `std::equal` / `std::string_view`
equality, which are constexpr in C++20.

**`if constexpr` / concepts replacing SFINAE**: 4.3; `MVKFoundation.h:238-266`
(`mvk_define_has_member` + tag dispatch → `requires requires(S s) { s.pNext; }`), `:455-470`
(`MVKAbs` partial specializations → `if constexpr (std::is_signed_v<T>)`),
`MVKInlineObjectConstructor.h:124-136`; `std::is_base_of<>::value` → `_v`,
`std::common_type<>::type` → `_t` (`MVKFoundation.h:448-495`). Constrain
`applyToActiveMTLState` (`MVKCommandEncoderState.h:529-530`) with `std::invocable`.

**`std::span` / `MVKArrayRef` for pointer+count pairs (internal signatures only)**:
`MVKCommandEncoderState.h:210-229,503-511` (`bindDescriptorSets`, `pushDescriptorSet`),
`MVKExtensions.h:80`, `MVKSync.h:520-530`, `MVKQueue.h:97`, `MVKDevice.h:715`.

**`std::bit_cast`**: `MVKCommandEncoderState.mm:448,469` (pointer punning),
`MVKDevice.mm:1403-1412,1439-1474,3299-3311` (`*(uint32_t*)&uuid[off]` writes into `uint8_t`
arrays: strict-aliasing UB today).

**`std::string_view` / `inline constexpr`**: `Common/MVKStrings.h:32` (per-TU static
`std::string`), `trim*` helpers; `MVKSync.h:581` `_compilerType` (one heap string per compile,
only used in log formats); `MVKShaderModule.h:63` `MVKMTLFunctionNull` (a namespace-scope const
object with a `std::string` and a `std::map` instantiated in every including TU → `inline`).

**`std::erase_if` / `std::copy_if`**: `MVKPipeline.mm:1230-1232,1322-1324`.

Deliberately not recommended: `std::format` (needs macOS 13.3 / iOS 16.3 runtime, above the
minimum target), `std::jthread` (the project uses GCD, not `std::thread`), modules, coroutines,
and replacing the dense enum-to-enum switches in `mvk_datatypes.mm` with tables (clang already
emits lookup tables for them).

---

## 6. Style and dead code

- `MVKCommandBuffer.h:564` `_flushCount` is write-only; `MVKCommandBuffer.h:503`
  `_mtlThreadgroupSize` unreferenced; `MVKPipeline.h:209` `_stageUsesPushConstants` written,
  never read; `MVKPipeline.mm:2551` `_count` unused; `MVKCommandEncoderState.mm:421-425`
  `updateImplicitBuffer` appears to have no callers after the bind-script rewrite.
- `MVKCommandBuffer.h:205-230`: the secondary-inheritance input-attachment-index vectors are
  filled in `begin()` and cleared in `reset()` but never read; `beginSecondaryEncoding()` only
  applies colour attachment locations. Either a missing `updateAttachmentInputIndices()` call
  for secondary buffers inheriting `VkRenderingInputAttachmentIndexInfo`, or 40 lines of dead
  state. Needs a maintainer decision.
- `MVKCommandEncoderState.h:177-178`, `.mm:1004-1017`: `MVKVulkanCommonEncoderState` copy
  ctor / `operator=` alias the other object's push-descriptor buffer and have no callers;
  `= delete` them.
- `MVKCmdDraw.mm:172-342,441-631,857-1135,1211-1527`: four 280-320 line draw encoders whose
  tessellation-control and tessellated-rasterization bodies are copies differing only in
  dispatch arguments. Extract `bindTessCtlStageBuffers`, `computeTessCtlWorkgroupSize`,
  `bindTessEvalStageBuffers` helpers. Pure refactor, but needs a CTS run.
- `MVKCmdTransfer.mm:952-953`: back-to-back `pushDebugGroup`/`popDebugGroup` per resolve slice.
  `:1121-1129`: `hasExpectedTexelSize()` evaluated per region. `:1770-1771`: `reserve()` then
  `memcpy` into `data()` with size 0; use `assign`. `:1102-1116`: `resize` then `memcpy`; use
  `assign`.
- `MVKDescriptorSet.mm:428-440,612-626`: `qsort` with a C comparator and an O(n²) immutable
  sampler gather; `std::sort` with a lambda and a recorded original index.
- `MVKCommandBuffer.mm:1378-1394`: constructor assigns 12 members in the body and leaves eight
  indeterminate until `beginEncoding()`; use default member initializers.
- `typedef struct X {...} X;` at `MVKCommandBuffer.h:56-89,545-548`,
  `MVKMTLResourceBindings.h:32-175`, `MVKCmdDraw.mm:675-681,1141-1150`, `MVKCmdTransfer.h:81-84,
  133-136`.
- `Commands/MVKCommandPipelineStateFactoryShaderSource.h` has no `#pragma once`.
- `MVKPipeline.mm:29`, `MVKDescriptorSet.mm:28`: `#include <sstream>` unused.
  `MVKFoundation.h` includes `<string>` only for `mvkGetVulkanVersionString`.
- `MVKPipeline.mm:3110-3128`: `mvkValidateCerealArchiveSize` hard-codes padding constants and
  uses `printf`; `static_assert(std::is_standard_layout_v<...>)` plus a test is safer.
- `MVKCodec.mm:51-52`: `instancesRespondToSelector:` check for an API available since
  macOS 10.15 / iOS 13, below the minimum target.
- `MVKConfigMembers.def:64-65,86`: member types differ from `mvk_private_api.h:224-225,247`
  (`VkBool32` vs `uint32_t`, `char*` vs `const char*`).
- `Common/MVKStrings.h:54-62`: `isalpha` on `char` (UB for negative values); cast to
  `unsigned char`.
- Sign-compare loops at `MVKQueue.mm:729`, `MVKSync.mm:524`.

---

## 7. GPU-side helper kernels

Outside the CPU focus of this review but noticed in
`Commands/MVKCommandPipelineStateFactoryShaderSource.h`:

- `:103-109` with dispatch at `MVKCmdTransfer.mm:1046`: `cmdCopyBufferBytes` copies unaligned
  `vkCmdCopyBuffer` regions with one GPU thread and a byte loop. One thread per byte, dispatched
  over `size` threads, is the standard fix. (H for large misaligned copies / L-M)
- `:123-204`: array clear and resolve kernels loop over every slice inside each (x, y) thread.
  A 3-D grid with `pos.z` as the slice parallelizes layered clears. (M / M, dispatch sites in
  `MVKCmdTransfer.mm` must change.)

---

## 8. Architectural suggestions

These are larger than a local edit and are offered for discussion, with the trade-off stated.

1. **Command stream as a contiguous arena instead of a linked list of pooled objects.**
   Every recorded command is a separately heap-allocated (then pooled) object with a vtable,
   an intrusive `_next` pointer and a virtual `getTypePool()` used on release
   (`Commands/MVKCommand.h`, `MVKCommandBuffer.mm:194-206,284-303`). Encoding walks the list
   with a virtual `encode()` per node, and pool return does a virtual call per command. A
   per-command-buffer bump arena (reset on `vkResetCommandBuffer`) with trivially destructible
   command payloads and an opcode dispatch would make replay cache-linear and make reset O(1).
   Cost: a large refactor of `MVKCommandTypePools.def` and every `MVKCmd*` class, and the
   variable-size commands (`MVKSmallVector` members) would need to spill into the same arena.
   The `draw` scenario in `Demos/Benchmarks` measures exactly this path, so the gain can be
   quantified before committing.

2. **Split thread-safety out of `MVKObjectPool`.** See 3.2. Most pools are externally
   synchronized by the Vulkan spec; only the buffer allocation pools need a lock.

3. **Cache configuration and device capabilities in plain fields.** See 1.1. A single
   `const MVKConfiguration&` and a handful of bools on `MVKDevice` / `MVKCommandEncoder`
   remove the virtual chain from every hot site at once, and make the intended immutability of
   the config explicit.

4. **Precompute per-render-pass Metal state in `MVKRenderSubpass`.** Several per-pass costs
   (3D/2D attachment mix, first view index per attachment, attachment count for the rendering
   iterator) are re-derived from Metal objects or via `std::function` callbacks on every
   `vkCmdBeginRenderPass`/`vkCmdBeginRendering`. The subpass already knows the attachment
   types and view masks and can store the answers once.

5. **Make `MVKPixelFormats` and `MVKExtensionList` mostly `constexpr`.** Both are rebuilt per
   physical device / instance / device from `.def` tables at runtime (3.5, 3.6). The immutable
   columns can live in `static constexpr` arrays generated from the same `.def` files, leaving
   only the per-GPU capability columns as instance state.

6. **Adopt `std::span` at the internal API boundary.** `MVKArrayRef` is already
   span-convertible. Passing `std::span` (or `MVKArrayRef`) instead of pointer+count pairs in
   the encoder-state and sync interfaces removes parameter noise and the repeated
   `static_cast<uint32_t>(size())` at call sites, with zero runtime cost.

---

## 9. Suggested execution order

1. Section 0 correctness fixes (all small; 0.1 and 0.2 first).
2. 1.1 config caching, 1.6 `<bit>` helpers, 1.7 `getStages`, 1.4 queue changes, 1.3 shared
   listener, 1.2 resource list. Measure with the `draw` benchmark scenario.
3. 2.1, 2.2, 2.3, 2.4: pipeline creation and cache load. Measure with the `pipelines`,
   `cache` and `launch` scenarios.
4. 3.1, 3.2, 3.3: memory per command pool and per resource.
5. Section 4 `MVKSmallVector` iterator and trivial-copy fast paths (compile-time-visible change,
   run the full CTS).
6. Sections 5 and 6 as opportunistic cleanups in the files already being touched.
7. Section 8 items as separate design discussions.
