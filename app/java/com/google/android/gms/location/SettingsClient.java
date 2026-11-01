/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

import android.content.Context;

import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;

public class SettingsClient {
    private final Context mContext;

    public SettingsClient(Context context) {
        mContext = context;
    }

    public Task<LocationSettingsResponse> checkLocationSettings(LocationSettingsRequest request) {
        return Tasks.forResult(new LocationSettingsResponse(new LocationSettingsStates(true, true, true, true, true, true)));
    }
}
