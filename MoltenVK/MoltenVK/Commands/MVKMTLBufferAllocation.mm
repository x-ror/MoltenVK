/*
 * MVKMTLBufferAllocation.mm
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

#include "MVKMTLBufferAllocation.h"
#include <functional>


#pragma mark -
#pragma mark MVKMTLBufferAllocation

MVKVulkanAPIObject* MVKMTLBufferAllocation::getVulkanAPIObject() { return _pool->getVulkanAPIObject(); };

void MVKMTLBufferAllocation::returnToPool() { _pool->returnAllocation(this); }


#pragma mark -
#pragma mark MVKMTLBufferAllocationPool

MVKMTLBufferAllocation* MVKMTLBufferAllocationPool::newObject() {
    // If we're at the end of the current MTLBuffer, add a new one.
    if (_nextOffset >= _mtlBufferLength) { addMTLBuffer(); }

    // Extract and return the next allocation from the current buffer,
    // which is always the last one in the array, and advance the offset
    // of future allocation to beyond this allocation.
    NSUInteger offset = _nextOffset;
    _nextOffset += _allocationLength;
    return new MVKMTLBufferAllocation(this, _mtlBuffers.back().mtlBuffer, offset, _allocationLength, _mtlBuffers.size() - 1);
}

// Adds a new MTLBuffer to the buffer pool and resets the next offset to the start of it
void MVKMTLBufferAllocationPool::addMTLBuffer() {
    MTLResourceOptions mbOpts = (_mtlStorageMode << MTLResourceStorageModeShift) | MTLResourceCPUCacheModeDefaultCache;
    _mtlBuffers.push_back({ [getMTLDevice() newBufferWithLength: _mtlBufferLength options: mbOpts], 0 });
	getDevice()->makeResident(_mtlBuffers.back().mtlBuffer);
    _nextOffset = 0;
}

MVKMTLBufferAllocation* MVKMTLBufferAllocationPool::acquireAllocationUnlocked() {
    MVKMTLBufferAllocation* ba = acquireObject();
    if (!_mtlBuffers[ba->_poolIndex].allocationCount++) {
        [ba->_mtlBuffer setPurgeableState: MTLPurgeableStateNonVolatile];
    }
    return ba;
}

MVKMTLBufferAllocation* MVKMTLBufferAllocationPool::acquireAllocation() {
    if (_isThreadSafe) {
        std::lock_guard<MVKUnfairLock> lock(_lock);
        return acquireAllocationUnlocked();
    } else {
        return acquireAllocationUnlocked();
    }
}

void MVKMTLBufferAllocationPool::returnAllocationUnlocked(MVKMTLBufferAllocation* ba) {
    if (!--_mtlBuffers[ba->_poolIndex].allocationCount) {
        [ba->_mtlBuffer setPurgeableState: MTLPurgeableStateVolatile];
    }
    returnObject(ba);
}

void MVKMTLBufferAllocationPool::returnAllocation(MVKMTLBufferAllocation* ba) {
    if (_isThreadSafe) {
        std::lock_guard<MVKUnfairLock> lock(_lock);
        returnAllocationUnlocked(ba);
    } else {
        returnAllocationUnlocked(ba);
    }
}

void MVKMTLBufferAllocationPool::returnAllocations(MVKArrayRef<MVKMTLBufferAllocation*> allocations) {
	std::sort(allocations.begin(), allocations.end(), [](auto* a, auto* b) { return std::less<>()(a->_pool, b->_pool); });

	size_t count = allocations.size();
	size_t start = 0;
	while (start < count) {
		MVKMTLBufferAllocationPool* pool = allocations[start]->_pool;
		size_t end = start + 1;
		while (end < count && allocations[end]->_pool == pool) { end++; }

		auto returnRange = [&]() { for (size_t i = start; i < end; i++) { pool->returnAllocationUnlocked(allocations[i]); } };
		if (pool->_isThreadSafe) {
			std::lock_guard<MVKUnfairLock> lock(pool->_lock);
			returnRange();
		} else {
			returnRange();
		}
		start = end;
	}
}

MVKMTLBufferAllocationPool::MVKMTLBufferAllocationPool(MVKDevice* device, NSUInteger allocationLength, bool makeThreadSafe,
													   bool isDedicated, MTLStorageMode mtlStorageMode) :
	MVKObjectPool<MVKMTLBufferAllocation>(true),
	MVKDeviceTrackingMixin(device) {

    _allocationLength = allocationLength;
	_isThreadSafe = makeThreadSafe;
    _mtlBufferLength = _allocationLength * (isDedicated ? 1 : calcMTLBufferAllocationCount());
    _mtlStorageMode = mtlStorageMode;
    _nextOffset = _mtlBufferLength;     // Force a MTLBuffer to be added on first access
}

// Returns the number of regions to allocate per MTLBuffer, as determined from the allocation size.
uint32_t MVKMTLBufferAllocationPool::calcMTLBufferAllocationCount() {
    if (_allocationLength <= 256 ) { return 256; }
    if (_allocationLength <= (1 * KIBI) ) { return 128; }
    if (_allocationLength <= (4 * KIBI) ) { return 64; }
    if (_allocationLength <= (256 * KIBI) ) { return (512 * KIBI) / _allocationLength; }

    return 1;
}

MVKMTLBufferAllocationPool::~MVKMTLBufferAllocationPool() {
    for (uint32_t bufferIndex = 0; bufferIndex < _mtlBuffers.size(); ++bufferIndex) {
		getDevice()->removeResidency(_mtlBuffers[bufferIndex].mtlBuffer);
        [_mtlBuffers[bufferIndex].mtlBuffer release];
    }
    _mtlBuffers.clear();
}


#pragma mark -
#pragma mark MVKMTLBufferAllocator

MVKMTLBufferAllocation* MVKMTLBufferAllocator::acquireMTLBufferRegion(NSUInteger length) {
	MVKAssert(length <= _maxAllocationLength, "This MVKMTLBufferAllocator has been configured to dispense MVKMTLBufferRegions no larger than %lu bytes.", (unsigned long)_maxAllocationLength);

	// Can't allocate a segment smaller than the minimum MTLBuffer alignment.
	length = std::max<NSUInteger>(length, getMetalFeatures().mtlBufferAlignment);

    // Convert max length to the next power-of-two exponent to use as a lookup
    NSUInteger p2Exp = mvkPowerOfTwoExponent(length);
    return getRegionPool(p2Exp)->acquireAllocation();
}

// Returns the pool for the power-of-two exponent, creating it on first use.
// The pointer is read without the lock on the fast path, and the lock only
// guards creation, so a pool is created once even when this allocator is shared.
MVKMTLBufferAllocationPool* MVKMTLBufferAllocator::getRegionPool(NSUInteger p2Exp) {
	std::atomic<MVKMTLBufferAllocationPool*>& poolSlot = _regionPools[p2Exp];
	MVKMTLBufferAllocationPool* pool = poolSlot.load(std::memory_order_acquire);
	if (pool) [[likely]] { return pool; }

	std::lock_guard<MVKUnfairLock> lock(_regionPoolsLock);
	pool = poolSlot.load(std::memory_order_relaxed);
	if ( !pool ) {
		pool = new MVKMTLBufferAllocationPool(_device, (NSUInteger)1 << p2Exp, _isThreadSafe, _isDedicated, _mtlStorageMode);
		poolSlot.store(pool, std::memory_order_release);
	}
	return pool;
}

MVKMTLBufferAllocator::MVKMTLBufferAllocator(MVKDevice* device, NSUInteger maxRegionLength, bool makeThreadSafe, bool isDedicated, MTLStorageMode mtlStorageMode) : MVKBaseDeviceObject(device) {
	_maxAllocationLength = std::max<NSUInteger>(maxRegionLength, getMetalFeatures().mtlBufferAlignment);
	_mtlStorageMode = mtlStorageMode;
	_isThreadSafe = makeThreadSafe;
	_isDedicated = isDedicated;

    // Convert max length to the next power-of-two exponent, and size the pool slots to cover it.
    NSUInteger maxP2Exp = mvkPowerOfTwoExponent(_maxAllocationLength);
	_regionPoolCount = maxP2Exp + 1;
	_regionPools.reset(new std::atomic<MVKMTLBufferAllocationPool*>[_regionPoolCount]);
	for (NSUInteger p2Exp = 0; p2Exp < _regionPoolCount; p2Exp++) {
		_regionPools[p2Exp].store(nullptr, std::memory_order_relaxed);
	}
}

MVKMTLBufferAllocator::~MVKMTLBufferAllocator() {
	for (NSUInteger p2Exp = 0; p2Exp < _regionPoolCount; p2Exp++) {
		MVKMTLBufferAllocationPool* pool = _regionPools[p2Exp].load(std::memory_order_relaxed);
		if (pool) { pool->destroy(); }
	}
}

