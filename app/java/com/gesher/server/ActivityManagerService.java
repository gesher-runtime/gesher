/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.ApplicationThreadConstants;
import android.app.ContentProviderHolder;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.app.Notification;
import android.content.ComponentName;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ProviderInfoList;
import android.content.pm.SystemFeaturesCache;
import android.content.pm.UserInfo;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.SystemClock;
import android.system.Os;
import android.util.ArrayMap;

import com.android.internal.os.ApplicationSharedMemory;
import com.gesher.app.DeviceProfile;

import java.io.FileDescriptor;
import java.io.IOException;
import java.util.List;

/**
 * {@code activity}: ActivityManagerService, lite. Answers what an app's lifecycle asks of
 * it, and attaches app processes ({@link #attachApplication}): the process is handed to
 * {@link ActivityTaskManagerService}, which binds it to a package and starts its
 * activities.
 */
public final class ActivityManagerService extends IActivityManager.Default {
    private final ActivityTaskManagerService mAtm;
    private final ActiveServices mServices;
    private final BroadcastController mBroadcasts;
    private final ContentProviders mProviders;
    private ApplicationSharedMemory mSharedMemory;  // created on the first bind
    private final PendingIntents mPendingIntents;

    ActivityManagerService(ActivityTaskManagerService atm, PackageManagerService pm,
            NotificationManagerService notifications) {
        mAtm = atm;
        mServices = new ActiveServices(atm, pm, notifications);
        mBroadcasts = new BroadcastController(atm, pm);
        mProviders = new ContentProviders(pm, atm);
        mPendingIntents = new PendingIntents(this);
        atm.setActivityManager(this);
    }

    /** An app process is gone: its services, receivers and providers too. */
    void processDied(ActivityTaskManagerService.ProcessRecord app) {
        mServices.processDied(app);
        mBroadcasts.processDied(app);
        // Providers it used: their processes may have no other client left.
        for (ActivityTaskManagerService.ProcessRecord p : mProviders.processDied(app)) {
            mAtm.maybeExit(p);
        }
    }

    /** The process is told to exit: its providers are not handed out any more. */
    void processExiting(ActivityTaskManagerService.ProcessRecord app) {
        mProviders.unpublish(app);
    }

    /**
     * Whether something keeps the process although it has no activity: a service of it,
     * or a provider of it that a process of another package uses.
     */
    boolean keepsAlive(ActivityTaskManagerService.ProcessRecord app) {
        return mServices.hasServices(app) || mProviders.hasClients(app);
    }

    @Override
    public List<android.app.ActivityManager.RunningAppProcessInfo> getRunningAppProcesses() {
        List<android.app.ActivityManager.RunningAppProcessInfo> result =
                new java.util.ArrayList<>();
        for (ActivityTaskManagerService.ProcessRecord p : mAtm.processes()) {
            if (p.packageName == null) continue;
            android.app.ActivityManager.RunningAppProcessInfo info =
                    new android.app.ActivityManager.RunningAppProcessInfo(p.packageName, p.pid,
                            new String[] {p.packageName});
            info.uid = android.os.Process.myUid();
            info.importance = !p.activities.isEmpty()
                    ? android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
                    : mServices.hasServices(p)
                            ? android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_SERVICE
                            : android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED;
            result.add(info);
        }
        return result;
    }

    // --- content providers (ContentProviders) -------------------------------------------

    @Override
    public ContentProviderHolder getContentProvider(IApplicationThread caller,
            String callingPackage, String name, int userId, boolean stable) {
        return mProviders.getContentProvider(mAtm.callerProcess(Binder.getCallingPid()),
                callingPackage, name);
    }

    @Override
    public ContentProviderHolder getContentProviderExternal(String name, int userId,
            IBinder token, String tag) {
        return mProviders.getContentProvider(null, tag, name);
    }

    @Override
    public void publishContentProviders(IApplicationThread caller,
            List<ContentProviderHolder> providers) {
        ActivityTaskManagerService.ProcessRecord app =
                mAtm.callerProcess(Binder.getCallingPid());
        if (app != null) mProviders.publishContentProviders(app, providers);
    }

    @Override
    public boolean refContentProvider(IBinder connection, int stable, int unstable) {
        return mProviders.refContentProvider(connection, stable, unstable);
    }

    @Override
    public void removeContentProvider(IBinder connection, boolean stable) {
    }

    // --- PendingIntents (PendingIntents) ---------------------------------------------------

    /** The PendingIntents (stored notifications make theirs again: NotificationStore). */
    PendingIntents pendingIntents() {
        return mPendingIntents;
    }

    @Override
    public IIntentSender getIntentSenderWithFeature(int type, String packageName,
            String featureId, IBinder token, String resultWho, int requestCode, Intent[] intents,
            String[] resolvedTypes, int flags, Bundle options, int userId) {
        return mPendingIntents.get(type, packageName, requestCode, intents, resolvedTypes,
                flags);
    }

    @Override
    public IIntentSender getIntentSender(int type, String packageName, IBinder token,
            String resultWho, int requestCode, Intent[] intents, String[] resolvedTypes,
            int flags, Bundle options, int userId) {
        return mPendingIntents.get(type, packageName, requestCode, intents, resolvedTypes,
                flags);
    }

    @Override
    public void cancelIntentSender(IIntentSender sender) {
        mPendingIntents.cancel(sender);
    }

    @Override
    public ActivityManager.PendingIntentInfo getInfoForIntentSender(IIntentSender sender) {
        PendingIntents.Record r = mPendingIntents.record(sender);
        if (r == null) return new ActivityManager.PendingIntentInfo(null, -1, false,
                ActivityManager.INTENT_SENDER_UNKNOWN);
        return new ActivityManager.PendingIntentInfo(r.packageName, r.uid,
                (r.flags & android.app.PendingIntent.FLAG_IMMUTABLE) != 0, r.type);
    }

    @Override
    public boolean registerIntentSenderCancelListenerEx(IIntentSender sender,
            com.android.internal.os.IResultReceiver receiver) {
        PendingIntents.Record r = mPendingIntents.record(sender);
        return r != null && !r.canceled;
    }

    @Override
    public Intent getIntentForIntentSender(IIntentSender sender) {
        PendingIntents.Record r = mPendingIntents.record(sender);
        return r != null ? new Intent(r.last()) : null;
    }

    @Override
    public boolean isIntentSenderTargetedToPackage(IIntentSender sender) {
        // As ActivityManagerService's: no intent names both a package and a component.
        PendingIntents.Record r = mPendingIntents.record(sender);
        if (r == null) return false;
        for (Intent intent : r.intents) {
            if (intent.getPackage() != null && intent.getComponent() != null) return false;
        }
        return true;
    }

    @Override
    public boolean isIntentSenderAnActivity(IIntentSender sender) {
        PendingIntents.Record r = mPendingIntents.record(sender);
        return r != null && r.type == ActivityManager.INTENT_SENDER_ACTIVITY;
    }

    @Override
    public int sendIntentSender(IApplicationThread caller, IIntentSender target,
            IBinder allowlistToken, int code, Intent intent, String resolvedType,
            IIntentReceiver finishedReceiver, String requiredPermission, Bundle options) {
        return mPendingIntents.send(mPendingIntents.record(target), intent, resolvedType);
    }

    int sendBroadcastFor(String packageName, Intent intent, String resolvedType) {
        return mBroadcasts.broadcastIntent(intent, resolvedType, packageName,
                android.os.Process.myPid());
    }

    int startActivityFor(String packageName, Intent intent, String resolvedType) {
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return mAtm.startActivity(null, packageName, null, intent, resolvedType, null, null, -1,
                0, null, null);
    }

    void startServiceFor(String packageName, Intent intent, String resolvedType,
            boolean foreground) {
        mServices.startService(null, intent, resolvedType, foreground, packageName);
    }

    // --- services (ActiveServices) -------------------------------------------------------

    @Override
    public ComponentName startService(IApplicationThread caller, Intent service,
            String resolvedType, boolean requireForeground, String callingPackage,
            String callingFeatureId, int userId) {
        return mServices.startService(caller, service, resolvedType, requireForeground,
                callingPackage);
    }

    @Override
    public int stopService(IApplicationThread caller, Intent service, String resolvedType,
            int userId) {
        return mServices.stopService(caller, service, resolvedType);
    }

    @Override
    public boolean stopServiceToken(ComponentName className, IBinder token, int startId) {
        return mServices.stopServiceToken(className, token, startId);
    }

    @Override
    public int bindService(IApplicationThread caller, IBinder token, Intent service,
            String resolvedType, IServiceConnection connection, long flags,
            String callingPackage, int userId) {
        return mServices.bindService(caller, service, resolvedType, connection, flags,
                callingPackage);
    }

    @Override
    public int bindServiceInstance(IApplicationThread caller, IBinder token, Intent service,
            String resolvedType, IServiceConnection connection, long flags,
            String instanceName, String callingPackage, int userId) {
        return mServices.bindService(caller, service, resolvedType, connection, flags,
                callingPackage);
    }

    @Override
    public void publishService(IBinder token, IBinder bindToken, IBinder service) {
        mServices.publishService(token, bindToken, service);
    }

    @Override
    public boolean unbindService(IServiceConnection connection) {
        return mServices.unbindService(connection);
    }

    @Override
    public void unbindFinished(IBinder token, IBinder bindToken) {
        mServices.unbindFinished(token, bindToken);
    }

    @Override
    public void serviceDoneExecuting(IBinder token, int type, int startId, int res) {
        mServices.serviceDoneExecuting(token, type, startId, res);
    }

    @Override
    public void setServiceForeground(ComponentName className, IBinder token, int id,
            Notification notification, int flags, int foregroundServiceType) {
        mServices.setServiceForeground(className, token, id, notification, flags,
                foregroundServiceType);
    }

    @Override
    public int getForegroundServiceType(ComponentName className, IBinder token) {
        return mServices.getForegroundServiceType(className, token);
    }

    @Override
    public IBinder peekService(Intent service, String resolvedType, String callingPackage) {
        return mServices.peekService(service, resolvedType);
    }

    @Override
    public void updateServiceGroup(IServiceConnection connection, int group, int importance) {
        // No process groups to put the service's process in.
    }

    // --- broadcasts (BroadcastController) ------------------------------------------------

    @Override
    public Intent registerReceiverWithFeature(IApplicationThread caller, String callerPackage,
            String callingFeatureId, String receiverId, IIntentReceiver receiver,
            IntentFilter filter, String requiredPermission, int userId, int flags) {
        return mBroadcasts.registerReceiver(caller, callerPackage, receiver, filter,
                Binder.getCallingPid());
    }

    @Override
    public Intent registerReceiver(IApplicationThread caller, String callerPackage,
            IIntentReceiver receiver, IntentFilter filter, String requiredPermission,
            int userId, int flags) {
        return mBroadcasts.registerReceiver(caller, callerPackage, receiver, filter,
                Binder.getCallingPid());
    }

    @Override
    public void unregisterReceiver(IIntentReceiver receiver) {
        mBroadcasts.unregisterReceiver(receiver);
    }

    @Override
    public int broadcastIntentWithFeature(IApplicationThread caller, String callingFeatureId,
            Intent intent, String resolvedType, IIntentReceiver resultTo, int resultCode,
            String resultData, Bundle map, String[] requiredPermissions,
            String[] excludePermissions, String[] excludePackages, int appOp, Bundle options,
            boolean serialized, boolean sticky, int userId) {
        ActivityTaskManagerService.ProcessRecord app =
                mAtm.callerProcess(Binder.getCallingPid());
        return mBroadcasts.broadcastIntent(intent, resolvedType,
                app != null ? app.packageName : null, Binder.getCallingPid());
    }

    @Override
    public int broadcastIntent(IApplicationThread caller, Intent intent, String resolvedType,
            IIntentReceiver resultTo, int resultCode, String resultData, Bundle map,
            String[] requiredPermissions, int appOp, Bundle options, boolean serialized,
            boolean sticky, int userId) {
        return broadcastIntentWithFeature(caller, null, intent, resolvedType, resultTo,
                resultCode, resultData, map, requiredPermissions, null, null, appOp, options,
                serialized, sticky, userId);
    }

    @Override
    public void finishReceiver(IBinder who, int resultCode, String resultData, Bundle map,
            boolean abortBroadcast, int flags) {
        mBroadcasts.finishReceiver(who, resultCode, resultData, map, abortBroadcast, flags);
        // A process started for the broadcast may have nothing else to do.
        mAtm.maybeExit(mAtm.callerProcess(Binder.getCallingPid()));
    }

    @Override
    public int checkPermission(String permission, int pid, int uid) {
        return PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public int checkPermissionForDevice(String permission, int pid, int uid, int deviceId) {
        return PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public UserInfo getCurrentUser() {
        return Services.UserManager.owner();
    }

    @Override
    public boolean handleApplicationWtf(android.os.IBinder app, String tag, boolean system,
            android.app.ApplicationErrorReport.ParcelableCrashInfo crashInfo,
            int immediateCallerPid) {
        SystemServer.log("wtf from pid " + Binder.getCallingPid() + ": " + tag + ": "
                + (crashInfo != null ? crashInfo.exceptionMessage : ""));
        return false;  // the process is not killed
    }

    @Override
    public void setRenderThread(int tid) {
        // No scheduling policies to apply.
    }

    @Override
    public void reportOptimizationInfo(android.os.IBinder app, String compilerFilter,
            String compilationReason) {
        // Nothing records dexopt state.
    }

    @Override
    public void attachApplication(IApplicationThread app, long startSeq) {
        mAtm.attachApplication(app, Binder.getCallingPid(), startSeq);
    }

    @Override
    public void finishAttachApplication(long startSeq, long timestampApplicationOnCreateNs) {
        mAtm.finishAttachApplication(Binder.getCallingPid());
    }

    /**
     * IApplicationThread.bindApplication, with what ActivityManagerService's
     * attachApplicationLocked sends: the configuration, the package's content providers
     * (installed by the process before Application.onCreate), no instrumentation,
     * no services map (the app asks servicemanager), no font map (Typeface loads the
     * preinstalled one) and the shared memory region with the system features.
     */
    void bindApplication(IApplicationThread thread, ApplicationInfo ai) throws Exception {
        FileDescriptor sharedMemory = sharedMemory().getReadOnlyFileDescriptor();
        Bundle coreSettings = new Bundle();
        // CoreSettingsObserver's bundle: the default device's settings, by device id.
        coreSettings.putBundle("0", new Bundle());
        thread.bindApplication(ai.processName != null ? ai.processName : ai.packageName, ai,
                null /* sdkSandboxClientAppVolumeUuid */, null /* sdkSandboxClientAppPackage */,
                false /* isSdkInSandbox */,
                ProviderInfoList.fromList(mProviders.providersFor(ai)),
                null /* instrumentationName */, null /* profilerInfo */,
                null /* instrumentationArgs */, null /* instrumentationWatcher */,
                null /* instrumentationUiConnection */, ApplicationThreadConstants.DEBUG_OFF,
                false /* enableBinderTracking */, false /* trackAllocation */,
                false /* isRestrictedBackupMode */, false /* persistent */,
                DeviceProfile.configuration(), CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                null /* services */, coreSettings, "unknown" /* buildSerial */,
                null /* autofillOptions */, null /* contentCaptureOptions */,
                CompatChanges.disabledFor(ai.targetSdkVersion), new long[0] /* loggableCompatChanges */,
                false /* logChangeChecksToStatsD */, null /* serializedSystemFontMap */,
                sharedMemory, SystemClock.elapsedRealtime(), SystemClock.uptimeMillis());
        if (!(thread.asBinder() instanceof Binder)) {
            Os.close(sharedMemory);  // the Parcel sent a duplicate
        }
    }

    /**
     * The ApplicationSharedMemory every app maps read-only, as SystemServer creates it:
     * here with no system features (the headless device declares none).
     */
    private synchronized ApplicationSharedMemory sharedMemory() throws IOException {
        if (mSharedMemory == null) {
            ApplicationSharedMemory m = ApplicationSharedMemory.create();
            // SystemServer writes PackageManagerService's features: the device's
            // (PackageManagerService.FEATURES).
            m.writeSystemFeaturesCache(new SystemFeaturesCache(
                    new ArrayMap<String, FeatureInfo>(PackageManagerService.FEATURES))
                    .getSdkFeatureVersions());
            // WindowManagerService keeps the animator duration scale there: 1.
            m.setCurrentAnimatorScale(1f);
            mSharedMemory = m;
        }
        return mSharedMemory;
    }
}
