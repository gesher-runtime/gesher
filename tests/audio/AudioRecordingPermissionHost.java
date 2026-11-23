/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import gesher.services.AudioRecordingPermissionTest;

/** Production Java permission store and the exact native admission body, with host JNI boundaries. */
public final class AudioRecordingPermissionHost {
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        AudioRecordingPermissionTest.run(true, false);
        System.out.println("Real AudioService/PackageManagerService/PermissionPolicy and MayRecordAudio; "
                + "host initialization/identity and framework catalog boundaries only. "
                + "No Windows capture, Parcel transport or AppOps silencing claim.");
    }
}
