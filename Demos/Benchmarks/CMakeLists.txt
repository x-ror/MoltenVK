cmake_minimum_required(VERSION 3.16)
project(MoltenVKBenchmarks LANGUAGES CXX)

# Headless CPU benchmarks for MoltenVK. See README.md.
# The Vulkan library is loaded at runtime, so only the Vulkan headers are needed to build.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(VULKAN_HEADERS_DIR "" CACHE PATH "Directory containing vulkan/vulkan.h (default: MoltenVK's External/Vulkan-Headers, then the Vulkan SDK)")
find_path(VULKAN_HEADERS_INCLUDE_DIR vulkan/vulkan.h
	HINTS
		${VULKAN_HEADERS_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/../../External/Vulkan-Headers/include
		$ENV{VULKAN_SDK}/include
	NO_CMAKE_FIND_ROOT_PATH)
if(NOT VULKAN_HEADERS_INCLUDE_DIR)
	message(FATAL_ERROR "Vulkan headers not found. Run ./fetchDependencies in the MoltenVK root, or set -DVULKAN_HEADERS_DIR=<dir containing vulkan/vulkan.h>.")
endif()

find_package(Threads REQUIRED)

add_executable(mvkbench mvkbench.cpp)
target_include_directories(mvkbench PRIVATE ${VULKAN_HEADERS_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvkbench PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
	target_compile_options(mvkbench PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unknown-pragmas)
endif()
