/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content;

import android.os.Bundle;

public class Intent {
    private String mAction;
    private String mPackage;
    private ComponentName mComponent;
    private Bundle mExtras = new Bundle();

    public Intent() {}
    public Intent(String action) { mAction = action; }
    public Intent(Intent other) {
        if (other != null) {
            mAction = other.mAction;
            mPackage = other.mPackage;
            mComponent = other.mComponent;
            mExtras = new Bundle(other.mExtras);
        }
    }

    public String getAction() { return mAction; }
    public Intent setPackage(String pkg) { mPackage = pkg; return this; }
    public String getPackage() { return mPackage; }
    public Intent setComponent(ComponentName comp) { mComponent = comp; return this; }
    public ComponentName getComponent() { return mComponent; }
    public Intent putExtra(String name, String value) { mExtras.putString(name, value); return this; }
    public String getStringExtra(String name) { return mExtras.getString(name); }
    public boolean hasExtra(String name) { return mExtras.get(name) != null; }
    public <T> T getParcelableExtra(String name) {
        @SuppressWarnings("unchecked")
        T res = (T) mExtras.get(name);
        return res;
    }
    public Bundle getExtras() { return mExtras; }
    public Intent putExtras(Bundle extras) { if (extras != null) mExtras.putAll(extras); return this; }
    public Intent putExtras(Intent src) { if (src != null) mExtras.putAll(src.mExtras); return this; }
}
