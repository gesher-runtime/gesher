/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

/**
 * The Connectivity module's TransportInfo (framework.jar compiles against the module's API
 * and does not contain it). Gesher's networks carry none (docs/network.md).
 */
public interface TransportInfo {
    /** @hide */
    default long getApplicableRedactions() {
        return 0;
    }

    /** @hide */
    default TransportInfo makeCopy(long redactions) {
        return this;
    }
}
