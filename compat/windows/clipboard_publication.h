/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace gesher::windows {

// The completion sequence belongs only to this accepted immutable publication.
// It requires the original OLE object, all preferred native buffers and a stable
// actual sequence. Zero means that ownership or completion could not be proved.
// Every later sequence change is external to that receipt, even for equal text.
bool PublishClipboard(const std::u16string& text, bool has_text,
                      const std::u16string& html, const uint8_t* image, size_t image_size,
                      uint32_t* written_sequence = nullptr);
bool ClearClipboardPublication(uint32_t* written_sequence = nullptr);

// Each live ClipboardService keeps one lease. Temporary direct writers hold an
// operation lease, so the shared STA cannot retire beneath another live user.
// Releasing the last lease flushes only a still-current, unchanged publication
// and retires its STA. Failure leaves no claimed completion or fabricated data.
bool RetainClipboardPublisher();
void ReleaseClipboardPublisher();

}  // namespace gesher::windows
