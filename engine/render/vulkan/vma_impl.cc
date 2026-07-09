// Single TU for the VMA implementation. Function pointers come from volk at
// allocator creation, nothing links against a vulkan loader.
#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
