/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.splitcontract;
public final class FeatureProvider extends android.content.ContentProvider {
    public boolean onCreate() { return "base:core:entry".equals(EntryMarker.value()); }
    public android.database.Cursor query(android.net.Uri uri, String[] projection,
            String selection, String[] args, String order) { return null; }
    public String getType(android.net.Uri uri) { return null; }
    public android.net.Uri insert(android.net.Uri uri, android.content.ContentValues values) { return null; }
    public int update(android.net.Uri uri, android.content.ContentValues values, String selection,
            String[] args) { return 0; }
    public int delete(android.net.Uri uri, String selection, String[] args) { return 0; }
}
