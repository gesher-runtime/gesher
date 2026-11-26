/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.accounts;

public interface AccountManagerCallback<V> {
    void run(AccountManagerFuture<V> future);
}
