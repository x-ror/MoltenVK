/*
 * MVKPointerMap.h
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>


#pragma mark -
#pragma mark MVKPointerMap

/**
 * A hash map from pointers to small values, optimized for being filled and cleared repeatedly,
 * such as once per Metal command encoder.
 *
 * Uses open addressing with linear probing in a power-of-two table. Its storage is retained when
 * cleared, and clearing is constant time: each slot records the generation in which it was
 * written, and clearing starts a new generation, which makes all slots empty at once.
 *
 * Values must be trivially copyable. References returned by emplace() are invalidated by
 * the next emplace(), which may grow the table.
 */
template <typename Value>
class MVKPointerMap {

public:

	struct EmplaceResult {
		Value& value;		/**< The value stored for the key. */
		bool inserted;		/**< Whether the key was inserted, rather than already present. */
	};

	/**
	 * If the key is not present, inserts it with the given value. Returns the value stored
	 * for the key, and whether it was inserted. Like std::unordered_map::emplace(), an
	 * existing value is not replaced.
	 */
	EmplaceResult emplace(const void* key, const Value& value) {
		if ((_count + 1) * 2 > _slots.size()) { grow(); }
		size_t mask = _slots.size() - 1;
		for (size_t idx = indexFor(key); ; idx = (idx + 1) & mask) {
			Slot& slot = _slots[idx];
			if (slot.generation != _generation) {
				slot.key = key;
				slot.generation = _generation;
				slot.value = value;
				_count++;
				return { slot.value, true };
			}
			if (slot.key == key) { return { slot.value, false }; }
		}
	}

	/** Returns the value stored for the key, or null if the key is not present. */
	Value* find(const void* key) {
		if (_count == 0) { return nullptr; }
		size_t mask = _slots.size() - 1;
		for (size_t idx = indexFor(key); ; idx = (idx + 1) & mask) {
			Slot& slot = _slots[idx];
			if (slot.generation != _generation) { return nullptr; }
			if (slot.key == key) { return &slot.value; }
		}
	}

	/** Removes all keys, retaining the storage. */
	void clear() {
		if (_count == 0) { return; }
		_count = 0;
		if (++_generation == 0) {
			// The generation counter wrapped around. Mark every slot empty explicitly.
			for (auto& slot : _slots) { slot.generation = 0; }
			_generation = 1;
		}
	}

	/** Returns the number of keys in the map. */
	size_t size() const { return _count; }

	/** Returns whether the map contains no keys. */
	bool empty() const { return _count == 0; }

protected:

	struct Slot {
		const void* key = nullptr;
		uint32_t generation = 0;	// The slot is occupied only if this equals the map's generation.
		Value value = {};
	};

	static constexpr size_t kInitialCapacity = 64;

	// Fibonacci hashing: multiply by 2^64 / golden ratio, and take the top bits.
	// This spreads pointers well even though their low bits are always zero.
	size_t indexFor(const void* key) const {
		uint64_t hash = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(key)) * 0x9E3779B97F4A7C15ull;
		return static_cast<size_t>(hash >> _shift);
	}

	void grow() {
		std::vector<Slot> oldSlots = std::move(_slots);
		size_t newCapacity = oldSlots.empty() ? kInitialCapacity : oldSlots.size() * 2;
		_slots.assign(newCapacity, Slot());
		_shift = 64 - static_cast<uint32_t>(__builtin_ctzll(newCapacity));
		uint32_t oldGeneration = _generation;
		_generation = 1;
		_count = 0;
		for (auto& slot : oldSlots) {
			if (slot.generation == oldGeneration) { emplace(slot.key, slot.value); }
		}
	}

	std::vector<Slot> _slots;
	size_t _count = 0;
	uint32_t _generation = 1;
	uint32_t _shift = 64;
};
