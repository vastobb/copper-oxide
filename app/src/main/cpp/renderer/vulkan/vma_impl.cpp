// Vulkan Memory Allocator implementation.
//
// Android's libvulkan.so only guarantees Vulkan 1.0 entry points, so VMA is
// compiled against the Vulkan 1.0 surface and loads everything else through
// vkGetInstanceProcAddr. This keeps the shipped library free of undefined
// 1.1/1.2/1.3 symbol references at link time.
#define VMA_IMPLEMENTATION
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_VULKAN_VERSION 1000000
#include <vk_mem_alloc.h>