/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.accounts;

import java.io.IOException;

public interface AccountManagerFuture<V> {
    V getResult() throws OperationCanceledException, IOException, AuthenticatorException;
}
