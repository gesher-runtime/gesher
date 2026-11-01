/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class LocationSettingsRequest {
    private final List<LocationRequest> mRequests;
    private final boolean mAlwaysShow;
    private final boolean mNeedBle;

    private LocationSettingsRequest(List<LocationRequest> requests, boolean alwaysShow, boolean needBle) {
        mRequests = requests;
        mAlwaysShow = alwaysShow;
        mNeedBle = needBle;
    }

    public static final class Builder {
        private final List<LocationRequest> mRequests = new ArrayList<>();
        private boolean mAlwaysShow;
        private boolean mNeedBle;

        public Builder addLocationRequest(LocationRequest request) {
            if (request != null) {
                mRequests.add(request);
            }
            return this;
        }

        public Builder setAlwaysShow(boolean show) {
            mAlwaysShow = show;
            return this;
        }

        public Builder setNeedBle(boolean needBle) {
            mNeedBle = needBle;
            return this;
        }

        public LocationSettingsRequest build() {
            return new LocationSettingsRequest(mRequests, mAlwaysShow, mNeedBle);
        }
    }
}
