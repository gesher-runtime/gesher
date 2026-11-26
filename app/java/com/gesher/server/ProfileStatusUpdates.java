/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ContentValues;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.provider.ContactsContract.CommonDataKinds.Email;
import android.provider.ContactsContract.CommonDataKinds.Im;
import android.provider.ContactsContract.StatusUpdates;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Status text and session presence attached to existing Profile data rows. */
final class ProfileStatusUpdates {
    private static final String STATUS_TABLE = "profile_status";
    private static final String PRESENCE_TABLE = "profile_presence";
    static final String VIEW = "view_profile_status_updates";
    private static final List<String> STATUS_COLUMNS = List.of(StatusUpdates.STATUS,
            StatusUpdates.STATUS_TIMESTAMP, StatusUpdates.STATUS_RES_PACKAGE,
            StatusUpdates.STATUS_LABEL, StatusUpdates.STATUS_ICON);
    private static final List<String> PRESENCE_IDENTITY = List.of(StatusUpdates.PROTOCOL,
            StatusUpdates.CUSTOM_PROTOCOL, StatusUpdates.IM_HANDLE, StatusUpdates.IM_ACCOUNT);
    private static final List<String> PRESENCE_COLUMNS =
            List.of(StatusUpdates.PRESENCE, StatusUpdates.CHAT_CAPABILITY);
    static final Map<String, String> PROJECTION;

    static {
        Map<String, String> map = new LinkedHashMap<>();
        map.put(StatusUpdates.DATA_ID, StatusUpdates.DATA_ID);
        map.put("raw_contact_id", "raw_contact_id");
        for (List<String> names : List.of(PRESENCE_IDENTITY, PRESENCE_COLUMNS, STATUS_COLUMNS)) {
            for (String name : names) map.put(name, name);
        }
        PROJECTION = Map.copyOf(map);
    }

    private ProfileStatusUpdates() { }

    static void createTables(SQLiteDatabase db) {
        String dataKey = " (presence_data_id INTEGER PRIMARY KEY REFERENCES data(_id) ON DELETE CASCADE,";
        db.execSQL("CREATE TABLE " + STATUS_TABLE + dataKey
                + " status TEXT, status_ts INTEGER, status_res_package TEXT,"
                + " status_label INTEGER, status_icon INTEGER)");
        db.execSQL("CREATE TABLE " + PRESENCE_TABLE + dataKey
                + " protocol INTEGER, custom_protocol TEXT, im_handle TEXT, im_account TEXT,"
                + " mode INTEGER, chat_capability INTEGER NOT NULL DEFAULT 0,"
                + " UNIQUE(protocol, custom_protocol, im_handle, im_account))");
        db.execSQL("CREATE VIEW " + VIEW + " AS SELECT d._id AS presence_data_id, d.raw_contact_id,"
                + " p.protocol, NULLIF(p.custom_protocol, '') AS custom_protocol, p.im_handle, p.im_account,"
                + " p.mode, p.chat_capability, s.status, s.status_ts, s.status_res_package,"
                + " s.status_label, s.status_icon FROM view_data d"
                + " LEFT JOIN profile_presence p ON p.presence_data_id = d._id"
                + " LEFT JOIN profile_status s ON s.presence_data_id = d._id"
                + " WHERE d.is_user_profile = 1");
    }

    static void clearPresence(SQLiteDatabase db) {
        db.delete(PRESENCE_TABLE, null, null);
        db.execSQL("UPDATE contacts SET contact_presence = NULL, contact_chat_capability = NULL"
                + " WHERE is_user_profile = 1");
    }

    static long upsert(SQLiteDatabase db, ContentValues input, long now) {
        Integer protocol = input.getAsInteger(StatusUpdates.PROTOCOL);
        String custom = input.getAsString(StatusUpdates.CUSTOM_PROTOCOL);
        if (protocol != null && protocol == Im.PROTOCOL_CUSTOM && (custom == null || custom.isEmpty())) {
            throw new IllegalArgumentException("A custom IM protocol requires a name");
        }
        long id = resolveDataId(db, input, protocol, custom);
        if (id == 0) return 0;
        if (input.getAsInteger(StatusUpdates.PRESENCE) != null) {
            ContentValues presence = subset(input, PRESENCE_IDENTITY);
            presence.putAll(subset(input, PRESENCE_COLUMNS));
            if (custom == null) presence.put(StatusUpdates.CUSTOM_PROTOCOL, "");
            if (presence.get(StatusUpdates.CHAT_CAPABILITY) == null) {
                presence.put(StatusUpdates.CHAT_CAPABILITY, 0);
            }
            presence.put(StatusUpdates.DATA_ID, id);
            db.replaceOrThrow(PRESENCE_TABLE, null, presence);
        }
        if (input.containsKey(StatusUpdates.STATUS)) {
            String status = input.getAsString(StatusUpdates.STATUS);
            if (status == null || status.isEmpty()) {
                db.delete(STATUS_TABLE, "presence_data_id = ?", idArgs(id));
            } else {
                ContentValues values = subset(input, STATUS_COLUMNS);
                Integer label = values.getAsInteger(StatusUpdates.STATUS_LABEL);
                if ((label == null || label == 0) && protocol != null) {
                    values.put(StatusUpdates.STATUS_LABEL, Im.getProtocolLabelResource(protocol));
                }
                if (values.getAsLong(StatusUpdates.STATUS_TIMESTAMP) == null) {
                    long time = now;
                    try (Cursor c = db.query(STATUS_TABLE, new String[] {"status", "status_ts"},
                            "presence_data_id = ?", idArgs(id), null, null, null)) {
                        if (c.moveToFirst() && status.equals(c.getString(0)) && !c.isNull(1)) time = c.getLong(1);
                    }
                    values.put(StatusUpdates.STATUS_TIMESTAMP, time);
                }
                values.put(StatusUpdates.DATA_ID, id);
                db.replaceOrThrow(STATUS_TABLE, null, values);
            }
        }
        return id;
    }

    private static long resolveDataId(SQLiteDatabase db, ContentValues input, Integer protocol,
            String custom) {
        Long dataId = input.getAsLong(StatusUpdates.DATA_ID);
        String selection;
        String[] args;
        if (dataId != null) {
            selection = "_id = ?";
            args = idArgs(dataId);
        } else {
            String handle = input.getAsString(StatusUpdates.IM_HANDLE);
            if (handle == null || handle.isEmpty() || protocol == null) {
                throw new IllegalArgumentException("Specify a Profile data ID or an IM handle and protocol");
            }
            selection = "(mimetype = ? AND data5 = ? AND data1 = ?";
            List<String> bindings = new ArrayList<>(List.of(Im.CONTENT_ITEM_TYPE, protocol.toString(), handle));
            if (protocol == Im.PROTOCOL_CUSTOM) {
                selection += " AND data6 = ?";
                bindings.add(custom);
            }
            selection += ")";
            if (protocol == Im.PROTOCOL_GOOGLE_TALK) {
                selection = "(" + selection + " OR (mimetype = ? AND data1 = ?))";
                bindings.add(Email.CONTENT_ITEM_TYPE);
                bindings.add(handle);
            }
            args = bindings.toArray(String[]::new);
        }
        try (Cursor c = db.query("view_data", new String[] {"_id"},
                "is_user_profile = 1 AND (" + selection + ")", args, null, null,
                "in_visible_group DESC, raw_contact_id, _id", "1")) {
            return c.moveToFirst() ? c.getLong(0) : 0;
        }
    }

    static int mutate(SQLiteDatabase db, ContentValues update, String selection, String[] args) {
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables(VIEW);
        qb.setProjectionMap(PROJECTION);
        qb.setStrict(true);
        qb.setStrictColumns(true);
        qb.setStrictGrammar(true);
        List<Long> ids = new ArrayList<>();
        try (Cursor c = qb.query(db, new String[] {StatusUpdates.DATA_ID},
                selection, args, null, null, null)) {
            while (c.moveToNext()) ids.add(c.getLong(0));
        }
        int count = 0;
        ContentValues status = update == null ? null : subset(update, STATUS_COLUMNS);
        ContentValues presence = update == null ? null : subset(update, PRESENCE_COLUMNS);
        for (long id : ids) {
            int statusCount = mutateRow(db, STATUS_TABLE, id, status);
            int presenceCount = mutateRow(db, PRESENCE_TABLE, id, presence);
            count += Math.max(statusCount, presenceCount);
        }
        return count;
    }

    private static int mutateRow(SQLiteDatabase db, String table, long id, ContentValues values) {
        String key = "presence_data_id = ?";
        if (values == null) return db.delete(table, key, idArgs(id));
        return values.size() == 0 ? 0 : db.update(table, values, key, idArgs(id));
    }

    static void applyAggregate(SQLiteDatabase db, long contactId, ContentValues contact) {
        String[] args = idArgs(contactId);
        copyAggregate(db, contact, "profile_presence", new String[] {"mode", "chat_capability"},
                new String[] {"contact_presence", "contact_chat_capability"}, args,
                "(COALESCE(mode, 0) * 10 + COALESCE(chat_capability, 0)) DESC");
        copyAggregate(db, contact, "profile_status",
                new String[] {"status", "status_ts", "status_res_package", "status_label", "status_icon"},
                new String[] {"contact_status", "contact_status_ts", "contact_status_res_package",
                        "contact_status_label", "contact_status_icon"}, args, "status_ts DESC");
    }

    private static void copyAggregate(SQLiteDatabase db, ContentValues contact, String table,
            String[] source, String[] destination, String[] args, String order) {
        try (Cursor c = db.query(table, source,
                "presence_data_id IN (SELECT _id FROM view_data WHERE contact_id = ?)", args,
                null, null, order + ", presence_data_id DESC", "1")) {
            boolean found = c.moveToFirst();
            for (int i = 0; i < source.length; i++) {
                if (!found || c.isNull(i)) contact.putNull(destination[i]);
                else if (c.getType(i) == Cursor.FIELD_TYPE_INTEGER) contact.put(destination[i], c.getLong(i));
                else contact.put(destination[i], c.getString(i));
            }
        }
    }

    private static String[] idArgs(long id) {
        return new String[] {Long.toString(id)};
    }

    private static ContentValues subset(ContentValues values, List<String> columns) {
        ContentValues result = new ContentValues();
        for (String column : columns) {
            if (values.containsKey(column)) result.put(column, values.getAsString(column));
        }
        return result;
    }
}
