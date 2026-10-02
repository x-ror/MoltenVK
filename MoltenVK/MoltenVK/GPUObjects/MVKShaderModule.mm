/*
 * MVKShaderModule.mm
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

#include "MVKShaderModule.h"
#include "MVKPipeline.h"
#include "MVKFoundation.h"
#include <algorithm>
#include <optional>
#include <sys/stat.h>

using namespace std;
using namespace mvk;

MVKMTLFunction::MVKMTLFunction(id<MTLFunction> mtlFunc, const SPIRVToMSLConversionResultInfo& scRslts, MTLSize tgSize) {
	_mtlFunction = [mtlFunc retain];		// retained
	shaderConversionResults = scRslts;
	threadGroupSize = tgSize;
}

MVKMTLFunction::MVKMTLFunction(const MVKMTLFunction& other) {
	_mtlFunction = [other._mtlFunction retain];		// retained
	shaderConversionResults = other.shaderConversionResults;
	threadGroupSize = other.threadGroupSize;
}

// Moving takes over the retained reference to the MTLFunction.
MVKMTLFunction::MVKMTLFunction(MVKMTLFunction&& other) noexcept :
	shaderConversionResults(std::move(other.shaderConversionResults)),
	threadGroupSize(other.threadGroupSize),
	_mtlFunction(other._mtlFunction) {
	other._mtlFunction = nil;
}

MVKMTLFunction& MVKMTLFunction::operator=(MVKMTLFunction&& other) noexcept {
	if (this != &other) {
		[_mtlFunction release];
		_mtlFunction = other._mtlFunction;
		other._mtlFunction = nil;
		shaderConversionResults = std::move(other.shaderConversionResults);
		threadGroupSize = other.threadGroupSize;
	}
	return *this;
}

MVKMTLFunction& MVKMTLFunction::operator=(const MVKMTLFunction& other) {
	// Retain new object first in case it's the same object
	[other._mtlFunction retain];
	[_mtlFunction release];
	_mtlFunction = other._mtlFunction;

	shaderConversionResults = other.shaderConversionResults;
	threadGroupSize = other.threadGroupSize;
	return *this;
}

MVKMTLFunction::~MVKMTLFunction() {
	[_mtlFunction release];
}


#pragma mark -
#pragma mark MVKShaderLibrary

// If the size of the workgroup dimension is specialized, extract it from the
// specialization info, otherwise use the value specified in the SPIR-V shader code.
static uint32_t getWorkgroupDimensionSize(const SPIRVWorkgroupSizeDimension& wgDim, const VkSpecializationInfo* pSpecInfo) {
	if (wgDim.isSpecialized && pSpecInfo) {
		for (uint32_t specIdx = 0; specIdx < pSpecInfo->mapEntryCount; specIdx++) {
			const VkSpecializationMapEntry* pMapEntry = &pSpecInfo->pMapEntries[specIdx];
			if (pMapEntry->constantID == wgDim.specializationID) {
				return *reinterpret_cast<uint32_t*>((uintptr_t)pSpecInfo->pData + pMapEntry->offset) ;
			}
		}
	}
	return wgDim.size;
}

MVKMTLFunction MVKShaderLibrary::getMTLFunction(const VkSpecializationInfo* pSpecializationInfo,
												VkPipelineCreationFeedback* pShaderFeedback,
												MVKShaderModule* shaderModule) {

	ensureCompiled();
	if ( !_mtlLibrary ) { return MVKMTLFunctionNull; }

	// If specialization happens on constants mapped to macros, the function comes from a library
	// variant compiled with the matching macro definitions, which then caches its own functions.
	if (pSpecializationInfo && _maySpecializeWithMacro) {
		MVKShaderLibrary* variant = getMacroSpecializedVariant(pSpecializationInfo);
		if (variant) { return variant->getMTLFunction(pSpecializationInfo, pShaderFeedback, shaderModule); }
	}

	@autoreleasepool {
		id<MTLFunction> mtlFunc = getSpecializedMTLFunction(pSpecializationInfo, pShaderFeedback, shaderModule);
		if (mtlFunc && pShaderFeedback) { mvkEnableFlags(pShaderFeedback->flags, VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT); }

		auto& wgSize = _shaderConversionResultInfo.entryPoint.workgroupSize;
		return MVKMTLFunction(mtlFunc, _shaderConversionResultInfo, MTLSizeMake(getWorkgroupDimensionSize(wgSize.width, pSpecializationInfo),
																				getWorkgroupDimensionSize(wgSize.height, pSpecializationInfo),
																				getWorkgroupDimensionSize(wgSize.depth, pSpecializationInfo)));
	}
}

// Returns the library variant compiled with the macro definitions matching the specialization info,
// creating it if needed, or nullptr if the specialization info specializes no macro constants.
MVKShaderLibrary* MVKShaderLibrary::getMacroSpecializedVariant(const VkSpecializationInfo* pSpecializationInfo) {
	// Most shaders map no specialization constants to macros, so avoid building the list for them.
	if (_shaderConversionResultInfo.specializationMacros.empty()) { return nullptr; }

	vector<pair<uint32_t, MVKShaderMacroValue>> spec_list;
	for (uint32_t specIdx = 0; specIdx < pSpecializationInfo->mapEntryCount; specIdx++) {
		const VkSpecializationMapEntry* pMapEntry = &pSpecializationInfo->pMapEntries[specIdx];
		uint32_t const_id = pMapEntry->constantID;
		MVKShaderMacroValue macro_value = {};
		size_t size = min(pMapEntry->size, sizeof(macro_value.value));

		memcpy(&macro_value.value, (char *)pSpecializationInfo->pData + pMapEntry->offset, size);
		macro_value.size = size;
		if (_shaderConversionResultInfo.specializationMacros.find(const_id) != _shaderConversionResultInfo.specializationMacros.end()) {
			spec_list.push_back(make_pair(const_id, macro_value));
		}
	}
	if (spec_list.empty()) { return nullptr; }

	// Sort the specialization list before it is used as a key to index the variants.
	// Pipelines sharing this library may be created concurrently, so guard the variant map.
	std::sort(spec_list.begin(), spec_list.end());
	lock_guard<mutex> lock(_variantsLock);
	auto entry = _specializationVariants.find(spec_list);
	if (entry != _specializationVariants.end()) { return entry->second; }

	MVKShaderLibrary* new_mvklib = new MVKShaderLibrary(_owner, _shaderConversionResultInfo, _compressedMSL, &spec_list);
	_specializationVariants[spec_list] = new_mvklib;
	return new_mvklib;
}

// Retrieves the unspecialized entry point function and its function constants once.
// Returns whether the function exists. Must be called with _functionsLock held.
bool MVKShaderLibrary::ensureBaseMTLFunction(VkPipelineCreationFeedback* pShaderFeedback, MVKShaderModule* shaderModule) {
	if (_isBaseMTLFunctionRetrieved) { return _baseMTLFunction != nil; }
	_isBaseMTLFunctionRetrieved = true;

	@autoreleasepool {
		NSString* mtlFuncName = @(_shaderConversionResultInfo.entryPoint.mtlFunctionName.c_str());
		uint64_t startTime = pShaderFeedback ? mvkGetTimestamp() : getPerformanceTimestamp();
		@synchronized (getMTLDevice()) {
			_baseMTLFunction = [_mtlLibrary newFunctionWithName: mtlFuncName];							// retained
			_mtlFunctionConstants = [_baseMTLFunction.functionConstantsDictionary.allValues retain];	// retained
		}

		// Index the function constants by constant index once, so specialization lookups
		// do not message each Metal constant object for every specialization entry.
		_mtlFunctionConstantTypes.clear();
		_mtlFunctionConstantTypes.reserve(_mtlFunctionConstants.count);
		for (MTLFunctionConstant* mfc in _mtlFunctionConstants) {
			_mtlFunctionConstantTypes.emplace_back((uint32_t)mfc.index, mfc.type);
		}
		std::sort(_mtlFunctionConstantTypes.begin(), _mtlFunctionConstantTypes.end(),
				  [](const auto& a, const auto& b) { return a.first < b.first; });
		addPerformanceInterval(getPerformanceStats().shaderCompilation.functionRetrieval, startTime);
		if (pShaderFeedback) {
			pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime);
		}

		// Set the debug name. First try name of shader module, otherwise try name of owner.
		NSString* dbName = shaderModule->getDebugName();
		if ( !dbName ) { dbName = _owner->getDebugName(); }
		_owner->setMetalObjectLabel(_baseMTLFunction, dbName);
	}
	return _baseMTLFunction != nil;
}

// Returns the entry point function, specialized with the function constant values from the
// specialization info if the shader has function constants. Functions are cached by the
// constant values, so pipelines with equal specialization share one compiled function.
// The returned function is owned by this library.
id<MTLFunction> MVKShaderLibrary::getSpecializedMTLFunction(const VkSpecializationInfo* pSpecializationInfo,
															VkPipelineCreationFeedback* pShaderFeedback,
															MVKShaderModule* shaderModule) {
	// Collect the specialization entries that correspond to function constants of this shader,
	// and build the cache key from their IDs and values, in constant ID order.
	struct SpecEntry { uint32_t constantID; const char* pData; uint32_t size; MTLDataType type; };
	vector<SpecEntry> specEntries;
	vector<uint8_t> key;

	// Functions are returned retained and autoreleased while the lock is held, so that clearing
	// the function cache on another thread cannot release a function before the caller retains it.
	unique_lock<mutex> lock(_functionsLock);
	if ( !ensureBaseMTLFunction(pShaderFeedback, shaderModule) ) { return nil; }
	if (_mtlFunctionConstants.count == 0) { return [[_baseMTLFunction retain] autorelease]; }
	string funcName = _shaderConversionResultInfo.entryPoint.mtlFunctionName;

	if (pSpecializationInfo) {
		specEntries.reserve(pSpecializationInfo->mapEntryCount);
		size_t keySize = 0;
		for (uint32_t specIdx = 0; specIdx < pSpecializationInfo->mapEntryCount; specIdx++) {
			const VkSpecializationMapEntry* pMapEntry = &pSpecializationInfo->pMapEntries[specIdx];
			auto fcIter = std::lower_bound(_mtlFunctionConstantTypes.begin(), _mtlFunctionConstantTypes.end(), pMapEntry->constantID,
										   [](const std::pair<uint32_t, MTLDataType>& fc, uint32_t constantID) { return fc.first < constantID; });
			if (fcIter != _mtlFunctionConstantTypes.end() && fcIter->first == pMapEntry->constantID) {
				specEntries.push_back({ pMapEntry->constantID,
										(const char*)pSpecializationInfo->pData + pMapEntry->offset,
										(uint32_t)pMapEntry->size,
										fcIter->second });
				keySize += sizeof(pMapEntry->constantID) + sizeof(uint32_t) + pMapEntry->size;
			}
		}
		std::sort(specEntries.begin(), specEntries.end(), [](const SpecEntry& a, const SpecEntry& b) { return a.constantID < b.constantID; });
		key.reserve(keySize);
		for (auto& se : specEntries) {
			const uint8_t* pID = (const uint8_t*)&se.constantID;
			const uint8_t* pSize = (const uint8_t*)&se.size;
			key.insert(key.end(), pID, pID + sizeof(se.constantID));
			key.insert(key.end(), pSize, pSize + sizeof(se.size));
			key.insert(key.end(), (const uint8_t*)se.pData, (const uint8_t*)se.pData + se.size);
		}
	}

	auto cached = _specializedMTLFunctions.find(key);
	if (cached != _specializedMTLFunctions.end()) { return [[cached->second retain] autorelease]; }

	// Compile the specialized function with the lock released, so that other pipelines
	// can retrieve functions from this library meanwhile.
	lock.unlock();
	id<MTLFunction> mtlFunc = nil;
	@autoreleasepool {
		MTLFunctionConstantValues* mtlFCVals = [[MTLFunctionConstantValues new] autorelease];
		for (auto& se : specEntries) {
			[mtlFCVals setConstantValue: se.pData type: se.type atIndex: se.constantID];
		}

		NSString* mtlFuncName = @(funcName.c_str());
		uint64_t startTime = pShaderFeedback ? mvkGetTimestamp() : 0;
		// Heap-allocated, because the completion handler may run after a timeout.
		MVKFunctionSpecializer* fs = new MVKFunctionSpecializer(_owner);
		mtlFunc = fs->newMTLFunction(_mtlLibrary, mtlFuncName, mtlFCVals);		// retained
		fs->destroy();
		if (pShaderFeedback) {
			pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime);
		}

		NSString* dbName = shaderModule->getDebugName();
		if ( !dbName ) { dbName = _owner->getDebugName(); }
		_owner->setMetalObjectLabel(mtlFunc, dbName);
	}
	lock.lock();

	// Don't cache a failed specialization, which may have been a transient compiler timeout.
	if ( !mtlFunc ) { return nil; }

	// If the entry point changed while the lock was released, the function is still correct for
	// this request, but belongs to a stale cache generation, so return it without caching it.
	if (funcName != _shaderConversionResultInfo.entryPoint.mtlFunctionName) { return [mtlFunc autorelease]; }

	// If another thread compiled the same specialization meanwhile, use its function.
	cached = _specializedMTLFunctions.find(key);
	if (cached != _specializedMTLFunctions.end()) {
		[mtlFunc release];
		return [[cached->second retain] autorelease];
	}
	_specializedMTLFunctions[std::move(key)] = mtlFunc;
	return [[mtlFunc retain] autorelease];
}

// Compiles the MTLLibrary if compilation was deferred when this library was restored from pipeline cache data.
// The deferred flag is cleared only after _mtlLibrary is set, so a reader that sees the flag
// clear (isSerializable(), or the unlocked check below) also sees the compiled library.
void MVKShaderLibrary::ensureCompiled() {
	if ( !_isCompileDeferred ) { return; }
	lock_guard<mutex> lock(_functionsLock);
	if ( !_isCompileDeferred ) { return; }
	string msl;
	decompressMSL(msl);
	compileLibrary(msl);
	_isCompileDeferred = false;
}

// Releases the cached functions. Called when the entry point changes, and on destruction.
void MVKShaderLibrary::clearFunctionCache() {
	lock_guard<mutex> lock(_functionsLock);
	for (auto& pair : _specializedMTLFunctions) { [pair.second release]; }
	_specializedMTLFunctions.clear();
	[_mtlFunctionConstants release];
	_mtlFunctionConstants = nil;
	_mtlFunctionConstantTypes.clear();
	[_baseMTLFunction release];
	_baseMTLFunction = nil;
	_isBaseMTLFunctionRetrieved = false;
}

void MVKShaderLibrary::setEntryPointName(string& funcName) {
	if (_shaderConversionResultInfo.entryPoint.mtlFunctionName == funcName) { return; }
	_shaderConversionResultInfo.entryPoint.mtlFunctionName = funcName;
	clearFunctionCache();
}

void MVKShaderLibrary::setWorkgroupSize(uint32_t x, uint32_t y, uint32_t z) {
	auto& wgSize = _shaderConversionResultInfo.entryPoint.workgroupSize;
	wgSize.width.size = x;
	wgSize.height.size = y;
	wgSize.depth.size = z;
}

// Sets the cached MSL source code, after first compressing it.
void MVKShaderLibrary::compressMSL(const string& msl) {
	uint64_t startTime = getPerformanceTimestamp();
	_compressedMSL.compress(msl, getMVKConfig().shaderSourceCompressionAlgorithm);
	addPerformanceInterval(getPerformanceStats().shaderCompilation.mslCompress, startTime);
}

// Decompresses the cached MSL into the string.
void MVKShaderLibrary::decompressMSL(string& msl) {
	uint64_t startTime = getPerformanceTimestamp();
	_compressedMSL.decompress(msl);
	addPerformanceInterval(getPerformanceStats().shaderCompilation.mslDecompress, startTime);
}

MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,
								   const SPIRVToMSLConversionResult& conversionResult) :
	MVKBaseDeviceObject(owner->getDevice()),
	_owner(owner),
	_maySpecializeWithMacro(true) {

	_shaderConversionResultInfo = conversionResult.resultInfo;
	compressMSL(conversionResult.msl);
	compileLibrary(conversionResult.msl);
}

// The result info and compressed MSL are taken by value, so a caller that no longer needs
// them (such as reading a pipeline cache) can move them in, while other callers copy.
MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,
								   SPIRVToMSLConversionResultInfo resultInfo,
								   MVKCompressor<std::string> compressedMSL,
								   const vector<pair<uint32_t, MVKShaderMacroValue> >* specializationMacroDef,
								   bool deferCompile) :
	MVKBaseDeviceObject(owner->getDevice()),
	_owner(owner),
	_compressedMSL(std::move(compressedMSL)),
	_shaderConversionResultInfo(std::move(resultInfo)),
	_maySpecializeWithMacro(specializationMacroDef == nullptr),
	_isCompileDeferred(deferCompile && !specializationMacroDef) {

	if (_isCompileDeferred) { return; }
	string msl;
	decompressMSL(msl);
	compileLibrary(msl, specializationMacroDef);
}

void MVKShaderLibrary::compileLibrary(const string& msl,
									  const vector<pair<uint32_t, MVKShaderMacroValue> >* specializationMacroDef) {
	MVKShaderLibraryCompiler* slc = new MVKShaderLibraryCompiler(_owner);
	NSString* nsSrc = [[NSString alloc] initWithUTF8String: msl.c_str()];	// temp retained

	// If specialization macro is used, translate the id to macro information and pass it to compiler
	vector<pair<MSLSpecializationMacroInfo, MVKShaderMacroValue>> macro_def;
	if (specializationMacroDef) {
		for (auto& def: *specializationMacroDef) {
			const auto& macro_name_iter = _shaderConversionResultInfo.specializationMacros.find(def.first);
			if (macro_name_iter != _shaderConversionResultInfo.specializationMacros.end()) {
				macro_def.push_back(make_pair(macro_name_iter->second, def.second));
			}
		}
	}

	_mtlLibrary = slc->newMTLLibrary(nsSrc, _shaderConversionResultInfo, macro_def);	// retained
	[nsSrc release];														// release temp string
	slc->destroy();
}

MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,
                                   const void* mslCompiledCodeData,
                                   size_t mslCompiledCodeLength) :
	MVKBaseDeviceObject(owner->getDevice()),
	_owner(owner),
	_maySpecializeWithMacro(false) {

    uint64_t startTime = getPerformanceTimestamp();
    @autoreleasepool {
        dispatch_data_t shdrData = dispatch_data_create(mslCompiledCodeData,
                                                        mslCompiledCodeLength,
                                                        NULL,
                                                        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError* err = nil;
        _mtlLibrary = [getMTLDevice() newLibraryWithData: shdrData error: &err];    // retained
        handleCompilationError(err, "Compiled shader module creation");
        [shdrData release];
    }
	addPerformanceInterval(getPerformanceStats().shaderCompilation.mslLoad, startTime);
}

// Macro-specialized variants are owned by the library that created them, so they are not
// shared with the copy. The copy re-creates any variant it needs on first use.
// The other library may be compiling on first use on another thread. Its deferred flag is
// cleared only after its MTLLibrary is set, so read the flag first, and read the MTLLibrary
// only if the flag is clear. Otherwise the copy stays deferred and compiles its own library.
MVKShaderLibrary::MVKShaderLibrary(const MVKShaderLibrary& other) :
	MVKBaseDeviceObject(other._device),
	_owner(other._owner),
	_maySpecializeWithMacro(other._maySpecializeWithMacro),
	_isCompileDeferred(other._isCompileDeferred.load()) {

	if ( !_isCompileDeferred ) { _mtlLibrary = [other._mtlLibrary retain]; }
	_shaderConversionResultInfo = other._shaderConversionResultInfo;
	_compressedMSL = other._compressedMSL;
}

MVKShaderLibrary& MVKShaderLibrary::operator=(const MVKShaderLibrary& other) {
	if (this == &other) { return *this; }
	clearFunctionCache();
	_isCompileDeferred = other._isCompileDeferred.load();
	id<MTLLibrary> otherLib = _isCompileDeferred ? nil : other._mtlLibrary;
	if (_mtlLibrary != otherLib) {
		[_mtlLibrary release];
		_mtlLibrary = [otherLib retain];
	}
	_owner = other._owner;
	_shaderConversionResultInfo = other._shaderConversionResultInfo;
	_compressedMSL = other._compressedMSL;
	return *this;
}

// If err object is nil, the compilation succeeded without any warnings.
// If err object exists, and the MTLLibrary was created, the compilation succeeded, but with warnings.
// If err object exists, and the MTLLibrary was not created, the compilation failed.
void MVKShaderLibrary::handleCompilationError(NSError* err, const char* opDesc) {
    if ( !err ) return;

    if (_mtlLibrary) {
        MVKLogInfo("%s succeeded with warnings (Error code %li):\n%s", opDesc, (long)err.code, err.localizedDescription.UTF8String);
    } else {
		_owner->setConfigurationResult(reportError(VK_ERROR_INITIALIZATION_FAILED,
												   "%s failed (Error code %li):\n%s",
												   opDesc, (long)err.code,
												   err.localizedDescription.UTF8String));
    }
}

MVKShaderLibrary::~MVKShaderLibrary() {
	clearFunctionCache();
	[_mtlLibrary release];

	for (auto& item: _specializationVariants) {
		delete item.second;
	}
}


#pragma mark -
#pragma mark MVKShaderLibraryCache

MVKShaderLibrary* MVKShaderLibraryCache::getShaderLibrary(SPIRVToMSLConversionConfiguration* pShaderConfig,
														  MVKShaderModule* shaderModule, MVKPipeline* pipeline,
														  bool* pWasAdded, VkPipelineCreationFeedback* pShaderFeedback,
														  uint64_t startTime,
														  unique_lock<mutex>* pCacheLock) {
	if (pWasAdded) { *pWasAdded = false; }

	// Exclusive access: look up, and convert and compile inline if needed.
	if ( !pCacheLock ) {
		MVKShaderLibrary* shLib = findShaderLibrary(pShaderConfig, pShaderFeedback, startTime);
		if ( !shLib && !pipeline->shouldFailOnPipelineCompileRequired() ) {
			SPIRVToMSLConversionResult conversionResult;
			if (shaderModule->convert(pShaderConfig, conversionResult) && !conversionResult.msl.empty()) {
				shLib = addShaderLibrary(pShaderConfig, conversionResult);
				if (pShaderFeedback) {
					pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime);
				}
				if (pWasAdded) { *pWasAdded = true; }
			}
		}
		return shLib;
	}

	// Shared access: the caller holds *pCacheLock. Look up under the lock. On a miss, if another
	// thread is already converting an equivalent request, wait for it and look up again. Otherwise
	// register this request as in flight and release the lock while converting and compiling.
	// The marked copy of the config is only needed on a miss, so the common cache hit does not pay
	// for copying it. The config is unchanged by a miss, so the copy stays valid across retries.
	std::optional<SPIRVToMSLConversionConfiguration> markedConfig;
	std::shared_ptr<InFlightConversion> inFlight;
	while (true) {
		MVKShaderLibrary* shLib = findShaderLibrary(pShaderConfig, pShaderFeedback, startTime);
		if (shLib) { return shLib; }
		if (pipeline->shouldFailOnPipelineCompileRequired()) { return nullptr; }

		if ( !markedConfig ) {
			markedConfig.emplace(*pShaderConfig);
			markedConfig->markAllInterfaceVarsAndResourcesUsed();
		}
		std::shared_ptr<InFlightConversion> other = findInFlight(*markedConfig);
		if ( !other ) { break; }
		other->done.wait(*pCacheLock, [&other]{ return other->isDone; });
	}

	inFlight = std::make_shared<InFlightConversion>();
	inFlight->config = std::move(*markedConfig);
	_inFlight.push_back(inFlight);

	// Retires the in-flight entry and wakes any waiters, whether or not the conversion succeeded.
	auto retireInFlight = [&]() {
		for (auto iter = _inFlight.begin(); iter != _inFlight.end(); iter++) {
			if (*iter == inFlight) { _inFlight.erase(iter); break; }
		}
		inFlight->isDone = true;
		inFlight->done.notify_all();
	};

	pCacheLock->unlock();
	MVKShaderLibrary* shLib = nullptr;
	try {
		SPIRVToMSLConversionResult conversionResult;
		if (shaderModule->convert(pShaderConfig, conversionResult) && !conversionResult.msl.empty()) {
			shLib = new MVKShaderLibrary(_owner, conversionResult);
		}
	} catch (...) {
		pCacheLock->lock();
		retireInFlight();
		throw;
	}
	pCacheLock->lock();
	retireInFlight();

	if ( !shLib ) { return nullptr; }
	return addOrReuseShaderLibrary(pShaderConfig, shLib, pWasAdded, pShaderFeedback, startTime);
}

// Returns an in-flight conversion whose request is equivalent to the marked config, or null if there is none.
// Both configs have all elements marked as used, so matching in both directions compares every element.
std::shared_ptr<MVKShaderLibraryCache::InFlightConversion> MVKShaderLibraryCache::findInFlight(const SPIRVToMSLConversionConfiguration& markedConfig) {
	for (auto& inFlight : _inFlight) {
		if (inFlight->config.matches(markedConfig) && markedConfig.matches(inFlight->config)) { return inFlight; }
	}
	return nullptr;
}

// Adds the newly compiled library to the cache, unless an equivalent library was added by another
// thread meanwhile, in which case the new library is destroyed and the existing one returned.
MVKShaderLibrary* MVKShaderLibraryCache::addOrReuseShaderLibrary(SPIRVToMSLConversionConfiguration* pShaderConfig,
																 MVKShaderLibrary* shLib, bool* pWasAdded,
																 VkPipelineCreationFeedback* pShaderFeedback,
																 uint64_t startTime) {
	MVKShaderLibrary* existing = findShaderLibrary(pShaderConfig, pShaderFeedback, startTime);
	if (existing) {
		shLib->destroy();
		return existing;
	}
	_shaderLibraries.emplace_back(*pShaderConfig, shLib);
	if (pShaderFeedback) {
		pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime);
	}
	if (pWasAdded) { *pWasAdded = true; }
	return shLib;
}

// Finds and returns a shader library matching the shader config, or returns nullptr if it doesn't exist.
// If a match is found, the shader config is aligned with the shader config of the matching library.
MVKShaderLibrary* MVKShaderLibraryCache::findShaderLibrary(SPIRVToMSLConversionConfiguration* pShaderConfig,
														   VkPipelineCreationFeedback* pShaderFeedback,
														   uint64_t startTime) {
	for (auto& slPair : _shaderLibraries) {
		if (slPair.first.matches(*pShaderConfig)) {
			pShaderConfig->alignWith(slPair.first);
			addPerformanceInterval(getPerformanceStats().shaderCompilation.shaderLibraryFromCache, startTime);
			if (pShaderFeedback) {
				pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime);
			}
			return slPair.second;
		}
	}
	return nullptr;
}

// Adds and returns a new shader library configured from the specified conversion configuration.
MVKShaderLibrary* MVKShaderLibraryCache::addShaderLibrary(const SPIRVToMSLConversionConfiguration* pShaderConfig,
														  const SPIRVToMSLConversionResult& conversionResult) {
	MVKShaderLibrary* shLib = new MVKShaderLibrary(_owner, conversionResult);
	_shaderLibraries.emplace_back(*pShaderConfig, shLib);
	return shLib;
}

// Adds and returns a new shader library configured from contents read from a pipeline cache.
// The arguments are taken by value and moved into the cache, so the caller's copies are not duplicated.
MVKShaderLibrary* MVKShaderLibraryCache::addShaderLibrary(SPIRVToMSLConversionConfiguration shaderConfig,
														  SPIRVToMSLConversionResultInfo resultInfo,
														  MVKCompressor<std::string> compressedMSL,
														  bool deferCompile) {
	MVKShaderLibrary* shLib = new MVKShaderLibrary(_owner, std::move(resultInfo), std::move(compressedMSL), nullptr, deferCompile);
	_shaderLibraries.emplace_back(std::move(shaderConfig), shLib);
	return shLib;
}

// Merge another shader library cache with this one. Handle null input.
void MVKShaderLibraryCache::merge(MVKShaderLibraryCache* other) {
	if ( !other ) { return; }
	for (auto& otherPair : other->_shaderLibraries) {
		if ( !findShaderLibrary(&otherPair.first) ) {
			_shaderLibraries.emplace_back(otherPair.first, new MVKShaderLibrary(*otherPair.second));
			_shaderLibraries.back().second->_owner = _owner;
		}
	}
}

MVKShaderLibraryCache::~MVKShaderLibraryCache() {
	for (auto& slPair : _shaderLibraries) { slPair.second->destroy(); }
}


#pragma mark -
#pragma mark MVKShaderModule

MVKMTLFunction MVKShaderModule::getMTLFunction(SPIRVToMSLConversionConfiguration* pShaderConfig,
											   const VkSpecializationInfo* pSpecializationInfo,
											   MVKPipeline* pipeline,
											   VkPipelineCreationFeedback* pShaderFeedback) {
	MVKShaderLibrary* mvkLib = _directMSLLibrary;
	if ( !mvkLib ) {
		uint64_t startTime = pShaderFeedback ? mvkGetTimestamp() : getPerformanceTimestamp();
		MVKPipelineCache* pipelineCache = pipeline->getPipelineCache();
		if (pipelineCache) {
			mvkLib = pipelineCache->getShaderLibrary(pShaderConfig, this, pipeline, pShaderFeedback, startTime);
		} else {
			unique_lock<mutex> lock(_accessLock);
			mvkLib = _shaderLibraryCache.getShaderLibrary(pShaderConfig, this, pipeline, nullptr, pShaderFeedback, startTime, &lock);
		}
	} else {
		mvkLib->setEntryPointName(pShaderConfig->options.entryPointName);
		pShaderConfig->markAllInterfaceVarsAndResourcesUsed();
	}

	return mvkLib ? mvkLib->getMTLFunction(pSpecializationInfo, pShaderFeedback, this) : MVKMTLFunctionNull;
}

bool MVKShaderModule::convert(SPIRVToMSLConversionConfiguration* pShaderConfig,
							  SPIRVToMSLConversionResult& conversionResult) {
	const auto& mvkCfg = getMVKConfig();
	bool shouldLogCode = mvkCfg.debugMode;
	bool shouldLogEstimatedGLSL = shouldLogCode && mvkCfg.shaderLogEstimatedGLSL;

	uint64_t startTime = getPerformanceTimestamp();
	bool wasConverted = _spvConverter.convert(*pShaderConfig, conversionResult, shouldLogCode, shouldLogCode, shouldLogEstimatedGLSL);
	addPerformanceInterval(getPerformanceStats().shaderCompilation.spirvToMSL, startTime);

	const char* dumpDir = getMVKConfig().shaderDumpDir;
	if (dumpDir && *dumpDir) {
		char path[PATH_MAX];
		const char* type;
		switch (pShaderConfig->options.entryPointStage) {
			case spv::ExecutionModelVertex:                 type = "-vs"; break;
			case spv::ExecutionModelTessellationControl:    type = "-tcs"; break;
			case spv::ExecutionModelTessellationEvaluation: type = "-tes"; break;
			case spv::ExecutionModelFragment:               type = "-fs"; break;
			case spv::ExecutionModelGeometry:               type = "-gs"; break;
			case spv::ExecutionModelTaskNV:                 type = "-ts"; break;
			case spv::ExecutionModelMeshNV:                 type = "-ms"; break;
			case spv::ExecutionModelGLCompute:              type = "-cs"; break;
			default:                                        type = "";    break;
		}
		mkdir(dumpDir, 0755);
		snprintf(path, sizeof(path), "%s/shader%s-%016zx.spv", dumpDir, type, _key.codeHash);
		FILE* file = fopen(path, "wb");
		if (file) {
			fwrite(_spvConverter.getSPIRV().data(), sizeof(uint32_t), _spvConverter.getSPIRV().size(), file);
			fclose(file);
		}
		snprintf(path, sizeof(path), "%s/shader%s-%016zx.metal", dumpDir, type, _key.codeHash);
		file = fopen(path, "wb");
		if (file) {
			if (wasConverted) {
				fwrite(conversionResult.msl.data(), 1, conversionResult.msl.size(), file);
				fclose(file);
			} else {
				fputs("Failed to convert:\n", file);
				fwrite(conversionResult.resultLog.data(), 1, conversionResult.resultLog.size(), file);
				fclose(file);
			}
		}
	}

	if (wasConverted) {
		if (shouldLogCode) { MVKLogInfo("%s", conversionResult.resultLog.c_str()); }
	} else {
		reportError(VK_ERROR_INITIALIZATION_FAILED, "Unable to convert SPIR-V to MSL:\n%s", conversionResult.resultLog.c_str());
	}
	return wasConverted;
}

void MVKShaderModule::setWorkgroupSize(uint32_t x, uint32_t y, uint32_t z) {
	if(_directMSLLibrary) { _directMSLLibrary->setWorkgroupSize(x, y, z); }
}


#pragma mark Reflection

// Entries are never removed from the map, and std::map does not move its nodes,
// so the returned reference stays valid after the lock is released.
const MVKShaderModule::InterfaceReflection& MVKShaderModule::getInterfaceReflection(spv::ExecutionModel model,
																					 spv::StorageClass storage,
																					 const char* entryName) {
	InterfaceReflectionKey key { model, storage, entryName ? entryName : "" };
	lock_guard<mutex> lock(_reflectionLock);
	auto iter = _interfaceReflections.find(key);
	if (iter == _interfaceReflections.end()) {
		iter = _interfaceReflections.emplace(key, InterfaceReflection()).first;
		InterfaceReflection& refl = iter->second;
		refl.success = mvk::getShaderInterfaceVariables(getSPIRV(), storage, model, key.entryName, refl.vars, refl.errorLog);
	}
	return iter->second;
}

bool MVKShaderModule::getTessReflectionData(const char* tescEntryName,
											MVKShaderModule* teseModule, const char* teseEntryName,
											SPIRVTessReflectionData& reflectData, string& errorLog) {
	TessReflectionKey key { tescEntryName ? tescEntryName : "", teseModule->getKey(), teseEntryName ? teseEntryName : "" };
	lock_guard<mutex> lock(_reflectionLock);
	auto iter = _tessReflections.find(key);
	if (iter == _tessReflections.end()) {
		iter = _tessReflections.emplace(key, TessReflection()).first;
		TessReflection& refl = iter->second;
		refl.success = mvk::getTessReflectionData(getSPIRV(), key.tescEntryName, teseModule->getSPIRV(), key.teseEntryName, refl.data, refl.errorLog);
	}
	reflectData = iter->second.data;
	errorLog = iter->second.errorLog;
	return iter->second.success;
}


#pragma mark Construction

MVKShaderModule::MVKShaderModule(MVKDevice* device,
								 const VkShaderModuleCreateInfo* pCreateInfo) : MVKVulkanAPIDeviceObject(device), _shaderLibraryCache(this) {

	_directMSLLibrary = nullptr;

	size_t codeSize = pCreateInfo->codeSize;

    // Ensure something is there.
    if ( (pCreateInfo->pCode == VK_NULL_HANDLE) || (codeSize < 4) ) {
		setConfigurationResult(reportError(VK_ERROR_INITIALIZATION_FAILED, "vkCreateShaderModule(): Shader module contains no shader code."));
		return;
	}

	size_t codeHash = 0;

	// Retrieve the magic number to determine what type of shader code has been loaded.
	// NOTE: Shader code should be submitted as SPIR-V. Although some simple direct MSL shaders may work,
	// direct loading of MSL source code or compiled MSL code is not officially supported at this time.
	// Future versions of MoltenVK may support direct MSL submission again.
	uint32_t magicNum = *pCreateInfo->pCode;
	switch (magicNum) {
		case kMVKMagicNumberSPIRVCode: {					// SPIR-V code
			size_t spvCount = (codeSize + 3) >> 2;			// Round up if byte length not exactly on uint32_t boundary

			uint64_t startTime = getPerformanceTimestamp();
			codeHash = mvkHash(pCreateInfo->pCode, spvCount);
			addPerformanceInterval(getPerformanceStats().shaderCompilation.hashShaderCode, startTime);

			_spvConverter.setSPIRV(pCreateInfo->pCode, spvCount);

			break;
		}
		case kMVKMagicNumberMSLSourceCode: {				// MSL source code
			size_t hdrSize = sizeof(MVKMSLSPIRVHeader);
			char* pMSLCode = (char*)(uintptr_t(pCreateInfo->pCode) + hdrSize);
			size_t mslCodeLen = codeSize - hdrSize;

			uint64_t startTime = getPerformanceTimestamp();
			codeHash = mvkHash(&magicNum);
			codeHash = mvkHash(pMSLCode, mslCodeLen, codeHash);
			addPerformanceInterval(getPerformanceStats().shaderCompilation.hashShaderCode, startTime);

			SPIRVToMSLConversionResult conversionResult;
			conversionResult.msl = pMSLCode;
			_directMSLLibrary = new MVKShaderLibrary(this, conversionResult);

			break;
		}
		case kMVKMagicNumberMSLCompiledCode: {				// MSL compiled binary code
			size_t hdrSize = sizeof(MVKMSLSPIRVHeader);
			char* pMSLCode = (char*)(uintptr_t(pCreateInfo->pCode) + hdrSize);
			size_t mslCodeLen = codeSize - hdrSize;

			uint64_t startTime = getPerformanceTimestamp();
			codeHash = mvkHash(&magicNum);
			codeHash = mvkHash(pMSLCode, mslCodeLen, codeHash);
			addPerformanceInterval(getPerformanceStats().shaderCompilation.hashShaderCode, startTime);

			_directMSLLibrary = new MVKShaderLibrary(this, (void*)(pMSLCode), mslCodeLen);

			break;
		}
		default:
			setConfigurationResult(reportError(VK_ERROR_INITIALIZATION_FAILED, "vkCreateShaderModule(): The SPIR-V contains an invalid magic number %x.", magicNum));
			break;
	}

	_key = MVKShaderModuleKey(codeSize, codeHash);
}

MVKShaderModule::~MVKShaderModule() {
	if (_directMSLLibrary) { _directMSLLibrary->destroy(); }
}


#pragma mark -
#pragma mark MVKShaderLibraryCompiler

id<MTLLibrary> MVKShaderLibraryCompiler::newMTLLibrary(NSString* mslSourceCode,
													   const SPIRVToMSLConversionResultInfo& shaderConversionResults,
													   const vector<pair<MSLSpecializationMacroInfo, MVKShaderMacroValue>>& specializationMacroDef) {
	unique_lock<mutex> lock(_completionLock);

	compile(lock, ^{
		auto mtlDev = getMTLDevice();
		@synchronized (mtlDev) {
			@autoreleasepool {
				auto mtlCompileOptions = getDevice()->getMTLCompileOptions(shaderConversionResults.entryPoint.fpFastMathFlags,
																		   shaderConversionResults.isPositionInvariant);
				if (!specializationMacroDef.empty()) {
					size_t macro_count = specializationMacroDef.size();
					NSString *macro_names[macro_count];
					NSNumber *macro_values[macro_count];
					for (uint32_t i = 0; i < specializationMacroDef.size(); i++) {
						macro_names[i] = @(specializationMacroDef[i].first.name.c_str());
						macro_values[i] = getMacroValue(specializationMacroDef[i].first, specializationMacroDef[i].second);
					}
					mtlCompileOptions.preprocessorMacros = [NSDictionary dictionaryWithObjects: macro_values
																					   forKeys: macro_names
																						 count: macro_count];
				}
				logCompilation(mtlCompileOptions);

				[mtlDev newLibraryWithSource: mslSourceCode
									options: mtlCompileOptions
						completionHandler: ^(id<MTLLibrary> mtlLib, NSError* error) {
							bool isLate = compileComplete(mtlLib, error);
							if (isLate) { destroy(); }
						}];
			}
		}
	});

	return [_mtlLibrary retain];
}

NSNumber *MVKShaderLibraryCompiler::getMacroValue(const MSLSpecializationMacroInfo& info,
												  const MVKShaderMacroValue& value) {
	NSNumber *result;

	if (info.isFloat) {
		if (value.size == sizeof(double)) {
			result = [NSNumber numberWithDouble: value.value.f64];
		} else {
			result = [NSNumber numberWithFloat: value.value.f32];
		}
	} else {
		if (info.isSigned) {
			switch (value.size) {
				case 1:
					result = [NSNumber numberWithChar: value.value.si8];
					break;
				case 2:
					result = [NSNumber numberWithShort: value.value.si16];
					break;
				case 4:
					result = [NSNumber numberWithInt: value.value.si32];
					break;
				case 8:
					result = [NSNumber numberWithLongLong: value.value.si64];
					break;
				default:
					result = [NSNumber numberWithInt: value.value.si32];
					break;
			}
		} else {
			switch (value.size) {
				case 1:
					result = [NSNumber numberWithUnsignedChar: value.value.ui8];
					break;
				case 2:
					result = [NSNumber numberWithUnsignedShort: value.value.ui16];
					break;
				case 4:
					result = [NSNumber numberWithUnsignedInt: value.value.ui32];
					break;
				case 8:
					result = [NSNumber numberWithUnsignedLongLong: value.value.ui64];
					break;
				default:
					result = [NSNumber numberWithUnsignedInt: value.value.ui32];
					break;
			}
		}
	}

	return result;
}

void MVKShaderLibraryCompiler::handleError() {
	if (_mtlLibrary) {
		MVKLogInfo("%s compilation succeeded with warnings (Error code %li):\n%s", _compilerType.c_str(),
				   (long)_compileError.code, _compileError.localizedDescription.UTF8String);
	} else {
		MVKMetalCompiler::handleError();
	}
}

bool MVKShaderLibraryCompiler::compileComplete(id<MTLLibrary> mtlLibrary, NSError* compileError) {
	lock_guard<mutex> lock(_completionLock);

	_mtlLibrary = [mtlLibrary retain];		// retained
	return endCompile(compileError);
}

void MVKShaderLibraryCompiler::logCompilation(MTLCompileOptions* mtlCompOpt) {
	if ( !getMVKConfig().debugMode ) { return; }

#if MVK_XCODE_16
	if ([mtlCompOpt respondsToSelector: @selector(mathMode)]) {
		const char* mathModeName = "Unknown";
		switch (mtlCompOpt.mathMode) {
			case MTLMathModeFast:
				mathModeName = "Fast";
				break;
			case MTLMathModeRelaxed:
				mathModeName = "Relaxed";
				break;
			case MTLMathModeSafe:
				mathModeName = "Safe";
				break;
			default:
				break;
		}
		const char* mathFPFName = "Unknown";
		switch (mtlCompOpt.mathFloatingPointFunctions) {
			case MTLMathFloatingPointFunctionsFast:
				mathFPFName = "Fast";
				break;
			case MTLMathFloatingPointFunctionsPrecise:
				mathFPFName = "Precise";
				break;
			default:
				break;
		}
		MVKLogInfo("Compiling Metal shader with MathMode %s, MathFloatingPointFunctions %s, and PreserveInvariance %sabled.",
				   mathModeName, mathFPFName, mtlCompOpt.preserveInvariance ? "en" : "dis");
	} else
#endif
	{
		MVKLogInfo("Compiling Metal shader with FastMath %sabled and PreserveInvariance %sabled.",
				   mtlCompOpt.fastMathEnabled ? "en" : "dis", mtlCompOpt.preserveInvariance ? "en" : "dis");
	}
}


#pragma mark Construction

MVKShaderLibraryCompiler::~MVKShaderLibraryCompiler() {
	[_mtlLibrary release];
}


#pragma mark -
#pragma mark MVKFunctionSpecializer

id<MTLFunction> MVKFunctionSpecializer::newMTLFunction(id<MTLLibrary> mtlLibrary,
													   NSString* funcName,
													   MTLFunctionConstantValues* constantValues) {
	unique_lock<mutex> lock(_completionLock);

	compile(lock, ^{
		@synchronized (getMTLDevice()) {
			[mtlLibrary newFunctionWithName: funcName
							 constantValues: constantValues
						  completionHandler: ^(id<MTLFunction> mtlFunc, NSError* error) {
							  bool isLate = compileComplete(mtlFunc, error);
							  if (isLate) { destroy(); }
						  }];
		}
	});

	return [_mtlFunction retain];
}

bool MVKFunctionSpecializer::compileComplete(id<MTLFunction> mtlFunction, NSError* compileError) {
	lock_guard<mutex> lock(_completionLock);

	_mtlFunction = [mtlFunction retain];		// retained
	return endCompile(compileError);
}

#pragma mark Construction

MVKFunctionSpecializer::~MVKFunctionSpecializer() {
	[_mtlFunction release];
}

