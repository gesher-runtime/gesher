// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Vulkan 1.0 core dispatch from the Android guest to the installed Windows loader.

#include "abi_types.h"

#include <gesher/bionic_shim.h>

#include <vulkan/vulkan_core.h>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace gesher::bionic {
namespace {

HMODULE Loader() {
  // Windows manages the process-architecture loader. Do not search the app's
  // directories or the current directory for a Windows DLL.
  static const HMODULE module = []() -> HMODULE {
    wchar_t path[MAX_PATH];
    constexpr wchar_t suffix[] = L"\\vulkan-1.dll";
    constexpr size_t suffix_length = sizeof(suffix) / sizeof(suffix[0]);
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length > MAX_PATH - suffix_length) return nullptr;
    std::copy_n(suffix, suffix_length, path + length);
    return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  }();
  return module;
}

template <typename Function>
Function Host(const char* name) {
  if (HMODULE module = Loader()) {
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
  }
  return nullptr;
}

template <typename Function, const char* Name>
struct Thunk;

template <typename R, typename... Args, const char* Name>
struct Thunk<R(VKAPI_PTR*)(Args...), Name> {
  static R BIONIC_SYSV Call(Args... args) {
    static const auto fn = Host<R(VKAPI_PTR*)(Args...)>(Name);
    if constexpr (std::is_same_v<R, VkResult>) {
      return fn != nullptr ? fn(args...) : VK_ERROR_INITIALIZATION_FAILED;
    } else if (fn != nullptr) {
      fn(args...);
    }
  }
};

// LP64 Vulkan core structs use fixed-width scalars, size_t and pointers, with
// identical layouts here. Each exported function is still a typed ABI thunk.
// Optional allocation callbacks contain guest code pointers. The core bridge
// supports only the default allocator, including for instance and device calls.
#define VK_DIRECT(domain, name)                                                \
  inline constexpr char kName_##name[] = #name;                                 \
  using Guest_##name = Thunk<PFN_##name, kName_##name>;
#define VK_ALLOCATOR_VKRESULT(domain, name, params, args)                       \
  struct Guest_##name {                                                        \
    static VkResult BIONIC_SYSV Call params {                                   \
      if (pAllocator != nullptr) return VK_ERROR_OUT_OF_HOST_MEMORY;             \
      static const auto fn = Host<PFN_##name>(#name);                            \
      return fn != nullptr ? fn args : VK_ERROR_INITIALIZATION_FAILED;           \
    }                                                                          \
  };
#define VK_ALLOCATOR_VOID(domain, name, params, args)                           \
  struct Guest_##name {                                                        \
    static void BIONIC_SYSV Call params {                                       \
      if (pAllocator != nullptr) return;                                       \
      static const auto fn = Host<PFN_##name>(#name);                            \
      if (fn != nullptr) fn args;                                               \
    }                                                                          \
  };
#include "vk_core_thunks.inc"
#undef VK_DIRECT
#undef VK_ALLOCATOR_VKRESULT
#undef VK_ALLOCATOR_VOID

VkResult BIONIC_SYSV CreateInstance(const VkInstanceCreateInfo* info,
                                   const VkAllocationCallbacks* allocator, VkInstance* out) {
  if (info == nullptr || out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  if (info->enabledLayerCount != 0) return VK_ERROR_LAYER_NOT_PRESENT;
  if (info->enabledExtensionCount != 0) return VK_ERROR_EXTENSION_NOT_PRESENT;
  if (allocator != nullptr) return VK_ERROR_OUT_OF_HOST_MEMORY;
  if (info->pNext != nullptr || info->flags != 0) return VK_ERROR_INITIALIZATION_FAILED;
  if (info->pApplicationInfo != nullptr) {
    // Core 1.0 permits rejecting a higher major/minor version at this entry
    // point. The patch component of the requested version is ignored.
    const uint32_t version = info->pApplicationInfo->apiVersion;
    if (VK_API_VERSION_VARIANT(version) != 0 ||
        VK_API_VERSION_MAJOR(version) > 1 || VK_API_VERSION_MINOR(version) > 0) {
      return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    if (info->pApplicationInfo->pNext != nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  }
  static const auto fn = Host<PFN_vkCreateInstance>("vkCreateInstance");
  return fn != nullptr ? fn(info, nullptr, out) : VK_ERROR_INCOMPATIBLE_DRIVER;
}

VkResult BIONIC_SYSV CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                 const VkAllocationCallbacks* allocator, VkDevice* out) {
  if (info == nullptr || out == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  // Deprecated device layers are ignored, as required by the current loader.
  if (info->enabledExtensionCount != 0) return VK_ERROR_EXTENSION_NOT_PRESENT;
  if (allocator != nullptr) return VK_ERROR_OUT_OF_HOST_MEMORY;
  if (info->pNext != nullptr || info->flags != 0) return VK_ERROR_INITIALIZATION_FAILED;
  for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
    if (info->pQueueCreateInfos[i].pNext != nullptr || info->pQueueCreateInfos[i].flags != 0) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }
  static const auto fn = Host<PFN_vkCreateDevice>("vkCreateDevice");
  return fn != nullptr ? fn(physical, info, nullptr, out) : VK_ERROR_INITIALIZATION_FAILED;
}

void BIONIC_SYSV GetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties* out) {
  static const auto fn = Host<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
  if (fn != nullptr) {
    fn(physical, out);
    // The API version is the implementation the application can actually use.
    // Hardware limits, identifiers and driver metadata are from the real ICD.
    if (out->apiVersion > VK_API_VERSION_1_0) out->apiVersion = VK_API_VERSION_1_0;
  }
}

VkResult BIONIC_SYSV EnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties*) {
  // No host layers or extension commands are exposed to the guest.
  *count = 0;
  return VK_SUCCESS;
}

VkResult BIONIC_SYSV EnumerateInstanceExtensionProperties(const char* layer,
                                                         uint32_t* count, VkExtensionProperties*) {
  if (layer != nullptr) return VK_ERROR_LAYER_NOT_PRESENT;
  *count = 0;
  return VK_SUCCESS;
}

VkResult BIONIC_SYSV EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* count,
                                                   VkLayerProperties* props) {
  return EnumerateInstanceLayerProperties(count, props);
}

VkResult BIONIC_SYSV EnumerateDeviceExtensionProperties(VkPhysicalDevice, const char* layer,
                                                       uint32_t* count, VkExtensionProperties* props) {
  return EnumerateInstanceExtensionProperties(layer, count, props);
}

VkResult BIONIC_SYSV CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
    const VkAllocationCallbacks* allocator, VkPipeline* out) {
  if (allocator != nullptr) {
    std::fill_n(out, count, VK_NULL_HANDLE);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  static const auto fn = Host<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  return fn != nullptr ? fn(device, cache, count, infos, nullptr, out) : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult BIONIC_SYSV CreateComputePipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo* infos,
    const VkAllocationCallbacks* allocator, VkPipeline* out) {
  if (allocator != nullptr) {
    std::fill_n(out, count, VK_NULL_HANDLE);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  static const auto fn = Host<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
  return fn != nullptr ? fn(device, cache, count, infos, nullptr, out) : VK_ERROR_INITIALIZATION_FAILED;
}

PFN_vkVoidFunction BIONIC_SYSV GetInstanceProcAddr(VkInstance instance, const char* name);
PFN_vkVoidFunction BIONIC_SYSV GetDeviceProcAddr(VkDevice device, const char* name);

enum class Domain { Global, Instance, Device };
struct Command {
  const char* name;
  Domain domain;
  PFN_vkVoidFunction guest;
};

const Command kCore[] = {
#define VK_ENTRY(domain, name, guest) \
  {#name, Domain::domain, reinterpret_cast<PFN_vkVoidFunction>(reinterpret_cast<void*>(&guest))},
#define VK_DIRECT(domain, name) VK_ENTRY(domain, name, Guest_##name::Call)
#define VK_ALLOCATOR_VKRESULT(domain, name, params, args) VK_DIRECT(domain, name)
#define VK_ALLOCATOR_VOID(domain, name, params, args) VK_DIRECT(domain, name)
#include "vk_core_thunks.inc"
  VK_ENTRY(Global, vkCreateInstance, CreateInstance)
  VK_ENTRY(Global, vkGetInstanceProcAddr, GetInstanceProcAddr)
  VK_ENTRY(Global, vkEnumerateInstanceExtensionProperties, EnumerateInstanceExtensionProperties)
  VK_ENTRY(Global, vkEnumerateInstanceLayerProperties, EnumerateInstanceLayerProperties)
  VK_ENTRY(Instance, vkCreateDevice, CreateDevice)
  VK_ENTRY(Instance, vkGetPhysicalDeviceProperties, GetPhysicalDeviceProperties)
  VK_ENTRY(Instance, vkEnumerateDeviceExtensionProperties, EnumerateDeviceExtensionProperties)
  VK_ENTRY(Instance, vkEnumerateDeviceLayerProperties, EnumerateDeviceLayerProperties)
  VK_ENTRY(Device, vkGetDeviceProcAddr, GetDeviceProcAddr)
  VK_ENTRY(Device, vkCreateGraphicsPipelines, CreateGraphicsPipelines)
  VK_ENTRY(Device, vkCreateComputePipelines, CreateComputePipelines)
#undef VK_ENTRY
#undef VK_DIRECT
#undef VK_ALLOCATOR_VKRESULT
#undef VK_ALLOCATOR_VOID
};

const Command* Find(const char* name) {
  if (name == nullptr) return nullptr;
  for (const auto& cmd : kCore) {
    if (std::strcmp(name, cmd.name) == 0) return &cmd;
  }
  return nullptr;
}

PFN_vkVoidFunction BIONIC_SYSV GetInstanceProcAddr(VkInstance instance, const char* name) {
  const auto* cmd = Find(name);
  if (cmd == nullptr) return nullptr;
  if (instance == VK_NULL_HANDLE) {
    return cmd->domain == Domain::Global && std::strcmp(name, "vkGetInstanceProcAddr") != 0
        ? cmd->guest : nullptr;
  }
  if (cmd->domain == Domain::Global) {
    return std::strcmp(name, "vkGetInstanceProcAddr") == 0 ? cmd->guest : nullptr;
  }
  static const auto fn = Host<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
  return fn != nullptr && fn(instance, name) != nullptr ? cmd->guest : nullptr;
}

PFN_vkVoidFunction BIONIC_SYSV GetDeviceProcAddr(VkDevice device, const char* name) {
  const auto* cmd = Find(name);
  if (cmd == nullptr || cmd->domain != Domain::Device) return nullptr;
  static const auto fn = Host<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
  return fn != nullptr && fn(device, name) != nullptr ? cmd->guest : nullptr;
}

[[maybe_unused]] const bool kRegistered = [] {
  for (const auto& cmd : kCore) AddBionicSymbol(cmd.name, reinterpret_cast<void*>(cmd.guest));
  return true;
}();

}  // namespace
}  // namespace gesher::bionic
