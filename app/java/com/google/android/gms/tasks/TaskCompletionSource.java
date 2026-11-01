/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.tasks;

public class TaskCompletionSource<TResult> {
    private final Tasks.TaskImpl<TResult> mTask = new Tasks.TaskImpl<>();

    public Task<TResult> getTask() {
        return mTask;
    }

    public void setResult(TResult result) {
        mTask.setResult(result);
    }

    public boolean trySetResult(TResult result) {
        return mTask.trySetResult(result);
    }

    public void setException(Exception e) {
        mTask.setException(e);
    }

    public boolean trySetException(Exception e) {
        return mTask.trySetException(e);
    }
}
