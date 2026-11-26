// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Public Vulkan loader, dispatch pointers, and a headless queue operation.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

static int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "Vulkan core check failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int fill_buffer(VkDevice device, uint32_t queue_family,
                       const VkPhysicalDeviceMemoryProperties* memory_props,
                       PFN_vkGetDeviceProcAddr device_proc) {
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    CHECK(queue != VK_NULL_HANDLE);
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 256,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    CHECK(vkCreateBuffer(device, &bi, NULL, &buffer) == VK_SUCCESS);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    uint32_t mem = 0;
    const VkMemoryPropertyFlags flags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (mem < memory_props->memoryTypeCount &&
           (!(req.memoryTypeBits & (1u << mem)) ||
            (memory_props->memoryTypes[mem].propertyFlags & flags) != flags)) {
        ++mem;
    }
    if (mem == memory_props->memoryTypeCount) {
        vkDestroyBuffer(device, buffer, NULL);
        puts("Vulkan queue check unavailable: buffer has no host-visible coherent memory type");
        return 0;
    }
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mem,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &ai, NULL, &memory) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    VkCommandPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &pi, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &ci, &cmd) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
    const uint32_t pattern = 0x47736872;
    PFN_vkCmdFillBuffer cmd_fill = (PFN_vkCmdFillBuffer)device_proc(device, "vkCmdFillBuffer");
    CHECK(cmd_fill != NULL);
    cmd_fill(cmd, buffer, 0, 256, pattern);
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(device, &fi, NULL, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    PFN_vkQueueSubmit queue_submit = (PFN_vkQueueSubmit)device_proc(device, "vkQueueSubmit");
    CHECK(queue_submit != NULL);
    CHECK(queue_submit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(5000000000)) == VK_SUCCESS);
    void* map = NULL;
    CHECK(vkMapMemory(device, memory, 0, 256, 0, &map) == VK_SUCCESS);
    CHECK(map != NULL);
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(((const uint32_t*)map)[i] == pattern);
    }
    vkUnmapMemory(device, memory);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    puts("Vulkan queue wrote exact pattern to mapped buffer");
    return 0;
}

static int run_vulkan_core(int limited) {
    void* vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    CHECK(vulkan != NULL);
    PFN_vkGetInstanceProcAddr get_instance_proc =
        (PFN_vkGetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr");
    CHECK(get_instance_proc != NULL);
    PFN_vkCreateInstance create = (PFN_vkCreateInstance) get_instance_proc(NULL, "vkCreateInstance");
    CHECK(create != NULL && dlsym(vulkan, "vkCreateInstance") != NULL);
    CHECK(get_instance_proc(NULL, "vkQueueSubmit") == NULL);
    CHECK(get_instance_proc(NULL, "vkDefinitelyNotAnApi") == NULL);
    uint32_t layer_count = UINT32_MAX;
    uint32_t extension_count = UINT32_MAX;
    CHECK(vkEnumerateInstanceLayerProperties(&layer_count, NULL) == VK_SUCCESS);
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL) == VK_SUCCESS);
    if (limited) {
        CHECK(layer_count == 0 && extension_count == 0);
        CHECK(dlsym(vulkan, "vkCreateAndroidSurfaceKHR") == NULL);
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Gesher Vulkan core contract",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    if (limited) {
        const char* missing = "VK_GESHER_unavailable_contract_fixture";
        ii.enabledExtensionCount = 1;
        ii.ppEnabledExtensionNames = &missing;
        CHECK(create(&ii, NULL, &instance) == VK_ERROR_EXTENSION_NOT_PRESENT);
        ii.enabledExtensionCount = 0;
        app.apiVersion = VK_API_VERSION_1_1;
        CHECK(create(&ii, NULL, &instance) == VK_ERROR_INCOMPATIBLE_DRIVER);
        app.apiVersion = VK_API_VERSION_1_0;
    }
    VkResult result = create(&ii, NULL, &instance);
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_INITIALIZATION_FAILED) {
        CHECK(instance == VK_NULL_HANDLE);
        CHECK(dlclose(vulkan) == 0);
        printf("Vulkan core unavailable (vkCreateInstance=%d): %d checks passed\n", result, checks);
        return 0;
    }
    CHECK(result == VK_SUCCESS && instance != VK_NULL_HANDLE);
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)get_instance_proc(instance, "vkEnumeratePhysicalDevices");
    CHECK(enumerate != NULL);
    uint32_t physical_count = 0;
    result = enumerate(instance, &physical_count, NULL);
    // Some loaders fail enumeration when no ICD can supply a device.
    CHECK(result == VK_SUCCESS || result == VK_ERROR_INITIALIZATION_FAILED);
    if (result == VK_ERROR_INITIALIZATION_FAILED || physical_count == 0) {
        vkDestroyInstance(instance, NULL);
        CHECK(dlclose(vulkan) == 0);
        printf("Vulkan core no physical device (enumeration=%d): %d checks passed\n", result, checks);
        return 0;
    }
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t capacity = 1;
    result = enumerate(instance, &capacity, &physical);
    CHECK((result == VK_SUCCESS || result == VK_INCOMPLETE)
          && capacity == 1 && physical != VK_NULL_HANDLE);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    CHECK(props.deviceName[0] != '\0');
    CHECK(props.limits.maxMemoryAllocationCount != 0);
    if (limited) CHECK(props.apiVersion == VK_API_VERSION_1_0);
    printf("Vulkan physical device: %s vendor=%x driver=%x api=%x\n",
           props.deviceName, props.vendorID, props.driverVersion, props.apiVersion);
    CHECK(vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL) == VK_SUCCESS);
    if (limited) CHECK(extension_count == 0);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    CHECK(memory.memoryTypeCount > 0 && memory.memoryTypeCount <= VK_MAX_MEMORY_TYPES);
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, NULL);
    CHECK(queue_count > 0);
    VkQueueFamilyProperties* queues = calloc(queue_count, sizeof(*queues));
    CHECK(queues != NULL);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues);
    uint32_t family = 0;
    const VkQueueFlags supported = VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    while (family < queue_count &&
           (!(queues[family].queueFlags & supported) || queues[family].queueCount == 0)) ++family;
    free(queues);
    CHECK(family < queue_count);
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qi,
    };
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)get_instance_proc(instance, "vkCreateDevice");
    CHECK(create_device != NULL);
    VkDevice device = VK_NULL_HANDLE;
    CHECK(create_device(physical, &di, NULL, &device) == VK_SUCCESS);
    PFN_vkGetDeviceProcAddr get_device_proc =
        (PFN_vkGetDeviceProcAddr)get_instance_proc(instance, "vkGetDeviceProcAddr");
    CHECK(get_device_proc != NULL);
    CHECK(get_device_proc(device, "vkEnumeratePhysicalDevices") == NULL);
    CHECK(fill_buffer(device, family, &memory, get_device_proc) == 0);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    CHECK(dlclose(vulkan) == 0);
    printf("Vulkan core: %d checks passed\n", checks);
    return 0;
}

#ifdef GESHER_VULKAN_JNI
#include <jni.h>
JNIEXPORT jint JNICALL Java_gesher_VulkanCoreTest_run(JNIEnv* env, jclass cls, jboolean limited) {
    (void)env;
    (void)cls;
    return run_vulkan_core(limited);
}
#else
int main(int argc, char** argv) {
    return run_vulkan_core(argc == 2 && strcmp(argv[1], "core-only") == 0);
}
#endif
