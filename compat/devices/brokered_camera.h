// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "camera_mf.h"
#include <gesher/capture_policy.h>

#include <functional>
#include <string>
#include <vector>

namespace gesher::devices::camera {

const std::vector<Device>& ListBrokered(std::string* status);
std::unique_ptr<Capture> OpenBrokered(const std::string& id, int width, int height,
                                     capture_policy::Attribution attribution, HRESULT* hr);
void ServeBrokeredCamera(HANDLE pipe, bool info_only, const std::function<bool()>& live,
                         const std::function<bool(const capture_policy::Attribution&)>& start,
                         const std::function<bool()>& authorized, bool slot);

}  // namespace gesher::devices::camera
