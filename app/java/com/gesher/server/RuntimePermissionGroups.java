/*
 * Copyright (C) 2022 The Android Open Source Project
 * Copyright (C) 2026 The Gesher Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.gesher.server;

import android.Manifest;
import android.content.pm.PermissionInfo;
import android.permission.flags.Flags;
import java.util.HashSet;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import static android.Manifest.permission.*;

/**
 * Android 16 PermissionController's logical groups, separate from the framework
 * manifest's raw UNDEFINED group. Adapted from android-16.0.0_r4 PermissionMapping.kt;
 * see docs/verification/issue56-f09-runtime-permission-state.md for the source.
 */
final class RuntimePermissionGroups {
    private RuntimePermissionGroups() { }

    static String group(PermissionInfo permission) {
        String platform = "android".equals(permission.packageName) ? platformGroup(permission.name) : null;
        if (platform != null) return platform;
        // UNDEFINED is a placeholder, not a shared user-decision group. Custom
        // permissions retain their declared group (and their defining owner).
        String declared = permission.group;
        return declared == null || "android.permission-group.UNDEFINED".equals(declared) ? null : declared;
    }

    /** Android classifies background grants by their foreground permission's definition. */
    static Set<String> backgroundPermissions(Map<String, PermissionInfo> definitions) {
        Set<String> background = new HashSet<>();
        for (PermissionInfo foreground : definitions.values()) {
            if (!foreground.isRuntime() || foreground.backgroundPermission == null) continue;
            PermissionInfo other = definitions.get(foreground.backgroundPermission);
            if (other != null && other.isRuntime()
                    && Objects.equals(foreground.packageName, other.packageName)) {
                background.add(other.name);
            }
        }
        return background;
    }

    private static String platformGroup(String permission) {
        return switch (permission) {
            case READ_CONTACTS, WRITE_CONTACTS, GET_ACCOUNTS -> Manifest.permission_group.CONTACTS;
            case READ_CALENDAR, WRITE_CALENDAR -> Manifest.permission_group.CALENDAR;
            case SEND_SMS, RECEIVE_SMS, READ_SMS, RECEIVE_MMS, RECEIVE_WAP_PUSH,
                    READ_CELL_BROADCASTS -> Manifest.permission_group.SMS;
            case READ_EXTERNAL_STORAGE, WRITE_EXTERNAL_STORAGE -> Manifest.permission_group.STORAGE;
            case READ_MEDIA_AUDIO -> Manifest.permission_group.READ_MEDIA_AURAL;
            case READ_MEDIA_IMAGES, READ_MEDIA_VIDEO, ACCESS_MEDIA_LOCATION,
                    READ_MEDIA_VISUAL_USER_SELECTED -> Manifest.permission_group.READ_MEDIA_VISUAL;
            case ACCESS_FINE_LOCATION, ACCESS_COARSE_LOCATION,
                    ACCESS_BACKGROUND_LOCATION -> Manifest.permission_group.LOCATION;
            case BLUETOOTH_ADVERTISE, BLUETOOTH_CONNECT, BLUETOOTH_SCAN,
                    UWB_RANGING, NEARBY_WIFI_DEVICES -> Manifest.permission_group.NEARBY_DEVICES;
            case RANGING -> Flags.rangingPermissionEnabled() ? Manifest.permission_group.NEARBY_DEVICES : null;
            case ACCESS_LOCAL_NETWORK -> Flags.accessLocalNetworkPermissionEnabled()
                    ? Manifest.permission_group.NEARBY_DEVICES : null;
            case READ_CALL_LOG, WRITE_CALL_LOG, PROCESS_OUTGOING_CALLS -> Manifest.permission_group.CALL_LOG;
            case READ_PHONE_STATE, READ_PHONE_NUMBERS, CALL_PHONE, ADD_VOICEMAIL,
                    USE_SIP, ANSWER_PHONE_CALLS, ACCEPT_HANDOVER -> Manifest.permission_group.PHONE;
            case RECORD_AUDIO, RECORD_BACKGROUND_AUDIO -> Manifest.permission_group.MICROPHONE;
            case ACTIVITY_RECOGNITION -> Manifest.permission_group.ACTIVITY_RECOGNITION;
            case CAMERA, BACKGROUND_CAMERA -> Manifest.permission_group.CAMERA;
            case POST_NOTIFICATIONS -> Manifest.permission_group.NOTIFICATIONS;
            case EYE_TRACKING_COARSE, FACE_TRACKING, HAND_TRACKING, SCENE_UNDERSTANDING_COARSE ->
                    android.xr.Flags.xrManifestEntries() ? Manifest.permission_group.XR_TRACKING : null;
            case EYE_TRACKING_FINE -> android.xr.Flags.xrManifestEntries()
                    ? Manifest.permission_group.XR_EYE_SENSITIVE : null;
            case HEAD_TRACKING, SCENE_UNDERSTANDING_FINE -> android.xr.Flags.xrManifestEntries()
                    ? Manifest.permission_group.XR_TRACKING_SENSITIVE : null;
            default -> null;
        };
    }
}
