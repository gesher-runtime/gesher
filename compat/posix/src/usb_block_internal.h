// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <gesher/file_description.h>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "internal.h"

namespace gesher::usb_block {

inline constexpr const char* kBlockName = "sdb";
bool Attached();
bool Writable();
bool SysContent(const std::string& name, std::string* content);
int Open(int flags);
int Stat(struct stat* info);
bool StatFd(const DescriptorSnapshotPtr& snapshot, struct stat* info, int* result);
bool Io(const DescriptorSnapshotPtr& snapshot, void* data, unsigned size,
        bool writing, int64_t offset, int* result);
bool Seek(const DescriptorSnapshotPtr& snapshot, int64_t offset, int whence, int64_t* result);
bool Sync(const DescriptorSnapshotPtr& snapshot, int* result);
bool Ioctl(const DescriptorSnapshotPtr& snapshot, unsigned long operation,
           void* output, int* result);

}  // namespace gesher::usb_block
