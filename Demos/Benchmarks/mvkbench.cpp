/*
 * mvkbench.cpp
 *
 * Copyright (c) 2015-2026 The Brenwill Workshop Ltd. (http://www.brenwill.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Headless CPU-side benchmarks for MoltenVK.
 *
 * The Vulkan implementation is loaded at runtime from a library path given on the command line,
 * so a single build of this tool can compare several MoltenVK builds (or any Vulkan driver).
 *
 * Scenarios:
 *   draw       Records one command buffer with many small draws and measures vkQueueSubmit(),
 *              which is where MoltenVK encodes Metal commands by default. Patterns cover
 *              descriptor rebinds, pipeline switches, gl_DrawID, and many short render passes
 *              with render pass objects or dynamic rendering.
 *   pipelines  Creates many graphics pipelines from 1..N threads sharing one VkPipelineCache,
 *              and measures the wall-clock time and scaling.
 *   cache      Measures vkCreatePipelineCache() with a populated cache blob, and the creation
 *              of pipelines from it.
 *
 * See README.md for usage.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "shaders.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std;


#pragma mark - Error handling

[[noreturn]] static void fail(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "mvkbench: error: ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
	exit(1);
}

#define VK_CHECK(expr) do { VkResult r_ = (expr); if (r_ != VK_SUCCESS) { fail("%s failed with VkResult %d (%s:%d)", #expr, r_, __FILE__, __LINE__); } } while (0)


#pragma mark - Vulkan function loading

#define MVKB_GLOBAL_FUNCS(X) \
	X(vkCreateInstance) \
	X(vkEnumerateInstanceExtensionProperties) \
	X(vkEnumerateInstanceLayerProperties)

#define MVKB_INSTANCE_FUNCS(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceFeatures) \
	X(vkGetPhysicalDeviceFeatures2) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkEnumerateDeviceExtensionProperties) \
	X(vkCreateDevice) \
	X(vkGetDeviceProcAddr)

#define MVKB_DEVICE_FUNCS(X) \
	X(vkDestroyDevice) \
	X(vkGetDeviceQueue) \
	X(vkDeviceWaitIdle) \
	X(vkCreateBuffer) \
	X(vkDestroyBuffer) \
	X(vkCreateImage) \
	X(vkDestroyImage) \
	X(vkGetBufferMemoryRequirements) \
	X(vkGetImageMemoryRequirements) \
	X(vkAllocateMemory) \
	X(vkFreeMemory) \
	X(vkBindBufferMemory) \
	X(vkBindImageMemory) \
	X(vkMapMemory) \
	X(vkUnmapMemory) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateSampler) \
	X(vkDestroySampler) \
	X(vkCreateRenderPass) \
	X(vkDestroyRenderPass) \
	X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer) \
	X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) \
	X(vkCreateDescriptorSetLayout) \
	X(vkDestroyDescriptorSetLayout) \
	X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) \
	X(vkCreateDescriptorPool) \
	X(vkDestroyDescriptorPool) \
	X(vkAllocateDescriptorSets) \
	X(vkUpdateDescriptorSets) \
	X(vkCreatePipelineCache) \
	X(vkDestroyPipelineCache) \
	X(vkGetPipelineCacheData) \
	X(vkCreateGraphicsPipelines) \
	X(vkDestroyPipeline) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkCmdBeginRenderPass) \
	X(vkCmdEndRenderPass) \
	X(vkCmdBindPipeline) \
	X(vkCmdBindDescriptorSets) \
	X(vkCmdPushConstants) \
	X(vkCmdSetViewport) \
	X(vkCmdSetScissor) \
	X(vkCmdDraw) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdClearColorImage) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkQueueSubmit) \
	X(vkQueueWaitIdle)

#define MVKB_DECLARE_FUNC(f) static PFN_##f f = nullptr;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
MVKB_GLOBAL_FUNCS(MVKB_DECLARE_FUNC)
MVKB_INSTANCE_FUNCS(MVKB_DECLARE_FUNC)
MVKB_DEVICE_FUNCS(MVKB_DECLARE_FUNC)

// Optional, loaded only when VK_KHR_dynamic_rendering is enabled.
static PFN_vkCmdBeginRenderingKHR vkCmdBeginRenderingKHR = nullptr;
static PFN_vkCmdEndRenderingKHR vkCmdEndRenderingKHR = nullptr;

static void loadLibrary(const string& path) {
	void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if ( !lib ) { fail("cannot load Vulkan library '%s': %s", path.c_str(), dlerror()); }
	vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	if ( !vkGetInstanceProcAddr ) { fail("'%s' does not export vkGetInstanceProcAddr", path.c_str()); }
#define MVKB_LOAD_GLOBAL(f) f = (PFN_##f)vkGetInstanceProcAddr(VK_NULL_HANDLE, #f); if ( !f ) { fail("cannot load %s", #f); }
	MVKB_GLOBAL_FUNCS(MVKB_LOAD_GLOBAL)
}

static void loadInstanceFunctions(VkInstance instance) {
#define MVKB_LOAD_INSTANCE(f) f = (PFN_##f)vkGetInstanceProcAddr(instance, #f); if ( !f ) { fail("cannot load %s", #f); }
	MVKB_INSTANCE_FUNCS(MVKB_LOAD_INSTANCE)
}

static void loadDeviceFunctions(VkDevice device) {
#define MVKB_LOAD_DEVICE(f) f = (PFN_##f)vkGetDeviceProcAddr(device, #f); if ( !f ) { fail("cannot load %s", #f); }
	MVKB_DEVICE_FUNCS(MVKB_LOAD_DEVICE)
}


#pragma mark - Timing and statistics

using Clock = chrono::steady_clock;

static double msSince(Clock::time_point start) {
	return chrono::duration<double, milli>(Clock::now() - start).count();
}

static double median(vector<double> values) {
	if (values.empty()) { return 0.0; }
	sort(values.begin(), values.end());
	size_t mid = values.size() / 2;
	return (values.size() % 2) ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}

static double minimum(const vector<double>& values) {
	return values.empty() ? 0.0 : *min_element(values.begin(), values.end());
}

// Prints a machine-readable result line, collected by run_benchmarks.sh.
static void result(const char* scenario, const string& variant, const char* metric, double value) {
	printf("RESULT %s %s %s %.4f\n", scenario, variant.c_str(), metric, value);
}


#pragma mark - Options

struct Options {
	string libraryPath;
	string scenario = "all";
	bool validation = false;
	uint32_t draws = 20000;
	uint32_t iterations = 20;
	uint32_t drawsPerPass = 8;
	uint32_t pipelines = 64;
	vector<uint32_t> threads = {1, 2, 4, 8};
	string mode = "unique";		// unique | spec | same
	bool useCache = true;
};

static void usage() {
	printf("Usage: mvkbench [options] [draw|pipelines|cache|all]\n"
		   "\n"
		   "Options:\n"
		   "  --lib PATH          Vulkan library to load (default: libMoltenVK.dylib on macOS, libvulkan.so.1 elsewhere)\n"
		   "  --validation        Enable VK_LAYER_KHRONOS_validation (requires a loader, not a bare ICD)\n"
		   "  --draws N           Draws per command buffer in the draw scenario (default 20000)\n"
		   "  --iterations N      Measured submits in the draw scenario (default 20)\n"
		   "  --draws-per-pass N  Draws per render pass in the passes and dynpasses draw patterns (default 8)\n"
		   "  --pipelines N       Pipelines per measurement in the pipelines and cache scenarios (default 64)\n"
		   "  --threads LIST      Comma-separated thread counts for the pipelines scenario (default 1,2,4,8)\n"
		   "  --mode MODE         Pipeline variety: unique (distinct shaders), spec (one shader, distinct\n"
		   "                      specialization constants), same (identical pipelines). Default unique.\n"
		   "  --no-cache          Create pipelines without a VkPipelineCache\n");
}

static vector<uint32_t> parseList(const char* s) {
	vector<uint32_t> list;
	for (const char* p = s; *p; ) {
		char* end = nullptr;
		unsigned long v = strtoul(p, &end, 10);
		if (end == p || v == 0) { fail("invalid list '%s'", s); }
		list.push_back((uint32_t)v);
		p = (*end == ',') ? end + 1 : end;
	}
	return list;
}

static Options parseOptions(int argc, char** argv) {
	Options opts;
#ifdef __APPLE__
	opts.libraryPath = "libMoltenVK.dylib";
#else
	opts.libraryPath = "libvulkan.so.1";
#endif
	for (int i = 1; i < argc; i++) {
		string arg = argv[i];
		auto next = [&]() -> const char* { if (i + 1 >= argc) { fail("%s requires a value", arg.c_str()); } return argv[++i]; };
		if (arg == "--lib") { opts.libraryPath = next(); }
		else if (arg == "--validation") { opts.validation = true; }
		else if (arg == "--draws") { opts.draws = parseList(next())[0]; }
		else if (arg == "--iterations") { opts.iterations = parseList(next())[0]; }
		else if (arg == "--draws-per-pass") { opts.drawsPerPass = parseList(next())[0]; }
		else if (arg == "--pipelines") { opts.pipelines = parseList(next())[0]; }
		else if (arg == "--threads") { opts.threads = parseList(next()); }
		else if (arg == "--mode") { opts.mode = next(); }
		else if (arg == "--no-cache") { opts.useCache = false; }
		else if (arg == "-h" || arg == "--help") { usage(); exit(0); }
		else if (arg[0] != '-') { opts.scenario = arg; }
		else { usage(); fail("unknown option '%s'", arg.c_str()); }
	}
	if (opts.mode != "unique" && opts.mode != "spec" && opts.mode != "same") { fail("unknown mode '%s'", opts.mode.c_str()); }
	if (opts.scenario != "all" && opts.scenario != "draw" && opts.scenario != "pipelines" && opts.scenario != "cache") {
		fail("unknown scenario '%s'", opts.scenario.c_str());
	}
	return opts;
}


#pragma mark - Context

static const VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
static const uint32_t kRenderSize = 64;

struct Context {
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties properties = {};
	VkPhysicalDeviceMemoryProperties memoryProperties = {};
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	bool shaderDrawParameters = false;	// gl_DrawID is available
	bool dynamicRendering = false;		// VK_KHR_dynamic_rendering is enabled
};

static bool hasExtension(const vector<VkExtensionProperties>& exts, const char* name) {
	for (auto& e : exts) { if (strcmp(e.extensionName, name) == 0) { return true; } }
	return false;
}

static Context createContext(const Options& opts) {
	Context ctx;

	uint32_t count = 0;
	VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
	vector<VkExtensionProperties> instExts(count);
	VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, instExts.data()));

	vector<const char*> extensions;
	VkInstanceCreateFlags instFlags = 0;
	if (hasExtension(instExts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		instFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}

	vector<const char*> layers;
	if (opts.validation) {
		VK_CHECK(vkEnumerateInstanceLayerProperties(&count, nullptr));
		vector<VkLayerProperties> layerProps(count);
		VK_CHECK(vkEnumerateInstanceLayerProperties(&count, layerProps.data()));
		bool found = false;
		for (auto& l : layerProps) { if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) { found = true; } }
		if ( !found ) { fail("VK_LAYER_KHRONOS_validation is not available"); }
		layers.push_back("VK_LAYER_KHRONOS_validation");
	}

	VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	appInfo.pApplicationName = "mvkbench";
	appInfo.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo instInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instInfo.flags = instFlags;
	instInfo.pApplicationInfo = &appInfo;
	instInfo.enabledExtensionCount = (uint32_t)extensions.size();
	instInfo.ppEnabledExtensionNames = extensions.data();
	instInfo.enabledLayerCount = (uint32_t)layers.size();
	instInfo.ppEnabledLayerNames = layers.data();
	VK_CHECK(vkCreateInstance(&instInfo, nullptr, &ctx.instance));
	loadInstanceFunctions(ctx.instance);

	VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &count, nullptr));
	if (count == 0) { fail("no Vulkan physical devices"); }
	vector<VkPhysicalDevice> gpus(count);
	VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &count, gpus.data()));
	ctx.physicalDevice = gpus[0];
	vkGetPhysicalDeviceProperties(ctx.physicalDevice, &ctx.properties);
	vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &ctx.memoryProperties);

	vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &count, nullptr);
	vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &count, families.data());
	bool foundQueue = false;
	for (uint32_t i = 0; i < count; i++) {
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { ctx.queueFamily = i; foundQueue = true; break; }
	}
	if ( !foundQueue ) { fail("no graphics queue family"); }

	VK_CHECK(vkEnumerateDeviceExtensionProperties(ctx.physicalDevice, nullptr, &count, nullptr));
	vector<VkExtensionProperties> devExts(count);
	VK_CHECK(vkEnumerateDeviceExtensionProperties(ctx.physicalDevice, nullptr, &count, devExts.data()));
	vector<const char*> deviceExtensions;
	// A portability-subset implementation requires the app to enable this extension.
	if (hasExtension(devExts, "VK_KHR_portability_subset")) { deviceExtensions.push_back("VK_KHR_portability_subset"); }

	// Optional features for some draw patterns. The instance stays at Vulkan 1.1, so dynamic
	// rendering is enabled through the extension and its dependencies.
	bool hasDynamicRenderingExts = (hasExtension(devExts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) &&
									hasExtension(devExts, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) &&
									hasExtension(devExts, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME));
	VkPhysicalDeviceDynamicRenderingFeaturesKHR dynRendFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
	VkPhysicalDeviceShaderDrawParametersFeatures drawParamFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES };
	drawParamFeatures.pNext = hasDynamicRenderingExts ? &dynRendFeatures : nullptr;
	VkPhysicalDeviceFeatures2 supported2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	supported2.pNext = &drawParamFeatures;
	vkGetPhysicalDeviceFeatures2(ctx.physicalDevice, &supported2);
	ctx.shaderDrawParameters = drawParamFeatures.shaderDrawParameters;
	ctx.dynamicRendering = hasDynamicRenderingExts && dynRendFeatures.dynamicRendering;
	if (ctx.dynamicRendering) {
		deviceExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
		deviceExtensions.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
		deviceExtensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
	}

	// The draw shader indexes descriptor arrays with a loop counter.
	VkPhysicalDeviceFeatures supported = {};
	vkGetPhysicalDeviceFeatures(ctx.physicalDevice, &supported);
	if ( !supported.shaderSampledImageArrayDynamicIndexing || !supported.shaderUniformBufferArrayDynamicIndexing ) {
		fail("the device does not support dynamic indexing of sampled image and uniform buffer arrays");
	}
	VkPhysicalDeviceDynamicRenderingFeaturesKHR enabledDynRend = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
	enabledDynRend.dynamicRendering = VK_TRUE;
	VkPhysicalDeviceShaderDrawParametersFeatures enabledDrawParams = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES };
	enabledDrawParams.shaderDrawParameters = ctx.shaderDrawParameters;
	enabledDrawParams.pNext = ctx.dynamicRendering ? &enabledDynRend : nullptr;
	VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	features2.pNext = &enabledDrawParams;
	VkPhysicalDeviceFeatures& features = features2.features;
	features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
	features.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;

	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = ctx.queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;

	VkDeviceCreateInfo devInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	devInfo.queueCreateInfoCount = 1;
	devInfo.pQueueCreateInfos = &queueInfo;
	devInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
	devInfo.ppEnabledExtensionNames = deviceExtensions.data();
	devInfo.pNext = &features2;
	VK_CHECK(vkCreateDevice(ctx.physicalDevice, &devInfo, nullptr, &ctx.device));
	loadDeviceFunctions(ctx.device);
	if (ctx.dynamicRendering) {
		vkCmdBeginRenderingKHR = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(ctx.device, "vkCmdBeginRenderingKHR");
		vkCmdEndRenderingKHR = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(ctx.device, "vkCmdEndRenderingKHR");
		if ( !vkCmdBeginRenderingKHR || !vkCmdEndRenderingKHR ) { fail("cannot load the VK_KHR_dynamic_rendering commands"); }
	}
	vkGetDeviceQueue(ctx.device, ctx.queueFamily, 0, &ctx.queue);

	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = ctx.queueFamily;
	VK_CHECK(vkCreateCommandPool(ctx.device, &poolInfo, nullptr, &ctx.commandPool));

	printf("Device: %s (Vulkan %u.%u.%u), library: %s\n", ctx.properties.deviceName,
		   VK_API_VERSION_MAJOR(ctx.properties.apiVersion), VK_API_VERSION_MINOR(ctx.properties.apiVersion),
		   VK_API_VERSION_PATCH(ctx.properties.apiVersion), opts.libraryPath.c_str());
	return ctx;
}

static void destroyContext(Context& ctx) {
	vkDestroyCommandPool(ctx.device, ctx.commandPool, nullptr);
	vkDestroyDevice(ctx.device, nullptr);
	vkDestroyInstance(ctx.instance, nullptr);
}

static uint32_t findMemoryType(const Context& ctx, uint32_t typeBits, VkMemoryPropertyFlags required) {
	for (uint32_t i = 0; i < ctx.memoryProperties.memoryTypeCount; i++) {
		if ((typeBits & (1u << i)) && (ctx.memoryProperties.memoryTypes[i].propertyFlags & required) == required) { return i; }
	}
	fail("no suitable memory type");
}

static VkDeviceMemory allocateMemory(const Context& ctx, VkMemoryRequirements reqs, VkMemoryPropertyFlags props) {
	VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	info.allocationSize = reqs.size;
	info.memoryTypeIndex = findMemoryType(ctx, reqs.memoryTypeBits, props);
	VkDeviceMemory mem;
	VK_CHECK(vkAllocateMemory(ctx.device, &info, nullptr, &mem));
	return mem;
}

struct Image {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
};

static Image createImage(const Context& ctx, uint32_t size, VkImageUsageFlags usage) {
	Image img;
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = kColorFormat;
	info.extent = { size, size, 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK(vkCreateImage(ctx.device, &info, nullptr, &img.image));
	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(ctx.device, img.image, &reqs);
	img.memory = allocateMemory(ctx, reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkBindImageMemory(ctx.device, img.image, img.memory, 0));

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = img.image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = kColorFormat;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &img.view));
	return img;
}

static void destroyImage(const Context& ctx, Image& img) {
	vkDestroyImageView(ctx.device, img.view, nullptr);
	vkDestroyImage(ctx.device, img.image, nullptr);
	vkFreeMemory(ctx.device, img.memory, nullptr);
}

static VkShaderModule createShaderModule(const Context& ctx, const uint32_t* code, size_t sizeBytes) {
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = sizeBytes;
	info.pCode = code;
	VkShaderModule module;
	VK_CHECK(vkCreateShaderModule(ctx.device, &info, nullptr, &module));
	return module;
}

static VkRenderPass createRenderPass(const Context& ctx) {
	VkAttachmentDescription att = {};
	att.format = kColorFormat;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &ref;
	VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	info.attachmentCount = 1;
	info.pAttachments = &att;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	VkRenderPass rp;
	VK_CHECK(vkCreateRenderPass(ctx.device, &info, nullptr, &rp));
	return rp;
}

static VkCommandBuffer allocateCommandBuffer(const Context& ctx) {
	VkCommandBufferAllocateInfo info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	info.commandPool = ctx.commandPool;
	info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	info.commandBufferCount = 1;
	VkCommandBuffer cb;
	VK_CHECK(vkAllocateCommandBuffers(ctx.device, &info, &cb));
	return cb;
}

static VkFence createFence(const Context& ctx) {
	VkFenceCreateInfo info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	VK_CHECK(vkCreateFence(ctx.device, &info, nullptr, &fence));
	return fence;
}

static void submitAndWait(const Context& ctx, VkCommandBuffer cb) {
	VkFence fence = createFence(ctx);
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cb;
	VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submit, fence));
	VK_CHECK(vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX));
	vkDestroyFence(ctx.device, fence, nullptr);
}

// The standard fixed-function state shared by all pipelines in this benchmark.
struct PipelineState {
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState blendAttachment = {};
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };

	PipelineState(bool blendEnable) {
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		viewport.viewportCount = 1;
		viewport.scissorCount = 1;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		blendAttachment.blendEnable = blendEnable ? VK_TRUE : VK_FALSE;
		blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
		blend.attachmentCount = 1;
		blend.pAttachments = &blendAttachment;
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates = dynamicStates;
	}
	PipelineState(const PipelineState&) = delete;

	void apply(VkGraphicsPipelineCreateInfo& info) const {
		info.pInputAssemblyState = &inputAssembly;
		info.pViewportState = &viewport;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &multisample;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
	}
};


#pragma mark - Draw scenario

static const uint32_t kDrawTextureCount = 16;	// Must match shaders/draw.frag
static const uint32_t kDrawMaterialCount = 8;	// Must match shaders/draw.frag

struct DrawPushConstants {
	float color[4];
	float offset[2];
	uint32_t drawIndex;
};

static void runDrawScenario(const Context& ctx, const Options& opts) {
	printf("\n== draw: %u draws per command buffer, %u measured submits, %u draws per pass in the passes patterns ==\n",
		   opts.draws, opts.iterations, opts.drawsPerPass);

	VkDevice dev = ctx.device;

	// Render target and framebuffer.
	VkRenderPass renderPass = createRenderPass(ctx);
	Image target = createImage(ctx, kRenderSize, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	VkFramebufferCreateInfo fbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	fbInfo.renderPass = renderPass;
	fbInfo.attachmentCount = 1;
	fbInfo.pAttachments = &target.view;
	fbInfo.width = kRenderSize;
	fbInfo.height = kRenderSize;
	fbInfo.layers = 1;
	VkFramebuffer framebuffer;
	VK_CHECK(vkCreateFramebuffer(dev, &fbInfo, nullptr, &framebuffer));

	// Textures, cleared once and transitioned to shader-read layout.
	vector<Image> textures;
	for (uint32_t i = 0; i < kDrawTextureCount; i++) {
		textures.push_back(createImage(ctx, 4, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
	}
	{
		VkCommandBuffer cb = allocateCommandBuffer(ctx);
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(cb, &begin));
		for (uint32_t i = 0; i < kDrawTextureCount; i++) {
			VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = textures[i].image;
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			VkClearColorValue clear = {{ i / float(kDrawTextureCount), 0.5f, 0.25f, 1.0f }};
			vkCmdClearColorImage(cb, textures[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &barrier.subresourceRange);
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		VK_CHECK(vkEndCommandBuffer(cb));
		submitAndWait(ctx, cb);
	}

	VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.maxLod = 1.0f;
	VkSampler sampler;
	VK_CHECK(vkCreateSampler(dev, &samplerInfo, nullptr, &sampler));

	// Uniform buffers: one transform, followed by the materials, each at an aligned offset.
	VkDeviceSize uboStride = max<VkDeviceSize>(256, ctx.properties.limits.minUniformBufferOffsetAlignment);
	VkBufferCreateInfo bufInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufInfo.size = uboStride * (1 + kDrawMaterialCount);
	bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	VkBuffer ubo;
	VK_CHECK(vkCreateBuffer(dev, &bufInfo, nullptr, &ubo));
	VkMemoryRequirements uboReqs;
	vkGetBufferMemoryRequirements(dev, ubo, &uboReqs);
	VkDeviceMemory uboMem = allocateMemory(ctx, uboReqs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK_CHECK(vkBindBufferMemory(dev, ubo, uboMem, 0));
	void* mapped = nullptr;
	VK_CHECK(vkMapMemory(dev, uboMem, 0, VK_WHOLE_SIZE, 0, &mapped));
	for (uint32_t i = 0; i < 1 + kDrawMaterialCount; i++) {
		float* v = (float*)((char*)mapped + i * uboStride);
		v[0] = 1.0f; v[1] = 1.0f; v[2] = 0.5f; v[3] = 1.0f;
	}
	vkUnmapMemory(dev, uboMem);

	// Descriptor set layouts: set 0 for the vertex stage, set 1 with many descriptors for the fragment stage.
	VkDescriptorSetLayoutBinding set0Binding = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
	VkDescriptorSetLayoutBinding set1Bindings[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDrawTextureCount, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDrawMaterialCount, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
	};
	VkDescriptorSetLayout setLayouts[2];
	VkDescriptorSetLayoutCreateInfo dslInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dslInfo.bindingCount = 1;
	dslInfo.pBindings = &set0Binding;
	VK_CHECK(vkCreateDescriptorSetLayout(dev, &dslInfo, nullptr, &setLayouts[0]));
	dslInfo.bindingCount = 2;
	dslInfo.pBindings = set1Bindings;
	VK_CHECK(vkCreateDescriptorSetLayout(dev, &dslInfo, nullptr, &setLayouts[1]));

	VkPushConstantRange pcRange = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawPushConstants) };
	VkPipelineLayoutCreateInfo plInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plInfo.setLayoutCount = 2;
	plInfo.pSetLayouts = setLayouts;
	plInfo.pushConstantRangeCount = 1;
	plInfo.pPushConstantRanges = &pcRange;
	VkPipelineLayout pipelineLayout;
	VK_CHECK(vkCreatePipelineLayout(dev, &plInfo, nullptr, &pipelineLayout));

	VkDescriptorPoolSize poolSizes[2] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 + kDrawMaterialCount },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDrawTextureCount },
	};
	VkDescriptorPoolCreateInfo dpInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpInfo.maxSets = 2;
	dpInfo.poolSizeCount = 2;
	dpInfo.pPoolSizes = poolSizes;
	VkDescriptorPool descPool;
	VK_CHECK(vkCreateDescriptorPool(dev, &dpInfo, nullptr, &descPool));
	VkDescriptorSetAllocateInfo dsAlloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsAlloc.descriptorPool = descPool;
	dsAlloc.descriptorSetCount = 2;
	dsAlloc.pSetLayouts = setLayouts;
	VkDescriptorSet sets[2];
	VK_CHECK(vkAllocateDescriptorSets(dev, &dsAlloc, sets));

	VkDescriptorBufferInfo transformInfo = { ubo, 0, 16 };
	VkDescriptorImageInfo imageInfos[kDrawTextureCount];
	for (uint32_t i = 0; i < kDrawTextureCount; i++) { imageInfos[i] = { sampler, textures[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }; }
	VkDescriptorBufferInfo materialInfos[kDrawMaterialCount];
	for (uint32_t i = 0; i < kDrawMaterialCount; i++) { materialInfos[i] = { ubo, uboStride * (1 + i), 16 }; }
	VkWriteDescriptorSet writes[3] = {};
	for (auto& w : writes) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; }
	writes[0].dstSet = sets[0];
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &transformInfo;
	writes[1].dstSet = sets[1];
	writes[1].dstBinding = 0;
	writes[1].descriptorCount = kDrawTextureCount;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = imageInfos;
	writes[2].dstSet = sets[1];
	writes[2].dstBinding = 1;
	writes[2].descriptorCount = kDrawMaterialCount;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[2].pBufferInfo = materialInfos;
	vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);

	// Pipelines, all with the same layout and fragment shader:
	//   0: the default pipeline
	//   1: differs from 0 only in blending, used by the switch pattern
	//   2: vertex shader reads gl_DrawID, used by the drawid pattern
	//   3: pipeline 0 for dynamic rendering, used by the dynpasses pattern
	enum { kPipelineDefault, kPipelineBlend, kPipelineDrawID, kPipelineDynamic, kPipelineCount };
	VkShaderModule vs = createShaderModule(ctx, kSPIRV_draw_vert, sizeof(kSPIRV_draw_vert));
	VkShaderModule vsDrawID = ctx.shaderDrawParameters ? createShaderModule(ctx, kSPIRV_draw_drawid_vert, sizeof(kSPIRV_draw_drawid_vert)) : VK_NULL_HANDLE;
	VkShaderModule fs = createShaderModule(ctx, kSPIRV_draw_frag, sizeof(kSPIRV_draw_frag));
	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkFormat colorFormat = kColorFormat;
	VkPipelineRenderingCreateInfoKHR renderingInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachmentFormats = &colorFormat;
	VkPipeline pipelines[kPipelineCount] = {};
	for (uint32_t p = 0; p < kPipelineCount; p++) {
		if (p == kPipelineDrawID && !ctx.shaderDrawParameters) { continue; }
		if (p == kPipelineDynamic && !ctx.dynamicRendering) { continue; }
		VkPipelineShaderStageCreateInfo stages[2] = {};
		stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
					  (p == kPipelineDrawID) ? vsDrawID : vs, "main", nullptr };
		stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr };
		PipelineState state(p == kPipelineBlend);
		VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertexInput;
		state.apply(info);
		info.layout = pipelineLayout;
		if (p == kPipelineDynamic) {
			info.pNext = &renderingInfo;
		} else {
			info.renderPass = renderPass;
		}
		VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipelines[p]));
	}

	VkCommandBuffer cb = allocateCommandBuffer(ctx);
	VkFence fence = createFence(ctx);

	struct Pattern { const char* name; const char* description; };
	const Pattern patterns[] = {
		{ "static",    "bind pipeline and sets once, then push constants + draw" },
		{ "rebind",    "bind descriptor sets before every draw" },
		{ "switch",    "alternate between two pipelines on every draw" },
		{ "drawid",    "as static, with a vertex shader that reads gl_DrawID" },
		{ "passes",    "as static, in many short render passes (VkRenderPass)" },
		{ "dynpasses", "as static, in many short render passes (dynamic rendering)" },
	};

	printf("%-9s %12s %12s %14s %12s   %s\n", "pattern", "record ms", "submit ms", "submit us/draw", "gpu wait ms", "description");
	for (const Pattern& pattern : patterns) {
		string name = pattern.name;
		bool isDynamic = (name == "dynpasses");
		uint32_t drawsPerPass = (name == "passes" || isDynamic) ? opts.drawsPerPass : opts.draws;
		uint32_t firstPipeline = kPipelineDefault;
		if (name == "drawid") { firstPipeline = kPipelineDrawID; }
		if (isDynamic) { firstPipeline = kPipelineDynamic; }
		if ( !pipelines[firstPipeline] ) {
			printf("%-9s skipped: the device does not support %s\n", pattern.name, isDynamic ? "VK_KHR_dynamic_rendering" : "shaderDrawParameters");
			continue;
		}

		VkClearValue clear = {};
		VkRenderPassBeginInfo rpBegin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		rpBegin.renderPass = renderPass;
		rpBegin.framebuffer = framebuffer;
		rpBegin.renderArea = { { 0, 0 }, { kRenderSize, kRenderSize } };
		rpBegin.clearValueCount = 1;
		rpBegin.pClearValues = &clear;
		VkRenderingAttachmentInfoKHR colorAtt = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
		colorAtt.imageView = target.view;
		colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAtt.clearValue = clear;
		VkRenderingInfoKHR renderingBegin = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
		renderingBegin.renderArea = rpBegin.renderArea;
		renderingBegin.layerCount = 1;
		renderingBegin.colorAttachmentCount = 1;
		renderingBegin.pColorAttachments = &colorAtt;
		auto beginPass = [&]() {
			if (isDynamic) {
				vkCmdBeginRenderingKHR(cb, &renderingBegin);
			} else {
				vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
			}
		};
		auto endPass = [&]() {
			if (isDynamic) {
				vkCmdEndRenderingKHR(cb);
			} else {
				vkCmdEndRenderPass(cb);
			}
		};

		auto recordStart = Clock::now();
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VK_CHECK(vkBeginCommandBuffer(cb, &begin));
		if (isDynamic) {
			// Dynamic rendering has no initial layout transition, so make one here.
			VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = target.image;
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		beginPass();
		VkViewport viewport = { 0, 0, float(kRenderSize), float(kRenderSize), 0, 1 };
		VkRect2D scissor = { { 0, 0 }, { kRenderSize, kRenderSize } };
		vkCmdSetViewport(cb, 0, 1, &viewport);
		vkCmdSetScissor(cb, 0, 1, &scissor);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[firstPipeline]);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 0, nullptr);
		for (uint32_t d = 0; d < opts.draws; d++) {
			if (d > 0 && d % drawsPerPass == 0) {
				endPass();
				beginPass();
			}
			if (name == "rebind") {
				vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 0, nullptr);
			} else if (name == "switch") {
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[(d & 1) ? kPipelineBlend : kPipelineDefault]);
			}
			DrawPushConstants pc = {};
			pc.color[0] = (d % 7) / 7.0f; pc.color[1] = (d % 11) / 11.0f; pc.color[2] = 0.5f; pc.color[3] = 1.0f;
			pc.offset[0] = ((d % 97) / 48.5f) - 1.0f;
			pc.offset[1] = (((d / 97) % 97) / 48.5f) - 1.0f;
			pc.drawIndex = d;
			vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
			vkCmdDraw(cb, 3, 1, 0, 0);
		}
		endPass();
		VK_CHECK(vkEndCommandBuffer(cb));
		double recordMs = msSince(recordStart);

		// Submit the same command buffer repeatedly. The first submit is a warm-up.
		vector<double> submitMs, waitMs;
		for (uint32_t it = 0; it <= opts.iterations; it++) {
			VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &cb;
			auto t0 = Clock::now();
			VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submit, fence));
			double s = msSince(t0);
			auto t1 = Clock::now();
			VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
			double w = msSince(t1);
			VK_CHECK(vkResetFences(dev, 1, &fence));
			if (it > 0) { submitMs.push_back(s); waitMs.push_back(w); }
		}
		double med = median(submitMs);
		printf("%-9s %12.3f %12.3f %14.3f %12.3f   %s\n", pattern.name, recordMs, med, med * 1000.0 / opts.draws, median(waitMs), pattern.description);
		result("draw", name, "record_ms", recordMs);
		result("draw", name, "submit_ms_median", med);
		result("draw", name, "submit_ms_min", minimum(submitMs));
		result("draw", name, "submit_us_per_draw", med * 1000.0 / opts.draws);
		result("draw", name, "gpu_wait_ms_median", median(waitMs));
	}

	VK_CHECK(vkDeviceWaitIdle(dev));
	vkDestroyFence(dev, fence, nullptr);
	for (auto p : pipelines) { if (p) { vkDestroyPipeline(dev, p, nullptr); } }
	vkDestroyShaderModule(dev, vs, nullptr);
	if (vsDrawID) { vkDestroyShaderModule(dev, vsDrawID, nullptr); }
	vkDestroyShaderModule(dev, fs, nullptr);
	vkDestroyDescriptorPool(dev, descPool, nullptr);
	vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
	for (auto l : setLayouts) { vkDestroyDescriptorSetLayout(dev, l, nullptr); }
	vkDestroyBuffer(dev, ubo, nullptr);
	vkFreeMemory(dev, uboMem, nullptr);
	vkDestroySampler(dev, sampler, nullptr);
	for (auto& t : textures) { destroyImage(ctx, t); }
	vkDestroyFramebuffer(dev, framebuffer, nullptr);
	destroyImage(ctx, target);
	vkDestroyRenderPass(dev, renderPass, nullptr);
}


#pragma mark - Pipeline creation scenarios

// Values patched into the shaders and used as specialization constants. They start at a random
// point on every run, so the generated MSL differs between runs and Metal's on-disk shader cache
// cannot serve compiled libraries from an earlier run. All values stay exactly representable
// as floats, which are exact for integers below 2^24.
static uint32_t nextUniqueValue() {
	static uint32_t next = [] {
		random_device rd;
		return 1 + (uint32_t)(rd() % 8000000u);
	}();
	return next++;
}

static float asFloat(uint32_t v) { return (float)v; }

static uint32_t floatBits(float f) {
	uint32_t bits;
	memcpy(&bits, &f, sizeof(bits));
	return bits;
}

// Returns a copy of the SPIR-V with the one word holding `from` replaced by `to`.
static vector<uint32_t> patchSPIRV(const uint32_t* code, size_t wordCount, float from, float to) {
	vector<uint32_t> words(code, code + wordCount);
	uint32_t fromBits = floatBits(from);
	size_t found = 0;
	for (auto& w : words) { if (w == fromBits) { w = floatBits(to); found++; } }
	if (found != 1) { fail("expected exactly one patchable constant in the SPIR-V, found %zu", found); }
	return words;
}

static const float kVertexPatchValue = 1234.5678f;		// Must match shaders/pipeline.vert
static const float kFragmentPatchValue = 8765.4321f;	// Must match shaders/pipeline.frag

struct PipelineResources {
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
};

static PipelineResources createPipelineResources(const Context& ctx) {
	PipelineResources res;
	res.renderPass = createRenderPass(ctx);
	VkDescriptorSetLayoutBinding bindings[3] = {
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
		{ 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
	};
	VkDescriptorSetLayoutCreateInfo dslInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dslInfo.bindingCount = 3;
	dslInfo.pBindings = bindings;
	VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &dslInfo, nullptr, &res.setLayout));
	VkPipelineLayoutCreateInfo plInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = &res.setLayout;
	VK_CHECK(vkCreatePipelineLayout(ctx.device, &plInfo, nullptr, &res.layout));
	return res;
}

static void destroyPipelineResources(const Context& ctx, PipelineResources& res) {
	vkDestroyPipelineLayout(ctx.device, res.layout, nullptr);
	vkDestroyDescriptorSetLayout(ctx.device, res.setLayout, nullptr);
	vkDestroyRenderPass(ctx.device, res.renderPass, nullptr);
}

// The shader modules and specialization values for a batch of pipelines.
struct PipelineBatch {
	vector<VkShaderModule> vertexModules;		// One per pipeline, or a single shared module
	vector<VkShaderModule> fragmentModules;
	vector<float> specValues;					// One per pipeline

	VkShaderModule vertexModule(size_t i) const { return vertexModules[min(i, vertexModules.size() - 1)]; }
	VkShaderModule fragmentModule(size_t i) const { return fragmentModules[min(i, fragmentModules.size() - 1)]; }
};

static PipelineBatch createPipelineBatch(const Context& ctx, const string& mode, uint32_t count) {
	PipelineBatch batch;
	size_t vsWords = sizeof(kSPIRV_pipeline_vert) / 4, fsWords = sizeof(kSPIRV_pipeline_frag) / 4;
	uint32_t moduleCount = (mode == "unique") ? count : 1;
	for (uint32_t i = 0; i < moduleCount; i++) {
		auto vs = patchSPIRV(kSPIRV_pipeline_vert, vsWords, kVertexPatchValue, asFloat(nextUniqueValue()));
		auto fs = patchSPIRV(kSPIRV_pipeline_frag, fsWords, kFragmentPatchValue, asFloat(nextUniqueValue()));
		batch.vertexModules.push_back(createShaderModule(ctx, vs.data(), vs.size() * 4));
		batch.fragmentModules.push_back(createShaderModule(ctx, fs.data(), fs.size() * 4));
	}
	float sharedSpec = asFloat(nextUniqueValue());
	for (uint32_t i = 0; i < count; i++) {
		batch.specValues.push_back(mode == "spec" ? asFloat(nextUniqueValue()) : sharedSpec);
	}
	return batch;
}

static void destroyPipelineBatch(const Context& ctx, PipelineBatch& batch) {
	for (auto m : batch.vertexModules) { vkDestroyShaderModule(ctx.device, m, nullptr); }
	for (auto m : batch.fragmentModules) { vkDestroyShaderModule(ctx.device, m, nullptr); }
	batch = {};
}

static VkPipeline createBatchPipeline(const Context& ctx, const PipelineResources& res, const PipelineBatch& batch,
									  size_t index, VkPipelineCache cache) {
	VkSpecializationMapEntry specEntry = { 0, 0, sizeof(float) };
	float specValue = batch.specValues[index];
	VkSpecializationInfo specInfo = { 1, &specEntry, sizeof(float), &specValue };

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, batch.vertexModule(index), "main", nullptr };
	stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, batch.fragmentModule(index), "main", &specInfo };

	VkVertexInputBindingDescription vtxBinding = { 0, 20, VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription vtxAttrs[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 },
	};
	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &vtxBinding;
	vertexInput.vertexAttributeDescriptionCount = 2;
	vertexInput.pVertexAttributeDescriptions = vtxAttrs;

	PipelineState state(false);
	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	state.apply(info);
	info.layout = res.layout;
	info.renderPass = res.renderPass;
	VkPipeline pipeline;
	VK_CHECK(vkCreateGraphicsPipelines(ctx.device, cache, 1, &info, nullptr, &pipeline));
	return pipeline;
}

static VkPipelineCache createPipelineCache(const Context& ctx, const vector<uint8_t>* initialData = nullptr) {
	VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	if (initialData) {
		info.initialDataSize = initialData->size();
		info.pInitialData = initialData->data();
	}
	VkPipelineCache cache;
	VK_CHECK(vkCreatePipelineCache(ctx.device, &info, nullptr, &cache));
	return cache;
}

// Creates all pipelines of the batch from `threadCount` threads, and returns the wall-clock time.
static double createPipelinesInParallel(const Context& ctx, const PipelineResources& res, const PipelineBatch& batch,
										uint32_t count, uint32_t threadCount, VkPipelineCache cache, vector<VkPipeline>& pipelines) {
	pipelines.assign(count, VK_NULL_HANDLE);
	mutex startLock;
	condition_variable startSignal;
	bool started = false;
	vector<thread> threads;
	for (uint32_t t = 0; t < threadCount; t++) {
		threads.emplace_back([&, t] {
			{
				unique_lock<mutex> lock(startLock);
				startSignal.wait(lock, [&] { return started; });
			}
			for (uint32_t i = t; i < count; i += threadCount) {
				pipelines[i] = createBatchPipeline(ctx, res, batch, i, cache);
			}
		});
	}
	auto start = Clock::now();
	{
		lock_guard<mutex> lock(startLock);
		started = true;
	}
	startSignal.notify_all();
	for (auto& th : threads) { th.join(); }
	return msSince(start);
}

static void runPipelinesScenario(const Context& ctx, const Options& opts) {
	printf("\n== pipelines: %u pipelines, mode %s, %s ==\n", opts.pipelines, opts.mode.c_str(),
		   opts.useCache ? "shared VkPipelineCache" : "no VkPipelineCache");

	PipelineResources res = createPipelineResources(ctx);

	// Warm up the driver and Metal compiler services with one throwaway pipeline.
	{
		PipelineBatch warmup = createPipelineBatch(ctx, "unique", 1);
		vector<VkPipeline> p;
		createPipelinesInParallel(ctx, res, warmup, 1, 1, VK_NULL_HANDLE, p);
		vkDestroyPipeline(ctx.device, p[0], nullptr);
		destroyPipelineBatch(ctx, warmup);
	}

	printf("%-8s %12s %16s %10s\n", "threads", "wall ms", "pipelines/sec", "speedup");
	double firstMs = 0.0;		// Wall time for the first thread count in the list, the speedup baseline
	for (uint32_t threadCount : opts.threads) {
		// Fresh shaders and a fresh cache for every measurement, so nothing is reused between them.
		PipelineBatch batch = createPipelineBatch(ctx, opts.mode, opts.pipelines);
		VkPipelineCache cache = opts.useCache ? createPipelineCache(ctx) : VK_NULL_HANDLE;
		vector<VkPipeline> pipelines;
		double ms = createPipelinesInParallel(ctx, res, batch, opts.pipelines, threadCount, cache, pipelines);
		if (firstMs == 0.0) { firstMs = ms; }
		double speedup = firstMs / ms;
		string variant = opts.mode + (opts.useCache ? "_cache" : "_nocache") + "_t" + to_string(threadCount);
		printf("%-8u %12.1f %16.1f %9.2fx\n", threadCount, ms, opts.pipelines * 1000.0 / ms, speedup);
		result("pipelines", variant, "wall_ms", ms);
		result("pipelines", variant, "pipelines_per_sec", opts.pipelines * 1000.0 / ms);
		result("pipelines", variant, "speedup", speedup);
		for (auto p : pipelines) { vkDestroyPipeline(ctx.device, p, nullptr); }
		if (cache) { vkDestroyPipelineCache(ctx.device, cache, nullptr); }
		destroyPipelineBatch(ctx, batch);
	}

	destroyPipelineResources(ctx, res);
}

static void runCacheScenario(const Context& ctx, const Options& opts) {
	printf("\n== cache: restore a VkPipelineCache holding %u pipelines ==\n", opts.pipelines);

	PipelineResources res = createPipelineResources(ctx);
	PipelineBatch batch = createPipelineBatch(ctx, "unique", opts.pipelines);

	// Populate a cache and serialize it.
	vector<uint8_t> blob;
	{
		VkPipelineCache cache = createPipelineCache(ctx);
		vector<VkPipeline> pipelines;
		createPipelinesInParallel(ctx, res, batch, opts.pipelines, 1, cache, pipelines);
		size_t size = 0;
		VK_CHECK(vkGetPipelineCacheData(ctx.device, cache, &size, nullptr));
		blob.resize(size);
		VK_CHECK(vkGetPipelineCacheData(ctx.device, cache, &size, blob.data()));
		blob.resize(size);
		for (auto p : pipelines) { vkDestroyPipeline(ctx.device, p, nullptr); }
		vkDestroyPipelineCache(ctx.device, cache, nullptr);
	}

	// Restore it, and create the same pipelines from it. The shader modules are kept, so they match the cache entries.
	auto t0 = Clock::now();
	VkPipelineCache cache = createPipelineCache(ctx, &blob);
	double createCacheMs = msSince(t0);

	vector<VkPipeline> pipelines(opts.pipelines, VK_NULL_HANDLE);
	auto t1 = Clock::now();
	pipelines[0] = createBatchPipeline(ctx, res, batch, 0, cache);
	double firstMs = msSince(t1);
	auto t2 = Clock::now();
	for (uint32_t i = 1; i < opts.pipelines; i++) { pipelines[i] = createBatchPipeline(ctx, res, batch, i, cache); }
	double restMs = msSince(t2);

	printf("%-28s %10.1f KB\n", "cache data size", blob.size() / 1024.0);
	printf("%-28s %10.2f ms\n", "vkCreatePipelineCache", createCacheMs);
	printf("%-28s %10.2f ms\n", "first pipeline from cache", firstMs);
	printf("%-28s %10.2f ms\n", "remaining pipelines", restMs);
	printf("%-28s %10.2f ms\n", "total", createCacheMs + firstMs + restMs);
	result("cache", "restore", "blob_kb", blob.size() / 1024.0);
	result("cache", "restore", "create_cache_ms", createCacheMs);
	result("cache", "restore", "first_pipeline_ms", firstMs);
	result("cache", "restore", "remaining_pipelines_ms", restMs);
	result("cache", "restore", "total_ms", createCacheMs + firstMs + restMs);

	for (auto p : pipelines) { vkDestroyPipeline(ctx.device, p, nullptr); }
	vkDestroyPipelineCache(ctx.device, cache, nullptr);
	destroyPipelineBatch(ctx, batch);
	destroyPipelineResources(ctx, res);
}


#pragma mark - Main

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IOLBF, 0);
	Options opts = parseOptions(argc, argv);
	loadLibrary(opts.libraryPath);
	Context ctx = createContext(opts);

	if (opts.scenario == "all" || opts.scenario == "draw")      { runDrawScenario(ctx, opts); }
	if (opts.scenario == "all" || opts.scenario == "pipelines") { runPipelinesScenario(ctx, opts); }
	if (opts.scenario == "all" || opts.scenario == "cache")     { runCacheScenario(ctx, opts); }

	destroyContext(ctx);
	return 0;
}
