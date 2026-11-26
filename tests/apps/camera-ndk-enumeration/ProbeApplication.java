/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package org.gesher.contracts.camerandk;

import android.app.Application;

public final class ProbeApplication extends Application {
    static { System.loadLibrary("camera-enumeration"); }

    static native String[] enumerate();

    // Application construction occurs before ActivityThread publishes the Application.
    final String[] startupIds = enumerate();
}
