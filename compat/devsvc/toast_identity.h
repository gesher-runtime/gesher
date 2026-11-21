/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <string_view>

namespace gesher::devsvc {
// The C WinRT boundary takes NUL-terminated strings. An expected key must not
// silently become another identity when it crosses that boundary.
inline bool ValidToastIdentityArguments(std::u16string_view app_id, std::u16string_view tag,
                                         std::u16string_view group, std::u16string_view launch) {
  return app_id.find(u'\0') == app_id.npos && tag.find(u'\0') == tag.npos
      && group.find(u'\0') == group.npos && launch.find(u'\0') == launch.npos;
}
}  // namespace gesher::devsvc
