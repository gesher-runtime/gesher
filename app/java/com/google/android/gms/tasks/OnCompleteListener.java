/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.tasks;

public interface OnCompleteListener<TResult> {
    void onComplete(Task<TResult> task);
}
