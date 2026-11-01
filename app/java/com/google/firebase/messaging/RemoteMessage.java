/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.firebase.messaging;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/**
 * An incoming push message delivered through Firebase Cloud Messaging / GCM.
 */
public final class RemoteMessage {
    private final Bundle mBundle;
    private Map<String, String> mData;
    private Notification mNotification;

    public RemoteMessage(Bundle bundle) {
        mBundle = bundle != null ? new Bundle(bundle) : new Bundle();
    }

    public String getFrom() {
        return mBundle.getString("from");
    }

    public String getTo() {
        return mBundle.getString("google.to");
    }

    public String getCollapseKey() {
        return mBundle.getString("collapse_key");
    }

    public String getMessageId() {
        String id = mBundle.getString("google.message_id");
        if (id == null) {
            id = mBundle.getString("message_id");
        }
        return id;
    }

    public String getMessageType() {
        return mBundle.getString("message_type");
    }

    public long getSentTime() {
        Object time = mBundle.get("google.sent_time");
        if (time instanceof Long) {
            return (Long) time;
        } else if (time instanceof String) {
            try {
                return Long.parseLong((String) time);
            } catch (NumberFormatException ignored) {
            }
        }
        return 0L;
    }

    public int getTtl() {
        Object ttl = mBundle.get("google.ttl");
        if (ttl instanceof Integer) {
            return (Integer) ttl;
        } else if (ttl instanceof String) {
            try {
                return Integer.parseInt((String) ttl);
            } catch (NumberFormatException ignored) {
            }
        }
        return 0;
    }

    public Map<String, String> getData() {
        if (mData == null) {
            Map<String, String> map = new HashMap<>();
            for (String key : mBundle.keySet()) {
                if (key.startsWith("google.") || key.startsWith("gcm.") || key.equals("from")
                        || key.equals("message_type") || key.equals("collapse_key")) {
                    continue;
                }
                Object val = mBundle.get(key);
                if (val instanceof String) {
                    map.put(key, (String) val);
                }
            }
            mData = Collections.unmodifiableMap(map);
        }
        return mData;
    }

    public Notification getNotification() {
        if (mNotification == null) {
            String title = mBundle.getString("gcm.notification.title");
            String body = mBundle.getString("gcm.notification.body");
            if (title != null || body != null) {
                mNotification = new Notification(title, body,
                        mBundle.getString("gcm.notification.icon"),
                        mBundle.getString("gcm.notification.sound"),
                        mBundle.getString("gcm.notification.tag"),
                        mBundle.getString("gcm.notification.color"),
                        mBundle.getString("gcm.notification.click_action"));
            }
        }
        return mNotification;
    }

    public Intent toIntent() {
        Intent intent = new Intent("com.google.android.c2dm.intent.RECEIVE");
        intent.putExtras(mBundle);
        return intent;
    }

    public static final class Notification {
        private final String mTitle;
        private final String mBody;
        private final String mIcon;
        private final String mSound;
        private final String mTag;
        private final String mColor;
        private final String mClickAction;

        Notification(String title, String body, String icon, String sound, String tag,
                String color, String clickAction) {
            mTitle = title;
            mBody = body;
            mIcon = icon;
            mSound = sound;
            mTag = tag;
            mColor = color;
            mClickAction = clickAction;
        }

        public String getTitle() { return mTitle; }
        public String getBody() { return mBody; }
        public String getIcon() { return mIcon; }
        public String getSound() { return mSound; }
        public String getTag() { return mTag; }
        public String getColor() { return mColor; }
        public String getClickAction() { return mClickAction; }
    }

    public static final class Builder {
        private final Bundle mBundle = new Bundle();
        private final Map<String, String> mData = new HashMap<>();

        public Builder(String to) {
            mBundle.putString("google.to", to);
        }

        public Builder addData(String key, String value) {
            mData.put(key, value);
            return this;
        }

        public Builder setData(Map<String, String> data) {
            mData.clear();
            mData.putAll(data);
            return this;
        }

        public Builder setMessageId(String messageId) {
            mBundle.putString("google.message_id", messageId);
            return this;
        }

        public Builder setMessageType(String messageType) {
            mBundle.putString("message_type", messageType);
            return this;
        }

        public Builder setCollapseKey(String collapseKey) {
            mBundle.putString("collapse_key", collapseKey);
            return this;
        }

        public Builder setTtl(int ttl) {
            mBundle.putInt("google.ttl", ttl);
            return this;
        }

        public RemoteMessage build() {
            Bundle b = new Bundle(mBundle);
            for (Map.Entry<String, String> e : mData.entrySet()) {
                b.putString(e.getKey(), e.getValue());
            }
            return new RemoteMessage(b);
        }
    }
}
