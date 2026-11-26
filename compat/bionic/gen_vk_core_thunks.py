#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Emit dispatch metadata for Vulkan 1.0 using the pinned Khronos declarations."""
import re
import sys
from pathlib import Path


INSTANCE_HANDWRITTEN = {
    "vkCreateDevice",
    "vkGetPhysicalDeviceProperties",
    "vkEnumerateDeviceLayerProperties",
    "vkEnumerateDeviceExtensionProperties",
}
GLOBAL_HANDWRITTEN = {
    "vkCreateInstance",
    "vkGetInstanceProcAddr",
    "vkEnumerateInstanceLayerProperties",
    "vkEnumerateInstanceExtensionProperties",
}
DEVICE_HANDWRITTEN = {"vkGetDeviceProcAddr", "vkCreateGraphicsPipelines", "vkCreateComputePipelines"}


def main():
    source = Path(sys.argv[2]).read_text()
    core = source.split("#define VK_VERSION_1_1 1", 1)[0].split("#define VK_VERSION_1_0 1", 1)[1]
    commands = re.findall(r"VKAPI_ATTR (\w+) VKAPI_CALL (vk\w+)\(\s*(.*?)\);", core, re.S)
    names = {name for _, name, _ in commands}
    if len(commands) != 137 or len(names) != len(commands):
        raise SystemExit("Pinned Vulkan header must contain 137 distinct core 1.0 commands")
    handwritten = GLOBAL_HANDWRITTEN | INSTANCE_HANDWRITTEN | DEVICE_HANDWRITTEN
    if not handwritten <= names:
        raise SystemExit(f"Missing handwritten Vulkan declarations: {handwritten - names}")
    rows = ["// Generated from pinned vulkan_core.h. Do not edit."]
    for result, name, params in commands:
        if name in handwritten:
            continue
        dispatch = params.split(maxsplit=1)[0]
        if dispatch in {"VkInstance", "VkPhysicalDevice"}:
            domain = "Instance"
        elif dispatch in {"VkDevice", "VkQueue", "VkCommandBuffer"}:
            domain = "Device"
        else:
            raise SystemExit(f"Invalid dispatch parameter for {name}: {dispatch}")
        if "const VkAllocationCallbacks* " in params:
            if result not in {"VkResult", "void"}:
                raise SystemExit(f"Unsupported allocation result for {name}: {result}")
            params = re.sub(r"\s+", " ", params)
            args = ", ".join(p.split()[-1].rstrip("]").split("[", 1)[0] for p in params.split(","))
            rows.append(f"VK_ALLOCATOR_{result.upper()}({domain}, {name}, ({params}), ({args}))")
        else:
            rows.append(f"VK_DIRECT({domain}, {name})")
    Path(sys.argv[1]).write_text("\n".join(rows) + "\n")


if __name__ == "__main__":
    main()
