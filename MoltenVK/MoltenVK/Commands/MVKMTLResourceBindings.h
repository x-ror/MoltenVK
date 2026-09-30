/*
 * MVKMTLResourceBindings.h
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

#include "mvk_vulkan.h"

#import <Metal/Metal.h>


class MVKResource;
class MVKBuffer;
class MVKImage;


/** Describes a MTLBuffer resource binding used for a vertex buffer. */
struct MVKVertexMTLBufferBinding {
	union { id<MTLBuffer> mtlBuffer = nil; id<MTLBuffer> mtlResource; }; // aliases
	VkDeviceSize offset = 0;
	uint32_t size = 0;
	uint32_t stride = 0;
};

/** Describes a MTLBuffer resource binding as used for an index buffer. */
struct MVKIndexMTLBufferBinding {
    union { id<MTLBuffer> mtlBuffer = nil; id<MTLBuffer> mtlResource; }; // aliases
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    VkIndexType vkIndexType = VK_INDEX_TYPE_UINT32;
    uint8_t mtlIndexType = 0;		// MTLIndexType
};

/** Concise and consistent structure for holding pipeline barrier info. */
struct MVKPipelineBarrier {

	typedef enum : uint8_t {
		None,
		Memory,
		Buffer,
		Image,
		Execution,
	} MVKPipelineBarrierType;

	// The 8-byte members come first and the byte-sized ones last, so the struct
	// carries no padding. It is stored inline in every pipeline barrier command.
	VkPipelineStageFlags2 srcStageMask = 0;
	VkAccessFlags2 srcAccessMask = 0;
	VkPipelineStageFlags2 dstStageMask = 0;
	VkAccessFlags2 dstAccessMask = 0;
	union { MVKBuffer* mvkBuffer = nullptr; MVKImage* mvkImage; MVKResource* mvkResource; };
	union {
		struct {
			VkDeviceSize offset = 0;
			VkDeviceSize size = 0;
		};
		struct {
			VkImageLayout newLayout;
			VkImageAspectFlags aspectMask;
			uint16_t baseArrayLayer;
			uint16_t layerCount;
			uint8_t baseMipLevel;
			uint8_t levelCount;
		};
	};
	MVKPipelineBarrierType type = None;
	uint8_t srcQueueFamilyIndex = 0;
	uint8_t dstQueueFamilyIndex = 0;

	bool isMemoryBarrier() { return type == Memory; }
	bool isBufferBarrier() { return type == Buffer; }
	bool isImageBarrier() { return type == Image; }

	MVKPipelineBarrier(VkPipelineStageFlags srcStageMask,
					   VkPipelineStageFlags dstStageMask) :
		srcStageMask(srcStageMask),
		dstStageMask(dstStageMask),
		type(Execution)
		{}

	MVKPipelineBarrier(const VkMemoryBarrier2& vkBarrier) :
		srcStageMask(vkBarrier.srcStageMask),
		srcAccessMask(vkBarrier.srcAccessMask),
		dstStageMask(vkBarrier.dstStageMask),
		dstAccessMask(vkBarrier.dstAccessMask),
		type(Memory)
		{}

	MVKPipelineBarrier(const VkMemoryBarrier& vkBarrier,
					   VkPipelineStageFlags srcStageMask,
					   VkPipelineStageFlags dstStageMask) :
		srcStageMask(srcStageMask),
		srcAccessMask(vkBarrier.srcAccessMask),
		dstStageMask(dstStageMask),
		dstAccessMask(vkBarrier.dstAccessMask),
		type(Memory)
		{}

	MVKPipelineBarrier(const VkBufferMemoryBarrier2& vkBarrier) :
		srcStageMask(vkBarrier.srcStageMask),
		srcAccessMask(vkBarrier.srcAccessMask),
		dstStageMask(vkBarrier.dstStageMask),
		dstAccessMask(vkBarrier.dstAccessMask),
		mvkBuffer((MVKBuffer*)vkBarrier.buffer),
		offset(vkBarrier.offset),
		size(vkBarrier.size),
		type(Buffer),
		srcQueueFamilyIndex(vkBarrier.srcQueueFamilyIndex),
		dstQueueFamilyIndex(vkBarrier.dstQueueFamilyIndex)
		{}

	MVKPipelineBarrier(const VkBufferMemoryBarrier& vkBarrier,
					   VkPipelineStageFlags srcStageMask,
					   VkPipelineStageFlags dstStageMask) :
		srcStageMask(srcStageMask),
		srcAccessMask(vkBarrier.srcAccessMask),
		dstStageMask(dstStageMask),
		dstAccessMask(vkBarrier.dstAccessMask),
		mvkBuffer((MVKBuffer*)vkBarrier.buffer),
		offset(vkBarrier.offset),
		size(vkBarrier.size),
		type(Buffer),
		srcQueueFamilyIndex(vkBarrier.srcQueueFamilyIndex),
		dstQueueFamilyIndex(vkBarrier.dstQueueFamilyIndex)
		{}

	MVKPipelineBarrier(const VkImageMemoryBarrier2& vkBarrier) :
		srcStageMask(vkBarrier.srcStageMask),
		srcAccessMask(vkBarrier.srcAccessMask),
		dstStageMask(vkBarrier.dstStageMask),
		dstAccessMask(vkBarrier.dstAccessMask),
		mvkImage((MVKImage*)vkBarrier.image),
		newLayout(vkBarrier.newLayout),
		aspectMask(vkBarrier.subresourceRange.aspectMask),
		baseArrayLayer(vkBarrier.subresourceRange.baseArrayLayer),
		layerCount(vkBarrier.subresourceRange.layerCount),
		baseMipLevel(vkBarrier.subresourceRange.baseMipLevel),
		levelCount(vkBarrier.subresourceRange.levelCount),
		type(Image),
		srcQueueFamilyIndex(vkBarrier.srcQueueFamilyIndex),
		dstQueueFamilyIndex(vkBarrier.dstQueueFamilyIndex)
		{}

	MVKPipelineBarrier(const VkImageMemoryBarrier& vkBarrier,
					   VkPipelineStageFlags srcStageMask,
					   VkPipelineStageFlags dstStageMask) :
		srcStageMask(srcStageMask),
		srcAccessMask(vkBarrier.srcAccessMask),
		dstStageMask(dstStageMask),
		dstAccessMask(vkBarrier.dstAccessMask),
		mvkImage((MVKImage*)vkBarrier.image),
		newLayout(vkBarrier.newLayout),
		aspectMask(vkBarrier.subresourceRange.aspectMask),
		baseArrayLayer(vkBarrier.subresourceRange.baseArrayLayer),
		layerCount(vkBarrier.subresourceRange.layerCount),
		baseMipLevel(vkBarrier.subresourceRange.baseMipLevel),
		levelCount(vkBarrier.subresourceRange.levelCount),
		type(Image),
		srcQueueFamilyIndex(vkBarrier.srcQueueFamilyIndex),
		dstQueueFamilyIndex(vkBarrier.dstQueueFamilyIndex)
		{}

};

