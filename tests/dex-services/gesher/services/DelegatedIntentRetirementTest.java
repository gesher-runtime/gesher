/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityManager;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.app.PendingIntent;
import android.app.IActivityTaskManager;
import android.app.ResultInfo;
import android.app.servertransaction.ActivityResultItem;
import android.app.servertransaction.ClientTransaction;
import android.content.IIntentSender;
import android.content.ComponentName;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.InvocationTargetException;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** Server-local creator retirement races; both ordinary APKs are metadata only. */
public final class DelegatedIntentRetirementTest {
    private static int checks;
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field value = type.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static Method method(Class<?> type, String name, Class<?>... args) throws Exception {
        Method value = type.getDeclaredMethod(name, args); value.setAccessible(true); return value;
    }
    private static Object make(Class<?> type, Class<?>[] args, Object... values) throws Exception {
        Constructor<?> value = type.getDeclaredConstructor(args); value.setAccessible(true);
        return value.newInstance(values);
    }
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        int creates, starts, binds, receivers, exits;
        Runnable onCreate;
        final java.util.List<ClientTransaction> transactions = new java.util.ArrayList<>();
        @Override public void scheduleTransaction(ClientTransaction transaction) { transactions.add(transaction); }
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compat, int state) {
            creates++; if (onCreate != null) onCreate.run();
        }
        @Override public void scheduleServiceArgs(IBinder token, ParceledListSlice args) { starts++; }
        @Override public void scheduleBindService(IBinder token, IBinder binding, Intent intent,
                boolean rebind, int state, long sequence) { binds++; }
        @Override public void scheduleReceiver(Intent intent, ActivityInfo info,
                CompatibilityInfo compat, int code, String data, Bundle extras, boolean ordered,
                boolean assumed, int user, int state, int uid, String sender) { receivers++; }
        @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                int code, String data, Bundle extras, boolean ordered, boolean sticky,
                boolean assumed, int user, int state, int uid, String sender) throws RemoteException {
            receiver.performReceive(intent, code, data, extras, ordered, sticky, user);
        }
        @Override public void scheduleExit() { exits++; }
    }
    private static final class Harness {
        final String creatorPath;
        final ApkPackage creator, target;
        final PackageManagerService pm;
        final Class<?> atmType, amType, processType, pendingType, servicesType, broadcastsType;
        final Object atm, am, process, pending, services, broadcasts;
        final AppThread thread = new AppThread();
        final ServiceInfo service;
        final ActivityInfo receiver;
        Harness(String creatorPath, String targetPath, boolean receiverProcess, boolean ready) throws Exception {
            this.creatorPath = creatorPath;
            creator = ApkPackage.parse(creatorPath); target = ApkPackage.parse(targetPath);
            require(!creator.packageName.equals(target.packageName), "Independent target is required");
            pm = new PackageManagerService(Arrays.asList(creator, target));
            atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
            amType = Class.forName("com.gesher.server.ActivityManagerService");
            Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
            atm = make(atmType, new Class<?>[]{PackageManagerService.class}, pm);
            am = make(amType, new Class<?>[]{atmType, PackageManagerService.class, notifications},
                    atm, pm, make(notifications, new Class<?>[0]));
            pending = method(amType, "pendingIntents").invoke(am); pendingType = pending.getClass();
            services = field(amType, "mServices").get(am); servicesType = services.getClass();
            broadcasts = field(amType, "mBroadcasts").get(am); broadcastsType = broadcasts.getClass();
            PackageInfo info = pm.getPackageInfo(target.packageName,
                    PackageManager.GET_SERVICES | PackageManager.GET_RECEIVERS, 0);
            service = info.services[0]; receiver = info.receivers[0];
            require(service.exported && receiver.exported, "Use ordinary orderedbroadcast target fixture");
            processType = Class.forName(atmType.getName() + "$ProcessRecord");
            process = make(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class},
                    atm, 910001, thread);
            field(processType, "packageName").set(process, target.packageName);
            String declared = receiverProcess ? receiver.processName : service.processName;
            String processName = (String) method(atmType, "componentProcess", String.class, String.class)
                    .invoke(atm, target.packageName, declared);
            field(processType, "processName").set(process, processName);
            field(processType, "ready").setBoolean(process, ready);
            @SuppressWarnings("unchecked") List<Object> processes = (List<Object>) field(atmType, "mProcesses").get(atm);
            processes.add(process);
        }
        Intent serviceIntent() { return new Intent().setComponent(new ComponentName(target.packageName, service.name)); }
        Intent receiverIntent() { return new Intent("com.gesher.orderedbroadcast.EVENT")
                .setComponent(new ComponentName(target.packageName, receiver.name)); }
        Object token(int type, Intent intent, int flags) throws Exception {
            return method(pendingType, "get", int.class, String.class, int.class,
                    Intent[].class, String[].class, int.class).invoke(pending, type, creator.packageName,
                            1, new Intent[]{intent}, null, flags);
        }
        int send(Object token) throws Exception {
            return (Integer) method(pendingType, "send", token.getClass(), Intent.class, String.class, String.class)
                    .invoke(pending, token, null, null, null);
        }
        void retire() {
            try {
                require(!Thread.holdsLock(field(pendingType, "mRecords").get(pending)) && !Thread.holdsLock(services)
                        && !Thread.holdsLock(broadcasts) && !Thread.holdsLock(pm), "App callback holds a service lock");
                ApkPackage original = pm.get(creator.packageName);
                method(PackageManagerService.class, "beginPackageRemoval", ApkPackage.class).invoke(pm, original);
                method(PackageManagerService.class, "revokePackageCapabilities", String.class).invoke(pm, creator.packageName);
                method(PackageManagerService.class, "remove", String.class).invoke(pm, creator.packageName);
                method(PackageManagerService.class, "add", ApkPackage.class).invoke(pm, ApkPackage.parse(creatorPath));
                method(PackageManagerService.class, "endPackageRemoval", ApkPackage.class).invoke(pm, original);
                require(pm.get(target.packageName) == target, "Race replaced the independent target");
            } catch (Exception failure) { throw new AssertionError(failure); }
        }
        @SuppressWarnings("unchecked") void resumeQueued() throws Exception {
            List<Object> queue = (List<Object>) field(atmType, "mPendingWork").get(atm);
            require(queue.size() == 1, "Expected one cold-process callback");
            Object queued = queue.remove(0);
            field(processType, "ready").setBoolean(process, true);
            ((Consumer<Object>) field(queued.getClass(), "work").get(queued)).accept(process);
        }
    }

    @SuppressWarnings("unchecked")
    private static Object caller(Harness h, AppThread client, int pid, Object replaced) throws Exception {
        Object app = make(h.processType, new Class<?>[]{h.atmType, int.class, IApplicationThread.class},
                h.atm, pid, client);
        field(h.processType, "packageName").set(app, h.creator.packageName);
        field(h.processType, "processName").set(app, h.creator.packageName);
        field(h.processType, "ready").setBoolean(app, true);
        require(field(h.processType, "uid").getInt(app) == h.creator.uid(), "Direct caller UID was not preserved");
        List<Object> processes = (List<Object>) field(h.atmType, "mProcesses").get(h.atm);
        synchronized (h.atm) {
            if (replaced != null) processes.remove(replaced);
            processes.add(app);
        }
        return app;
    }

    /** Native direct callers: strict acceptance, then package-owned accepted work. */
    private static void directCapture(String[] args, int route, int race) throws Exception {
        Harness h = new Harness(args[0], args[1], route == 2, race < 3);
        int pid = 920001 + route, uid = h.creator.uid();
        AppThread client = new AppThread();
        long original = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) uid << 32) | (pid & 0xffffffffL));
            require(Binder.getCallingPid() == pid && Binder.getCallingUid() == uid,
                    "Native direct-app Binder identity unavailable");
            Object originalProcess = caller(h, client, pid, null);
            Runnable replacement = () -> {
                try {
                    require(Binder.getCallingPid() == pid && Binder.getCallingUid() == uid,
                            "Paused authentication changed transport identity");
                    if (race == 1 || race == 4) {
                        h.retire();
                        require(h.pm.get(h.creator.packageName) != h.creator
                                && h.pm.get(h.creator.packageName).uid() == uid,
                                "Code replacement did not preserve the ordinary caller UID");
                        require(method(h.atmType, "callerProcess", int.class).invoke(h.atm, pid) == originalProcess,
                                "Code race also replaced the process fixture");
                    } else if (race == 2) {
                        caller(h, client, pid, originalProcess);
                        require(h.pm.get(h.creator.packageName) == h.creator,
                                "PID reuse race also replaced the code fixture");
                    } else {
                        // Exercise the real idle-exit and death cleanup path. Accepted
                        // starts/broadcasts survive; a dead client's bindings do not.
                        method(h.atmType, "maybeExit", h.processType).invoke(h.atm, originalProcess);
                        require(client.exits == 1 && field(h.processType, "exiting").getBoolean(originalProcess),
                                "Accepted caller was not allowed its ordinary idle exit");
                        method(h.atmType, "processDied", h.processType).invoke(h.atm, originalProcess);
                        require(!((List<?>) field(h.atmType, "mProcesses").get(h.atm)).contains(originalProcess),
                                "Actual caller death did not remove the original process");
                        require(h.pm.get(h.creator.packageName) == h.creator,
                                "Normal caller exit changed package authority");
                        if (race == 3) caller(h, new AppThread(), pid, null);
                    }
                } catch (Exception failure) { throw new AssertionError(failure); }
            };
            Object controller = route == 2 ? h.broadcasts : h.services;
            Class<?> type = controller.getClass();
            if (race == 1 || race == 2) field(type, "mBeforeSenderAuthentication").set(controller, replacement);
            boolean denied = false;
            try {
                if (route == 0) {
                    method(type, "startService", IApplicationThread.class, Intent.class, String.class,
                            boolean.class, String.class).invoke(controller, client, h.serviceIntent(),
                                    null, false, h.creator.packageName);
                } else if (route == 1) {
                    IServiceConnection connection = new IServiceConnection.Default() {
                        private final IBinder binder = new Binder();
                        @Override public IBinder asBinder() { return binder; }
                    };
                    method(type, "bindService", IApplicationThread.class, Intent.class, String.class,
                            IServiceConnection.class, long.class, String.class).invoke(controller,
                                    client, h.serviceIntent(), null, connection,
                                    (long) Context.BIND_AUTO_CREATE, h.creator.packageName);
                } else {
                    method(type, "broadcastIntent", IApplicationThread.class, Intent.class, String.class,
                            String.class, int.class, IIntentReceiver.class, int.class, String.class,
                            Bundle.class, String[].class, String[].class, String[].class, boolean.class)
                            .invoke(controller, client, h.receiverIntent(), null, h.creator.packageName,
                                    pid, null, 0, null, null, null, null, null, false);
                }
            } catch (InvocationTargetException failure) {
                if (!(failure.getCause() instanceof SecurityException)) throw failure;
                denied = true;
            }
            if (race >= 3) {
                require(!denied, "Live direct call was rejected before its queued wait");
                replacement.run();
                h.resumeQueued();
            }
            int callbacks = h.thread.creates + h.thread.starts + h.thread.binds + h.thread.receivers;
            if (race == 0) require(!denied && callbacks > 0, "Live ordinary direct sender was rejected");
            else if ((race == 3 || race == 5) && route != 1) {
                require(callbacks > 0, "Accepted direct route " + route + " was revoked by normal caller exit/PID reuse");
            } else require(callbacks == 0, "Direct route " + route + " adopted retired code/client authority, race=" + race);
            if ((race == 3 || race == 5) && route == 1) {
                require(((java.util.Map<?, ?>) field(h.servicesType, "mConnections").get(h.services)).isEmpty(),
                        "Actual caller death retained its binding");
            }
            if (race == 1 || race == 2) require(denied, "Retired direct sender passed authentication");
        } finally { Binder.restoreCallingIdentity(original); }
    }

    @SuppressWarnings("unchecked")
    private static void intentSenderActivity(String[] args, int mode) throws Exception {
        Harness h = new Harness(args[0], args[1], false, true);
        int pid = 930001, uid = h.target.uid();
        AppThread client = new AppThread();
        long original = Binder.clearCallingIdentity();
        try {
            // The capability's creator and its recipient are independent apps.
            Object parsedActivity = h.creator.parsed.getActivities().get(0);
            parsedActivity.getClass().getMethod("setExported", boolean.class).invoke(parsedActivity, false);
            ActivityInfo destination = h.creator.launcherActivity(null);
            require(destination != null && !destination.exported, "Use an actual non-exported capability target");
            Object pending = method(h.pendingType, "get", int.class, String.class, int.class,
                    Intent[].class, String[].class, int.class).invoke(h.pending,
                    mode == 3 ? ActivityManager.INTENT_SENDER_BROADCAST : ActivityManager.INTENT_SENDER_ACTIVITY,
                    h.creator.packageName, 17, new Intent[]{new Intent().setComponent(
                            new ComponentName(h.creator.packageName, destination.name))}, null,
                    PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_ONE_SHOT);
            Binder.restoreCallingIdentity(((long) uid << 32) | pid);
            Object app = make(h.processType, new Class<?>[]{h.atmType, int.class, IApplicationThread.class},
                    h.atm, pid, client);
            field(h.processType, "packageName").set(app, h.target.packageName);
            field(h.processType, "processName").set(app, h.target.packageName);
            field(h.processType, "ready").setBoolean(app, true);
            field(h.processType, "windowMode").set(app, "hidden");
            ((List<Object>) field(h.atmType, "mProcesses").get(h.atm)).add(app);
            // The source record is a planted fixture; only the delegated destination
            // is resolved from a manifest. No application code is executed.
            ActivityInfo sourceInfo = new ActivityInfo(destination);
            sourceInfo.packageName = h.target.packageName;
            sourceInfo.name = h.target.packageName + ".FixtureSource";
            sourceInfo.applicationInfo = h.target.applicationInfo(0);
            field(h.processType, "packageName").set(h.process, h.creator.packageName);
            field(h.processType, "processName").set(h.process, h.creator.packageName);
            Class<?> activityType = Class.forName(h.atmType.getName() + "$ActivityRecord");
            Object source = make(activityType, new Class<?>[]{h.atmType, ActivityInfo.class, Intent.class,
                    int.class, int.class, String.class, activityType, String.class, int.class},
                    h.atm, sourceInfo, new Intent().setComponent(new ComponentName(h.target.packageName,
                            sourceInfo.name)), 41, pid, h.target.packageName, null, null, -1);
            field(activityType, "app").set(source, app);
            Class<?> stateType = field(activityType, "state").getType();
            field(activityType, "state").set(source, Enum.valueOf((Class) stateType, "RESUMED"));
            List<Object> activities = (List<Object>) field(h.atmType, "mActivities").get(h.atm);
            activities.add(source);
            ((List<Object>) field(h.processType, "activities").get(app)).add(source);
            IBinder token = (IBinder) field(activityType, "token").get(source);
            IActivityTaskManager api = (IActivityTaskManager) h.atm;
            if (mode == 4) field(h.atmType, "mBeforeActivityPublication").set(h.atm, (Runnable) () -> {
                try { method(PackageManagerService.class, "add", ApkPackage.class)
                        .invoke(h.pm, ApkPackage.parse(args[1])); }
                catch (Exception failure) { throw new AssertionError(failure); }
            });
            try {
                int result = api.startActivityIntentSender(mode == 1 ? new AppThread() : client,
                        (IIntentSender) pending, null, new Intent("fixture.cannot.replace.immutable"), null,
                        mode == 2 ? new Binder() : token, "fixture-result", 73,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION, Intent.FLAG_GRANT_READ_URI_PERMISSION, null);
                require(mode != 1 && mode != 2, "Forged IntentSender caller/result token was accepted");
                if (mode >= 3) {
                    require(result == ActivityManager.START_CANCELED && activities.size() == 1,
                            "Wrong capability type or retired caller published an activity");
                    return;
                }
                require(result >= ActivityManager.START_SUCCESS && activities.size() == 2,
                        "IntentSender reported success without launching its activity");
                Object launched = activities.get(1);
                require("hidden".equals(field(activityType, "inheritedWindowMode").get(launched))
                        && h.creator.packageName.equals(field(activityType, "launchedFromPackage").get(launched)),
                        "Delegated activity lost recipient window mode or creator authority");
                require(field(activityType, "resultTo").get(launched) == source
                        && field(activityType, "requestCode").getInt(launched) == 73
                        && "fixture-result".equals(field(activityType, "resultWho").get(launched)),
                        "IntentSender lost its authenticated result recipient");
                Intent launchedIntent = (Intent) field(activityType, "intent").get(launched);
                require(launchedIntent.getAction() == null
                        && (launchedIntent.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0,
                        "Immutable fill-in or URI-grant authority was altered by the recipient");
                require(api.startActivityIntentSender(client, (IIntentSender) pending, null,
                        null, null, token, null, 74, 0, 0, null) == ActivityManager.START_CANCELED,
                        "One-shot activity capability was reusable");
                api.getActivityClientController().activityPaused(token);
                IBinder launchedToken = (IBinder) field(activityType, "token").get(launched);
                api.getActivityClientController().finishActivity(launchedToken, android.app.Activity.RESULT_OK,
                        new Intent().putExtra("fixture-result", true), 0);
                api.getActivityClientController().activityPaused(launchedToken);
                boolean delivered = false;
                for (ClientTransaction transaction : client.transactions) {
                    for (Object item : transaction.getTransactionItems()) {
                        if (!(item instanceof ActivityResultItem)) continue;
                        for (ResultInfo resultInfo : (List<ResultInfo>) field(ActivityResultItem.class,
                                "mResultInfoList").get(item)) {
                            delivered |= resultInfo.mRequestCode == 73
                                    && resultInfo.mResultCode == android.app.Activity.RESULT_OK
                                    && resultInfo.mData.getBooleanExtra("fixture-result", false);
                        }
                    }
                }
                require(delivered, "Finishing the capability activity did not deliver its actual result");
            } catch (SecurityException denied) {
                require(mode == 1 || mode == 2, "Valid delegated activity was denied: " + denied);
                require(activities.size() == 1, "Rejected caller published an activity");
            }
        } finally { Binder.restoreCallingIdentity(original); }
    }

    private static void credentialResultFormat() throws Exception {
        Method normalize = method(Class.forName("com.gesher.server.ActivityTaskManagerService"),
                "normalizeMicrogCredentialResult", String.class, String.class, String.class, int.class, Intent.class);
        String provider = "org.microg.gms.auth.credentials.identity.IdentityCredentialChooserActivity";
        String recipient = "androidx.credentials.playservices.controllers.identityauth.HiddenActivity";
        String key = "android.service.credentials.extra.GET_CREDENTIAL_RESPONSE";
        Bundle content = new Bundle(); content.putString("fixture-value", "opaque synthetic provider data");
        Bundle legacy = new Bundle();
        legacy.putString("androidx.credentials.provider.extra.EXTRA_CREDENTIAL_TYPE", "fixture.credential.type");
        legacy.putBundle("androidx.credentials.provider.extra.EXTRA_CREDENTIAL_DATA", content);
        Intent original = new Intent().putExtra(key, legacy).putExtra("unrelated", 17);
        Intent result = (Intent) normalize.invoke(null, "com.google.android.gms", provider, recipient,
                android.app.Activity.RESULT_OK, original);
        android.credentials.GetCredentialResponse response = result.getParcelableExtra(key,
                android.credentials.GetCredentialResponse.class);
        require(response != null && "fixture.credential.type".equals(response.getCredential().getType())
                && "opaque synthetic provider data".equals(response.getCredential().getData().getString("fixture-value")),
                "Credential transport changed or invented the provider's opaque data");
        require(result != original && original.getExtras().get(key) instanceof Bundle
                && result.getIntExtra("unrelated", -1) == 17, "Credential normalization mutated the source or unrelated extras");
        require(normalize.invoke(null, "com.example.other", provider, recipient, -1, original) == original,
                "Unrelated provider received credential conversion");
        require(normalize.invoke(null, "com.google.android.gms", "other.Activity", recipient, -1, original) == original,
                "Unrelated activity received credential conversion");
        require(normalize.invoke(null, "com.google.android.gms", provider, "other.Activity", -1, original) == original,
                "Unrelated recipient received credential conversion");
        require(normalize.invoke(null, "com.google.android.gms", provider, recipient, 0, original) == original,
                "Canceled result received credential conversion");
        require(normalize.invoke(null, "com.google.android.gms", provider, recipient, -1, result) == result,
                "Already typed response was converted again");
        Intent malformed = new Intent().putExtra(key, new Bundle());
        require(normalize.invoke(null, "com.google.android.gms", provider, recipient, -1, malformed) == malformed,
                "Incomplete provider response acquired invented credential fields");
        String pictureKey = "com.google.android.libraries.identity.googleid.BUNDLE_KEY_PROFILE_PICTURE_URI";
        String picture = "https://example.invalid/synthetic-profile-picture";
        content.putString(pictureKey, picture);
        legacy.putString("androidx.credentials.provider.extra.EXTRA_CREDENTIAL_TYPE",
                "com.google.android.libraries.identity.googleid.TYPE_GOOGLE_ID_TOKEN_CREDENTIAL");
        Intent google = (Intent) normalize.invoke(null, "com.google.android.gms", provider, recipient, -1, original);
        android.os.Parcel wire = android.os.Parcel.obtain();
        try {
            google.writeToParcel(wire, 0); wire.setDataPosition(0);
            Intent decoded = Intent.CREATOR.createFromParcel(wire);
            Bundle decodedData = decoded.getParcelableExtra(key, android.credentials.GetCredentialResponse.class)
                    .getCredential().getData();
            require(android.net.Uri.parse(picture).equals(decodedData.getParcelable(pictureKey, android.net.Uri.class))
                    && "opaque synthetic provider data".equals(decodedData.getString("fixture-value")),
                    "Google credential Uri or opaque fields did not survive actual parcel transport");
            require(content.get(pictureKey) instanceof String && decoded.getIntExtra("unrelated", -1) == 17,
                    "Picture normalization mutated the source or unrelated intent data");
        } finally { wire.recycle(); }
        content.putParcelable(pictureKey, android.net.Uri.parse(picture));
        Intent typedPicture = (Intent) normalize.invoke(null, "com.google.android.gms", provider, recipient, -1, original);
        require(android.net.Uri.parse(picture).equals(typedPicture.getParcelableExtra(key,
                android.credentials.GetCredentialResponse.class).getCredential().getData()
                .getParcelable(pictureKey, android.net.Uri.class)), "Already typed picture Uri changed");
        legacy.putString("androidx.credentials.provider.extra.EXTRA_CREDENTIAL_TYPE", "fixture.credential.type");
        content.putString(pictureKey, picture);
        Intent otherType = (Intent) normalize.invoke(null, "com.google.android.gms", provider, recipient, -1, original);
        require(picture.equals(otherType.getParcelableExtra(key, android.credentials.GetCredentialResponse.class)
                .getCredential().getData().getString(pictureKey)), "Unrelated credential type received Google field conversion");
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 2, "Pass ordinary orderedbroadcast source and target APKs");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA", Files.createTempDirectory("delegated-retirement-").toString(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);

        Harness before = new Harness(args[0], args[1], true, true);
        Object paused = before.token(ActivityManager.INTENT_SENDER_BROADCAST,
                before.receiverIntent(), PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_ONE_SHOT);
        field(before.pendingType, "mBeforeSendDispatch").set(before.pending, (Runnable) before::retire);
        require(before.send(paused) == ActivityManager.START_CANCELED && before.thread.receivers == 0,
                "Validated send inherited reinstalled creator authority");

        Harness activity = new Harness(args[0], args[1], false, true);
        ActivityInfo launcher = activity.creator.launcherActivity(null);
        Object activityToken = method(activity.pendingType, "get", int.class, String.class, int.class,
                Intent[].class, String[].class, int.class).invoke(activity.pending,
                        ActivityManager.INTENT_SENDER_ACTIVITY, activity.target.packageName, 2,
                        new Intent[]{new Intent().setComponent(new ComponentName(
                                activity.creator.packageName, launcher.name))}, null, PendingIntent.FLAG_IMMUTABLE);
        field(activity.atmType, "mBeforeActivityPublication").set(activity.atm, (Runnable) activity::retire);
        require(activity.send(activityToken) == ActivityManager.START_CANCELED,
                "Resolved activity target inherited reinstalled code");
        require(((List<?>) field(activity.atmType, "mActivities").get(activity.atm)).isEmpty(),
                "Retired target published an activity record");

        Harness updated = new Harness(args[0], args[1], true, true);
        Object reusable = updated.token(ActivityManager.INTENT_SENDER_BROADCAST,
                updated.receiverIntent(), PendingIntent.FLAG_IMMUTABLE);
        method(PackageManagerService.class, "add", ApkPackage.class)
                .invoke(updated.pm, ApkPackage.parse(args[0]));
        require(updated.send(reusable) == 0 && updated.thread.receivers == 1,
                "Future send after safe same-code update lost its token");

        Harness oneShot = new Harness(args[0], args[1], false, true);
        Object consumed = oneShot.token(ActivityManager.INTENT_SENDER_SERVICE, oneShot.serviceIntent(),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_ONE_SHOT);
        require(oneShot.send(consumed) == 0 && oneShot.thread.starts == 1,
                "One-shot consumption incorrectly revoked its own live dispatch");
        require(oneShot.send(consumed) == ActivityManager.START_CANCELED, "One-shot token was reusable");

        Harness queuedService = new Harness(args[0], args[1], false, false);
        queuedService.send(queuedService.token(ActivityManager.INTENT_SENDER_SERVICE,
                queuedService.serviceIntent(), PendingIntent.FLAG_IMMUTABLE));
        queuedService.retire(); queuedService.resumeQueued();
        require(queuedService.thread.creates == 0 && queuedService.thread.starts == 0,
                "Cold service start retained retired creator authority");

        Harness between = new Harness(args[0], args[1], false, true);
        between.thread.onCreate = between::retire;
        between.send(between.token(ActivityManager.INTENT_SENDER_SERVICE,
                between.serviceIntent(), PendingIntent.FLAG_IMMUTABLE));
        require(between.thread.creates == 1 && between.thread.starts == 0,
                "Service start survived creator removal in the create callback");

        Harness binding = new Harness(args[0], args[1], false, false);
        BooleanSupplier lifetime = () -> binding.pm.get(binding.creator.packageName) == binding.creator;
        IServiceConnection connection = new IServiceConnection.Default() {
            private final IBinder binder = new Binder();
            @Override public IBinder asBinder() { return binder; }
        };
        method(binding.servicesType, "bindService", IApplicationThread.class, Intent.class, String.class,
                IServiceConnection.class, long.class, String.class, BooleanSupplier.class)
                .invoke(binding.services, null, binding.serviceIntent(), null, connection,
                        (long) Context.BIND_AUTO_CREATE, binding.creator.packageName, lifetime);
        binding.retire(); binding.resumeQueued();
        require(binding.thread.creates == 0 && binding.thread.binds == 0,
                "Cold binding retained retired creator authority");

        Harness queuedBroadcast = new Harness(args[0], args[1], true, false);
        queuedBroadcast.send(queuedBroadcast.token(ActivityManager.INTENT_SENDER_BROADCAST,
                queuedBroadcast.receiverIntent(), PendingIntent.FLAG_IMMUTABLE));
        queuedBroadcast.retire(); queuedBroadcast.resumeQueued();
        require(queuedBroadcast.thread.receivers == 0,
                "Queued manifest broadcast retained retired creator authority");
        require(((List<?>) field(queuedBroadcast.broadcastsType, "mManifest").get(queuedBroadcast.broadcasts)).isEmpty(),
                "Retired queued broadcast kept its reservation");

        Harness registered = new Harness(args[0], args[1], true, true);
        int[] received = {0};
        for (int i = 0; i < 2; i++) {
            final boolean first = i == 0;
            IIntentReceiver receiver = new IIntentReceiver.Stub() {
                @Override public void performReceive(Intent intent, int code, String data, Bundle extras,
                        boolean ordered, boolean sticky, int user) {
                    received[0]++; if (first) registered.retire();
                }
            };
            method(registered.broadcastsType, "registerReceiver", IApplicationThread.class, String.class,
                    IIntentReceiver.class, IntentFilter.class, int.class, String.class, int.class)
                    .invoke(registered.broadcasts, registered.thread, registered.target.packageName, receiver,
                            new IntentFilter("fixture.delegated"), 910001, null, Context.RECEIVER_EXPORTED);
        }
        registered.send(registered.token(ActivityManager.INTENT_SENDER_BROADCAST,
                new Intent("fixture.delegated").setPackage(registered.target.packageName), PendingIntent.FLAG_IMMUTABLE));
        require(received[0] == 1, "Later registered receiver inherited retired sender authority");
        for (int route = 0; route < 3; route++) {
            for (int race = 0; race < 6; race++) directCapture(args, route, race);
        }
        for (int mode = 0; mode < 5; mode++) intentSenderActivity(args, mode);
        credentialResultFormat();
        System.out.println("DelegatedIntentRetirementTest all passed: " + checks + " checks");
    }
}
