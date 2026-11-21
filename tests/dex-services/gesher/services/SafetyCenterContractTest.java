/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.os.Parcel;
import android.os.Parcelable;
import android.safetycenter.SafetyCenterManager;
import android.safetycenter.SafetyEvent;
import android.safetycenter.SafetySourceData;
import android.safetycenter.SafetySourceErrorDetails;
import android.safetycenter.SafetySourceStatus;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.List;

/**
 * The android.safetycenter classes (framework-permission-s) on the boot class path,
 * without a Safety Center service: the Play Store's package-change receiver resolves
 * SafetyCenterManager and builds SafetySourceData, SafetySourceStatus and SafetyEvent
 * values. Before they existed every PACKAGE_ADDED broadcast killed its process with
 * NoClassDefFoundError.
 */
public final class SafetyCenterContractTest {
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    private static <T extends Parcelable> T roundTrip(T value, Parcelable.Creator<T> creator) {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            T copy = creator.createFromParcel(parcel);
            require(parcel.dataAvail() == 0, value.getClass().getSimpleName() + " left unread Parcel data");
            return copy;
        } finally {
            parcel.recycle();
        }
    }

    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("safetycenter-test", root + "/build.prop", root + "/framework/framework-res.apk");
        com.gesher.server.Services.installInProcess(com.gesher.server.Services.createInProcess(List.of()));
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Method create = Class.forName("android.app.ContextImpl").getDeclaredMethod("createSystemContext", threadType);
        create.setAccessible(true);
        Context context = (Context) create.invoke(null, constructor.newInstance());

        for (Class<?> type : new Class<?>[] {SafetyCenterManager.class, SafetySourceData.class,
                SafetySourceStatus.class, SafetyEvent.class, SafetySourceErrorDetails.class}) {
            require(type.getClassLoader() == Object.class.getClassLoader(),
                    type.getName() + " is not on the boot class path");
        }

        // No Safety Center service runs: the manager is absent, not a fabricated one.
        require(context.getSystemService(SafetyCenterManager.class) == null,
                "SafetyCenterManager reported a Safety Center service that does not exist");
        require(context.getSystemService(Context.SAFETY_CENTER_SERVICE) == null,
                "safety_center reported a Safety Center service that does not exist");
        System.out.println("SafetyCenterContractTest: no Safety Center service, getSystemService returns null");

        SafetyEvent changed = new SafetyEvent.Builder(SafetyEvent.SAFETY_EVENT_TYPE_SOURCE_STATE_CHANGED).build();
        require(roundTrip(changed, SafetyEvent.CREATOR).equals(changed), "SafetyEvent Parcel round trip");
        boolean rejected = false;
        try {
            new SafetyEvent.Builder(SafetyEvent.SAFETY_EVENT_TYPE_RESOLVING_ACTION_SUCCEEDED).build();
        } catch (IllegalArgumentException expected) {
            rejected = true;
        }
        require(rejected, "A resolving-action SafetyEvent without its issue was accepted");

        SafetySourceStatus status = new SafetySourceStatus.Builder(
                "Play Protect", "No harmful apps found", SafetySourceData.SEVERITY_LEVEL_INFORMATION)
                .setEnabled(true).build();
        SafetySourceData data = new SafetySourceData.Builder().setStatus(status).build();
        SafetySourceData copy = roundTrip(data, SafetySourceData.CREATOR);
        require(copy.equals(data) && copy.getStatus().equals(status), "SafetySourceData Parcel round trip");
        require(copy.getIssues().isEmpty(), "SafetySourceData invented issues");

        SafetySourceErrorDetails error = new SafetySourceErrorDetails(changed);
        require(roundTrip(error, SafetySourceErrorDetails.CREATOR).equals(error),
                "SafetySourceErrorDetails Parcel round trip");

        System.out.println("SafetyCenterContractTest: values build, validate and round-trip through Parcel");
        System.out.println("SafetyCenterContractTest all passed: " + checks + " checks");
    }
}
