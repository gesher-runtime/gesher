/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.safetycenter;

import android.annotation.CallbackExecutor;
import android.annotation.IntDef;
import android.annotation.NonNull;
import android.annotation.Nullable;
import android.annotation.SystemApi;
import android.safetycenter.config.SafetyCenterConfig;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.util.List;
import java.util.concurrent.Executor;

/**
 * The SDK 36 {@code SafetyCenterManager} API (framework-permission-s), with its
 * constants and listener interface.
 *
 * <p>On Android this class is on every app's boot class path; apps that reference it
 * (the Play Store's package-change receiver does) link against it whether or not a
 * Safety Center service runs. Gesher runs no Safety Center service and registers no
 * service wrapper ({@link SafetyCenterFrameworkInitializer}), so
 * {@code Context.getSystemService(SafetyCenterManager.class)} and
 * {@code getSystemService(Context.SAFETY_CENTER_SERVICE)} return {@code null}, as
 * {@code SystemServiceRegistry} does on a device whose service is not published. No
 * instance is ever created: the constructor is private, and the instance methods
 * describe that absence rather than report a Safety Center state.
 */
@SystemApi
public final class SafetyCenterManager {

    public static final String ACTION_SAFETY_CENTER_ENABLED_CHANGED =
            "android.safetycenter.action.SAFETY_CENTER_ENABLED_CHANGED";
    public static final String ACTION_REFRESH_SAFETY_SOURCES =
            "android.safetycenter.action.REFRESH_SAFETY_SOURCES";
    public static final String EXTRA_REFRESH_SAFETY_SOURCE_IDS =
            "android.safetycenter.extra.REFRESH_SAFETY_SOURCE_IDS";
    public static final String EXTRA_REFRESH_SAFETY_SOURCES_REQUEST_TYPE =
            "android.safetycenter.extra.REFRESH_SAFETY_SOURCES_REQUEST_TYPE";
    public static final String EXTRA_REFRESH_SAFETY_SOURCES_BROADCAST_ID =
            "android.safetycenter.extra.REFRESH_SAFETY_SOURCES_BROADCAST_ID";
    public static final String EXTRA_SAFETY_SOURCE_ISSUE_ID =
            "android.safetycenter.extra.SAFETY_SOURCE_ISSUE_ID";
    public static final String EXTRA_SAFETY_SOURCE_ID =
            "android.safetycenter.extra.SAFETY_SOURCE_ID";
    public static final String EXTRA_SAFETY_SOURCE_USER_HANDLE =
            "android.safetycenter.extra.SAFETY_SOURCE_USER_HANDLE";
    public static final String EXTRA_SAFETY_SOURCES_GROUP_ID =
            "android.safetycenter.extra.SAFETY_SOURCES_GROUP_ID";

    public static final int EXTRA_REFRESH_REQUEST_TYPE_FETCH_FRESH_DATA = 0;
    public static final int EXTRA_REFRESH_REQUEST_TYPE_GET_DATA = 1;

    /** @hide */
    @IntDef(
            prefix = {"EXTRA_REFRESH_REQUEST_TYPE_"},
            value = {
                EXTRA_REFRESH_REQUEST_TYPE_FETCH_FRESH_DATA,
                EXTRA_REFRESH_REQUEST_TYPE_GET_DATA,
            })
    @Retention(RetentionPolicy.SOURCE)
    public @interface RefreshRequestType {}

    public static final int REFRESH_REASON_PAGE_OPEN = 100;
    public static final int REFRESH_REASON_RESCAN_BUTTON_CLICK = 200;
    public static final int REFRESH_REASON_DEVICE_REBOOT = 300;
    public static final int REFRESH_REASON_DEVICE_LOCALE_CHANGE = 400;
    public static final int REFRESH_REASON_SAFETY_CENTER_ENABLED = 500;
    public static final int REFRESH_REASON_OTHER = 600;
    public static final int REFRESH_REASON_PERIODIC = 700;

    /** @hide */
    @IntDef(
            prefix = {"REFRESH_REASON_"},
            value = {
                REFRESH_REASON_PAGE_OPEN,
                REFRESH_REASON_RESCAN_BUTTON_CLICK,
                REFRESH_REASON_DEVICE_REBOOT,
                REFRESH_REASON_DEVICE_LOCALE_CHANGE,
                REFRESH_REASON_SAFETY_CENTER_ENABLED,
                REFRESH_REASON_OTHER,
                REFRESH_REASON_PERIODIC
            })
    @Retention(RetentionPolicy.SOURCE)
    public @interface RefreshReason {}

    /** Listener for changes to {@link SafetyCenterData}. */
    public interface OnSafetyCenterDataChangedListener {
        void onSafetyCenterDataChanged(@NonNull SafetyCenterData data);

        default void onError(@NonNull SafetyCenterErrorDetails errorDetails) {}
    }

    private SafetyCenterManager() {}

    private static UnsupportedOperationException noService() {
        return new UnsupportedOperationException("Gesher runs no Safety Center service");
    }

    public boolean isSafetyCenterEnabled() {
        throw noService();
    }

    public void setSafetySourceData(
            @NonNull String safetySourceId,
            @Nullable SafetySourceData safetySourceData,
            @NonNull SafetyEvent safetyEvent) {
        throw noService();
    }

    @Nullable
    public SafetySourceData getSafetySourceData(@NonNull String safetySourceId) {
        throw noService();
    }

    public void reportSafetySourceError(
            @NonNull String safetySourceId,
            @NonNull SafetySourceErrorDetails safetySourceErrorDetails) {
        throw noService();
    }

    public void refreshSafetySources(@RefreshReason int refreshReason) {
        throw noService();
    }

    public void refreshSafetySources(
            @RefreshReason int refreshReason, @NonNull List<String> safetySourceIds) {
        throw noService();
    }

    @Nullable
    public SafetyCenterConfig getSafetyCenterConfig() {
        throw noService();
    }

    @NonNull
    public SafetyCenterData getSafetyCenterData() {
        throw noService();
    }

    public void addOnSafetyCenterDataChangedListener(
            @NonNull @CallbackExecutor Executor executor,
            @NonNull OnSafetyCenterDataChangedListener listener) {
        throw noService();
    }

    public void removeOnSafetyCenterDataChangedListener(
            @NonNull OnSafetyCenterDataChangedListener listener) {
        throw noService();
    }

    public void dismissSafetyCenterIssue(@NonNull String safetyCenterIssueId) {
        throw noService();
    }

    public void executeSafetyCenterIssueAction(
            @NonNull String safetyCenterIssueId,
            @NonNull String safetyCenterIssueActionId,
            int taskId) {
        throw noService();
    }

    public void executeSafetyCenterIssueAction(
            @NonNull String safetyCenterIssueId, @NonNull String safetyCenterIssueActionId) {
        throw noService();
    }

    public void clearAllSafetySourceDataForTests() {
        throw noService();
    }

    public void setSafetyCenterConfigForTests(@NonNull SafetyCenterConfig safetyCenterConfig) {
        throw noService();
    }

    public void clearSafetyCenterConfigForTests() {
        throw noService();
    }
}
