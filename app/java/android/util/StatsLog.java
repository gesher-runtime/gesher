/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.util;

/**
 * Placeholder for the statsd module's StatsLog (see {@link StatsEvent}): there is no
 * statsd, so every event is dropped and the log calls answer false.
 */
public final class StatsLog {
    public static final byte ANNOTATION_ID_IS_UID = 1;
    public static final byte ANNOTATION_ID_TRUNCATE_TIMESTAMP = 2;
    public static final byte ANNOTATION_ID_PRIMARY_FIELD = 3;
    public static final byte ANNOTATION_ID_EXCLUSIVE_STATE = 4;
    public static final byte ANNOTATION_ID_PRIMARY_FIELD_FIRST_UID = 5;
    public static final byte ANNOTATION_ID_DEFAULT_STATE = 6;
    public static final byte ANNOTATION_ID_TRIGGER_STATE_RESET = 7;
    public static final byte ANNOTATION_ID_STATE_NESTED = 8;
    public static final byte ANNOTATION_ID_RESTRICTION_CATEGORY = 9;
    public static final byte ANNOTATION_ID_FIELD_RESTRICTION_PERIPHERAL_DEVICE_INFO = 10;
    public static final byte ANNOTATION_ID_FIELD_RESTRICTION_APP_USAGE = 11;
    public static final byte ANNOTATION_ID_FIELD_RESTRICTION_APP_ACTIVITY = 12;
    public static final byte ANNOTATION_ID_FIELD_RESTRICTION_HEALTH_CONNECT = 13;
    public static final byte ANNOTATION_ID_FIELD_RESTRICTION_ACCESSIBILITY = 14;
    public static final byte ANNOTATION_ID_FIELD_RESTRICTION_SYSTEM_SEARCH = 15;
    public static final byte ANNOTATION_ID_FIELD_RESTRICTION_USER_ENGAGEMENT = 16;
    public static final byte ANNOTATION_ID_FIELD_RESTRICTION_AMBIENT_SENSING = 17;
    public static final byte ANNOTATION_ID_FIELD_RESTRICTION_DEMOGRAPHIC_CLASSIFICATION = 18;
    public static final int RESTRICTION_CATEGORY_DIAGNOSTIC = 1;
    public static final int RESTRICTION_CATEGORY_SYSTEM_INTELLIGENCE = 2;
    public static final int RESTRICTION_CATEGORY_AUTHENTICATION = 3;
    public static final int RESTRICTION_CATEGORY_FRAUD_AND_ABUSE = 4;

    private StatsLog() {
    }

    public static boolean logEvent(int label) { return false; }
    public static boolean logStart(int label) { return false; }
    public static boolean logStop(int label) { return false; }

    public static boolean logBinaryPushStateChanged(String trainName, long trainVersionCode,
            int options, int state, long[] experimentIds) {
        return false;
    }

    public static void write(StatsEvent event) {
        event.release();
    }

    public static void writeRaw(byte[] buffer, int size) {
    }
}
