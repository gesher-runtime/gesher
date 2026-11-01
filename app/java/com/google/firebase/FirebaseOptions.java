/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.firebase;

public final class FirebaseOptions {
    private final String mApiKey;
    private final String mApplicationId;
    private final String mDatabaseUrl;
    private final String mGaTrackingId;
    private final String mGcmSenderId;
    private final String mStorageBucket;
    private final String mProjectId;

    private FirebaseOptions(String apiKey, String applicationId, String databaseUrl,
            String gaTrackingId, String gcmSenderId, String storageBucket, String projectId) {
        mApiKey = apiKey;
        mApplicationId = applicationId;
        mDatabaseUrl = databaseUrl;
        mGaTrackingId = gaTrackingId;
        mGcmSenderId = gcmSenderId;
        mStorageBucket = storageBucket;
        mProjectId = projectId;
    }

    public String getApiKey() {
        return mApiKey;
    }

    public String getApplicationId() {
        return mApplicationId;
    }

    public String getDatabaseUrl() {
        return mDatabaseUrl;
    }

    public String getGcmSenderId() {
        return mGcmSenderId;
    }

    public String getStorageBucket() {
        return mStorageBucket;
    }

    public String getProjectId() {
        return mProjectId;
    }

    public static final class Builder {
        private String mApiKey = "dummy_api_key_gesher";
        private String mApplicationId = "1:1234567890:android:abcdef";
        private String mDatabaseUrl;
        private String mGaTrackingId;
        private String mGcmSenderId = "1234567890";
        private String mStorageBucket;
        private String mProjectId = "gesher-project";

        public Builder() {
        }

        public Builder setApiKey(String apiKey) {
            mApiKey = apiKey;
            return this;
        }

        public Builder setApplicationId(String applicationId) {
            mApplicationId = applicationId;
            return this;
        }

        public Builder setDatabaseUrl(String databaseUrl) {
            mDatabaseUrl = databaseUrl;
            return this;
        }

        public Builder setGcmSenderId(String gcmSenderId) {
            mGcmSenderId = gcmSenderId;
            return this;
        }

        public Builder setStorageBucket(String storageBucket) {
            mStorageBucket = storageBucket;
            return this;
        }

        public Builder setProjectId(String projectId) {
            mProjectId = projectId;
            return this;
        }

        public FirebaseOptions build() {
            return new FirebaseOptions(mApiKey, mApplicationId, mDatabaseUrl, mGaTrackingId,
                    mGcmSenderId, mStorageBucket, mProjectId);
        }
    }
}
