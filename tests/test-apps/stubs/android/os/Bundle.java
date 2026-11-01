/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.os;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

public class Bundle {
    private final Map<String, Object> mMap = new HashMap<>();

    public Bundle() {}
    public Bundle(Bundle other) {
        if (other != null) {
            mMap.putAll(other.mMap);
        }
    }

    public void putString(String key, String value) { mMap.put(key, value); }
    public String getString(String key) { Object v = mMap.get(key); return v instanceof String ? (String) v : null; }
    public void putInt(String key, int value) { mMap.put(key, value); }
    public int getInt(String key) { Object v = mMap.get(key); return v instanceof Integer ? (Integer) v : 0; }
    public void putLong(String key, long value) { mMap.put(key, value); }
    public long getLong(String key) { Object v = mMap.get(key); return v instanceof Long ? (Long) v : 0L; }
    public Object get(String key) { return mMap.get(key); }
    public Set<String> keySet() { return mMap.keySet(); }
    public void putAll(Bundle b) { if (b != null) mMap.putAll(b.mMap); }
    public void remove(String key) { mMap.remove(key); }
    public boolean hasFileDescriptors() { return false; }
}
