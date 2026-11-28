// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace gesher {

inline constexpr std::string_view kProtectedBinderStartupOption = "--gesher-protected-services";

// Native executable entry points use this before invoking Android code.
// The arguments constrain the route. Each connection still authenticates
// the Windows pipe and binderd admits the exact registered process.
bool ConfigureProtectedBinderStartup(std::string_view locator, std::string_view authority_pid,
                                     std::string_view desktop_session);

}  // namespace gesher
