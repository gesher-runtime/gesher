/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.android.server.uri;

/** Oracle boundary: the unmodified owner calls this after removing membership. */
interface UriGrantsManagerInternal {
    void removeUriPermissionIfNeeded(UriPermission permission);
}
