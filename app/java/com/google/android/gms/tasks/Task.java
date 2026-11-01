/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.tasks;

import java.util.concurrent.Executor;

/**
 * Represents an asynchronous operation in Google Play Services.
 */
public abstract class Task<TResult> {
    public abstract boolean isComplete();

    public abstract boolean isSuccessful();

    public abstract boolean isCanceled();

    public abstract TResult getResult();

    public abstract <X extends Throwable> TResult getResult(Class<X> exceptionType) throws X;

    public abstract Exception getException();

    public abstract Task<TResult> addOnSuccessListener(OnSuccessListener<? super TResult> listener);

    public abstract Task<TResult> addOnSuccessListener(Executor executor,
            OnSuccessListener<? super TResult> listener);

    public abstract Task<TResult> addOnFailureListener(OnFailureListener listener);

    public abstract Task<TResult> addOnFailureListener(Executor executor,
            OnFailureListener listener);

    public abstract Task<TResult> addOnCompleteListener(OnCompleteListener<TResult> listener);

    public abstract Task<TResult> addOnCompleteListener(Executor executor,
            OnCompleteListener<TResult> listener);

    public abstract Task<TResult> addOnCanceledListener(OnCanceledListener listener);

    public abstract Task<TResult> addOnCanceledListener(Executor executor,
            OnCanceledListener listener);
}
