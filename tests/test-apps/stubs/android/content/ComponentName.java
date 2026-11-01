/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content;

public class ComponentName {
    private final String mPackage;
    private final String mClass;

    public ComponentName(String pkg, String cls) {
        mPackage = pkg;
        mClass = cls;
    }

    public String getPackageName() {
        return mPackage;
    }

    public String getClassName() {
        return mClass;
    }
}
