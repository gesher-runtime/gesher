/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.IApplicationThread;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;
import android.permission.IPermissionManager;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedPermissionGroupImpl;
import com.android.internal.pm.pkg.component.ParsedPermissionImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.PermissionPolicy;
import com.gesher.server.StandIn;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;
import java.util.function.ToIntBiFunction;
import sun.misc.Unsafe;

/** Real permission policy/controller/store with explicit metadata and caller transport fixtures. */
public final class RuntimePermissionStateTest {
    private static final int UID = 12882, PID = 898820;
    private static final String NAME = "fixture.permission.owner", PEER = "fixture.permission.peer";
    private static final String CAMERA = Manifest.permission.CAMERA;
    private static final String AUDIO = Manifest.permission.RECORD_AUDIO;
    private static final String ANSWER = Manifest.permission.ANSWER_PHONE_CALLS;
    private static final String CONTACTS = Manifest.permission.READ_CONTACTS;
    private static final String WRITE_CONTACTS = Manifest.permission.WRITE_CONTACTS;
    private static final String FINE = Manifest.permission.ACCESS_FINE_LOCATION;
    private static final String COARSE = Manifest.permission.ACCESS_COARSE_LOCATION;
    private static final String BACKGROUND = Manifest.permission.ACCESS_BACKGROUND_LOCATION;
    private static final String STORAGE = Manifest.permission.READ_EXTERNAL_STORAGE;
    private static final String NORMAL = Manifest.permission.VIBRATE;
    private static final String UNKNOWN = "fixture.permission.UNKNOWN";
    private static final String CUSTOM_READ = "fixture.permission.CUSTOM_READ";
    private static final String CUSTOM_WRITE = "fixture.permission.CUSTOM_WRITE";
    private static final String CUSTOM_OTHER = "fixture.permission.CUSTOM_OTHER";
    private static final String UNDEFINED_GROUP = "android.permission-group.UNDEFINED";
    private static final int USER_SET = PackageManager.FLAG_PERMISSION_USER_SET;
    private static final int USER_FIXED = PackageManager.FLAG_PERMISSION_USER_FIXED;
    // These are the actual JNI/controller protocol values, not injected permission results.
    private static final int DISMISSED = 0, DENIED = 1, ALLOWED = 2;
    private static final List<String> RUNTIME = List.of(CAMERA, AUDIO, ANSWER, CONTACTS, WRITE_CONTACTS, FINE, COARSE, BACKGROUND, STORAGE);
    private static final List<String> CUSTOM = List.of(CUSTOM_READ, CUSTOM_WRITE, CUSTOM_OTHER);
    private static int checks;
    private static boolean binder;
    private static final Unsafe UNSAFE;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (Exception e) { throw new ExceptionInInitializerError(e); }
    }
    private interface Checked { void run() throws Exception; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object value, String name) throws Exception { return field(value.getClass(), name).get(value); }
    private static void set(Object value, String name, Object fieldValue) throws Exception { field(value.getClass(), name).set(value, fieldValue); }
    private static Object make(Class<?> type, Class<?>[] signature, Object... args) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(signature); constructor.setAccessible(true);
        return constructor.newInstance(args);
    }
    private static Object call(Object value, String name, Class<?>[] signature, Object... args) throws Exception {
        Method method = value.getClass().getDeclaredMethod(name, signature); method.setAccessible(true);
        return method.invoke(value, args);
    }
    private static ApkPackage pkg(String name, int uid, int target, byte signer) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(target);
        for (String permission : RUNTIME) parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0, java.util.Set.of()));
        for (String permission : CUSTOM) parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0, java.util.Set.of()));
        if (NAME.equals(name)) {
            for (String groupName : List.of("fixture.permission.CUSTOM_GROUP", "fixture.permission.OTHER_GROUP")) {
                ParsedPermissionGroupImpl group = new ParsedPermissionGroupImpl();
                group.setName(groupName); group.setPackageName(name); parsed.addPermissionGroup(group);
            }
            for (String permissionName : CUSTOM) {
                ParsedPermissionImpl permission = new ParsedPermissionImpl();
                permission.setName(permissionName); permission.setPackageName(name);
                permission.setProtectionLevel(PermissionInfo.PROTECTION_DANGEROUS);
                permission.setGroup(permissionName.equals(CUSTOM_OTHER)
                        ? "fixture.permission.OTHER_GROUP" : "fixture.permission.CUSTOM_GROUP");
                parsed.addPermission(permission);
            }
        }
        parsed.addUsesPermission(new ParsedUsesPermissionImpl(NORMAL, 0, java.util.Set.of()));
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(apk, "packageName", name); set(apk, "parsed", parsed); set(apk, "mUid", uid);
        set(apk, "apkPath", "/fixture/" + name + "/base.apk"); set(apk, "mDataDir", "/fixture/" + name + "/data");
        set(apk, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
        set(apk, "mSigningIdentityChecked", true);
        set(apk, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{signer})});
        return apk;
    }
    private static IPermissionManager api(PermissionPolicy policy) {
        if (!binder) return policy;
        IBinder target = StandIn.of(IPermissionManager.class, policy).asBinder();
        IBinder transport = new Binder() {
            @Override public IInterface queryLocalInterface(String name) { return null; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                return target.transact(code, data, reply, flags);
            }
        };
        return IPermissionManager.Stub.asInterface(transport);
    }
    private static final class Harness {
        ApkPackage owner = pkg(NAME, UID, 36, (byte)7);
        final ApkPackage peer = pkg(PEER, UID + 1, 36, (byte)8);
        final PackageManagerService pm;
        final PermissionPolicy policy;
        final IPermissionManager query;
        final Object atm, am, process;
        @SuppressWarnings("unchecked") Harness() throws Exception {
            pm = (PackageManagerService)make(PackageManagerService.class,
                    new Class<?>[]{Collection.class, boolean.class}, List.of(owner, peer), false);
            set(pm, "mPackageEventSender", (Consumer<Intent>)event -> { });
            policy = pm.permissions();
            if (!binder) {
                // The host archive has no framework-res.apk. These raw attributes
                // are pinned to android-16.0.0_r4/core/res/AndroidManifest.xml
                // (SHA-256 201d8390999de22fc6363a40ffdda84c2ed1a4cbd3c4f18cab6d721b505797a9).
                // In particular, the real manifest exposes UNDEFINED, not the
                // PermissionController's logical groups. Windows reads the actual APK.
                Object catalog = get(policy, "mCatalog");
                Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>)get(catalog, "mFramework");
                for (String name : RUNTIME) {
                    String background = name.equals(FINE) || name.equals(COARSE) ? BACKGROUND
                            : name.equals(CAMERA) ? Manifest.permission.BACKGROUND_CAMERA
                            : name.equals(AUDIO) ? Manifest.permission.RECORD_BACKGROUND_AUDIO : null;
                    PermissionInfo info = new PermissionInfo(background);
                    info.name = name; info.packageName = "android";
                    info.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS;
                    if (name.equals(CAMERA) || name.equals(AUDIO) || name.equals(FINE) || name.equals(COARSE) || name.equals(BACKGROUND)) {
                        info.protectionLevel |= PermissionInfo.PROTECTION_FLAG_INSTANT;
                    }
                    if (name.equals(ANSWER)) info.protectionLevel |= PermissionInfo.PROTECTION_FLAG_RUNTIME_ONLY;
                    info.group = UNDEFINED_GROUP;
                    if (name.equals(BACKGROUND)) info.flags = PermissionInfo.FLAG_HARD_RESTRICTED;
                    if (name.equals(STORAGE)) info.flags = PermissionInfo.FLAG_SOFT_RESTRICTED | PermissionInfo.FLAG_IMMUTABLY_RESTRICTED;
                    definitions.put(name, info);
                }
                PermissionInfo normal = new PermissionInfo(); normal.name = NORMAL; normal.packageName = "android";
                normal.protectionLevel = PermissionInfo.PROTECTION_NORMAL | PermissionInfo.PROTECTION_FLAG_INSTANT;
                definitions.put(NORMAL, normal);
                set(catalog, "mLoaded", true);
                call(policy, "initializePackages", new Class<?>[0]);
            }
            atm = make(type("ActivityTaskManagerService"), new Class<?>[]{PackageManagerService.class}, pm);
            am = make(type("ActivityManagerService"), new Class<?>[]{atm.getClass(), PackageManagerService.class,
                    type("NotificationManagerService")}, atm, pm, null);
            IApplicationThread thread = new IApplicationThread.Default() {
                final Binder token = new Binder(); @Override public IBinder asBinder() { return token; }
            };
            long identity = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long)UID << 32) | PID);
                process = make(type("ActivityTaskManagerService$ProcessRecord"),
                        new Class<?>[]{atm.getClass(), int.class, IApplicationThread.class}, atm, PID, thread);
            } finally { Binder.restoreCallingIdentity(identity); }
            set(process, "packageName", NAME); set(process, "processName", NAME);
            set(process, "packageCode", owner); set(process, "ready", true);
            ((List<Object>)get(atm, "mProcesses")).add(process);
            query = api(policy);
        }
        void asApp(Checked action) throws Exception {
            long identity = Binder.clearCallingIdentity();
            try { Binder.restoreCallingIdentity(((long)UID << 32) | PID); action.run(); }
            finally { Binder.restoreCallingIdentity(identity); }
        }
        boolean rationale(String permission) throws Exception {
            boolean[] result = new boolean[1];
            asApp(() -> result[0] = query.shouldShowRequestPermissionRationale(NAME, permission, 0, 0));
            return result[0];
        }
        int state(String permission) throws Exception {
            int[] result = new int[1];
            asApp(() -> result[0] = query.getPermissionRequestState(NAME, permission, 0));
            return result[0];
        }
        int flags(String permission) { return policy.getPermissionFlags(NAME, permission, "default:0", 0); }
        void flags(String permission, int values) {
            policy.updatePermissionFlags(NAME, permission, PackageManager.MASK_PERMISSION_FLAGS_ALL, values, true, "default:0", 0);
        }
        void reset() {
            List<String> permissions = new ArrayList<>(RUNTIME); permissions.addAll(CUSTOM);
            for (String permission : permissions) {
                flags(permission, 0);
                policy.revokeRuntimePermission(NAME, permission, "default:0", 0, "fixture reset");
            }
        }
        void flush() throws Exception {
            check((Boolean)call(get(policy, "mWriter"), "flush", new Class<?>[0]), "permission ledger flush failed");
        }
        PermissionPolicy reopen() throws Exception {
            PermissionPolicy restored = (PermissionPolicy)make(PermissionPolicy.class,
                    new Class<?>[]{PackageManagerService.class}, pm);
            call(restored, "attach", new Class<?>[]{type("CallerIdentity")}, get(policy, "mCallers"));
            if (!binder) set(restored, "mCatalog", get(policy, "mCatalog"));
            return restored;
        }
        void install(ApkPackage replacement) throws Exception {
            call(pm, "add", new Class<?>[]{ApkPackage.class}, replacement);
            owner = replacement; set(process, "packageCode", owner);
        }
        Object controller(Executor executor, ToIntBiFunction<String, String> choice) throws Exception {
            Class<?> requests = type("PermissionRequests"), decisions = type("PermissionRequests$Decision");
            Object decision = Proxy.newProxyInstance(decisions.getClassLoader(), new Class<?>[]{decisions},
                    (proxy, method, args) -> {
                        int result = choice.applyAsInt((String)args[0], (String)args[1]);
                        // Allow the same test to expose missing denial history on
                        // the baseline's older boolean-only injection boundary.
                        if (method.getReturnType() == boolean.class) return result == ALLOWED;
                        return result;
                    });
            return make(requests, new Class<?>[]{PackageManagerService.class, Executor.class, decisions}, pm, executor, decision);
        }
        Reply request(Object controller, String... permissions) throws Exception {
            Reply reply = new Reply(); RuntimePermissionStateTest.request(controller, owner, permissions, reply); return reply;
        }
    }
    private static void groups(Harness h) throws Exception {
        h.reset();
        for (String permission : List.of(CAMERA, CONTACTS, WRITE_CONTACTS, AUDIO, FINE, COARSE)) {
            check(UNDEFINED_GROUP.equals(h.policy.getPermissionInfo(permission, "android", 0).group),
                    "test must use the raw framework group, not idealized permission-controller metadata");
        }
        Object deny = choose(h, DENIED, new AtomicInteger());
        h.request(deny, CAMERA);
        h.request(deny, CONTACTS);
        h.request(deny, AUDIO);
        check(h.flags(CAMERA) == USER_SET && h.flags(CONTACTS) == USER_SET && h.flags(AUDIO) == USER_SET,
                "unrelated platform groups shared first-denial history through UNDEFINED");
        check(h.rationale(CONTACTS) && h.rationale(AUDIO), "first contacts/microphone denial lost its retry rationale");
        h.request(deny, WRITE_CONTACTS);
        check(h.flags(WRITE_CONTACTS) == (USER_SET | USER_FIXED), "logical contacts group lost repeated-denial history");
        h.request(deny, AUDIO);
        check(h.flags(AUDIO) == (USER_SET | USER_FIXED) && h.flags(CONTACTS) == USER_SET,
                "microphone retry changed an unrelated contacts decision");

        // PermissionController keeps a separate denial history for the
        // foreground and background halves of a split permission group.
        final int decisionFlags = USER_SET | USER_FIXED;
        final int exempt = PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT;
        h.reset();
        h.flags(BACKGROUND, exempt);
        h.request(deny, FINE);
        h.request(deny, BACKGROUND);
        check((h.flags(BACKGROUND) & decisionFlags) == USER_SET,
                "Foreground denial fixed the first background decision");
        check(h.state(BACKGROUND) == Context.PERMISSION_REQUEST_STATE_REQUESTABLE,
                "First background denial remained unavailable");
        h.request(deny, BACKGROUND);
        check((h.flags(BACKGROUND) & decisionFlags) == decisionFlags,
                "Repeated background denial failed to retain its own history");
        h.reset();
        h.flags(BACKGROUND, exempt);
        h.request(deny, BACKGROUND);
        h.request(deny, COARSE);
        check((h.flags(COARSE) & decisionFlags) == USER_SET,
                "Background denial fixed the first foreground decision");
        h.request(deny, FINE);
        check((h.flags(FINE) & decisionFlags) == decisionFlags,
                "Fine and coarse lost their shared foreground denial history");
        h.reset();

        // These definitions come through production PermissionCatalog from the
        // fixture's actual ParsedPermission declarations, on both host and ART.
        h.request(deny, CUSTOM_READ);
        h.request(deny, CUSTOM_OTHER);
        check(h.flags(CUSTOM_READ) == USER_SET && h.flags(CUSTOM_OTHER) == USER_SET,
                "different custom permission groups shared denial history");
        h.request(deny, CUSTOM_WRITE);
        check(h.flags(CUSTOM_WRITE) == (USER_SET | USER_FIXED), "declared custom group lost denial history");
        h.reset();
    }
    private static final class Reply implements Consumer<Intent> {
        int count; Intent result;
        @Override public void accept(Intent intent) { count++; result = intent; }
        int grant(int index) { return result.getIntArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS)[index]; }
    }
    private static void request(Object controller, ApkPackage owner, String[] names, Reply reply) throws Exception {
        call(controller, "request", new Class<?>[]{ApkPackage.class, String[].class, Consumer.class}, owner, names, reply);
    }
    private static Object choose(Harness h, int decision, AtomicInteger prompts) throws Exception {
        return h.controller(Runnable::run, (owner, names) -> {
            check(NAME.equals(owner), "dialog used an unauthenticated owner");
            prompts.incrementAndGet(); return decision;
        });
    }
    private static void decisions(Harness h) throws Exception {
        h.reset();
        check(!h.rationale(CAMERA), "new permission incorrectly requests rationale");
        check(h.state(CAMERA) == Context.PERMISSION_REQUEST_STATE_REQUESTABLE, "new permission unavailable");
        AtomicInteger prompts = new AtomicInteger();
        Object deny = choose(h, DENIED, prompts);
        Reply first = h.request(deny, CAMERA, CAMERA, NORMAL, UNKNOWN);
        check(first.count == 1 && first.grant(0) == PackageManager.PERMISSION_DENIED
                && first.grant(1) == PackageManager.PERMISSION_DENIED, "denied duplicate lost its single result");
        check(first.grant(2) == PackageManager.PERMISSION_GRANTED && first.grant(3) == PackageManager.PERMISSION_DENIED,
                "normal/unknown permission results changed");
        check(h.flags(CAMERA) == USER_SET && h.rationale(CAMERA), "first denial did not enable rationale");
        check(h.state(CAMERA) == Context.PERMISSION_REQUEST_STATE_REQUESTABLE, "first denial prevented retry");
        h.flush();
        PermissionPolicy restored = h.reopen();
        h.asApp(() -> check(api(restored).shouldShowRequestPermissionRationale(NAME, CAMERA, 0, 0), "denial rationale lost on reload"));
        h.request(deny, CAMERA);
        check(h.flags(CAMERA) == (USER_SET | USER_FIXED) && !h.rationale(CAMERA), "second denial did not fix permission");
        check(h.state(CAMERA) == Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE, "fixed denial remained requestable");
        Reply third = h.request(choose(h, ALLOWED, prompts), CAMERA);
        check(prompts.get() == 2 && third.count == 1 && third.grant(0) == PackageManager.PERMISSION_DENIED,
                "permanent denial prompted again or gained permission");
        h.flush();
        PermissionPolicy fixed = h.reopen();
        check(!fixed.canRequest(NAME, CAMERA) && fixed.getPermissionFlags(NAME, CAMERA, "default:0", 0) == (USER_SET | USER_FIXED),
                "permanent denial lost on reload");

        h.reset(); prompts.set(0);
        Object allow = choose(h, ALLOWED, prompts);
        Reply granted = h.request(allow, CAMERA);
        check(granted.grant(0) == PackageManager.PERMISSION_GRANTED && h.flags(CAMERA) == USER_SET, "explicit grant lost user history");
        check(!h.rationale(CAMERA) && h.state(CAMERA) == Context.PERMISSION_REQUEST_STATE_GRANTED, "granted permission asked for rationale");
        h.request(allow, CAMERA);
        check(prompts.get() == 1, "already granted permission prompted again");

        h.reset();
        h.request(deny, CONTACTS, WRITE_CONTACTS);
        check(h.flags(CONTACTS) == USER_SET && h.flags(WRITE_CONTACTS) == USER_SET, "one group request counted twice");
        h.reset();
        h.request(deny, CONTACTS);
        h.request(deny, WRITE_CONTACTS);
        check(h.flags(WRITE_CONTACTS) == (USER_SET | USER_FIXED), "second decision ignored existing group history");
        h.reset();
        h.request(deny, CAMERA);
        h.request(allow, CAMERA);
        check(h.flags(CAMERA) == USER_SET && !h.rationale(CAMERA), "retry grant retained denial rationale");
    }
    private static void exclusions(Harness h) throws Exception {
        h.reset();
        if (!binder) {
            Method unavailableDialog = type("PermissionRequests").getDeclaredMethod("confirmDecision", String.class, String.class);
            unavailableDialog.setAccessible(true);
            check((Integer)unavailableDialog.invoke(null, NAME, CAMERA) == DISMISSED,
                    "unavailable native dialog was counted as an explicit denial");
        }
        for (int choice : new int[]{DISMISSED, -100}) {
            Reply reply = h.request(choose(h, choice, new AtomicInteger()), CAMERA);
            check(reply.count == 1 && reply.grant(0) == PackageManager.PERMISSION_DENIED
                    && h.flags(CAMERA) == 0 && !h.rationale(CAMERA), "dismissal/invalid decision created user history");
        }
        Reply unavailable = h.request(h.controller(Runnable::run, (pkg, names) -> { throw new IllegalStateException("no dialog"); }), CAMERA);
        check(unavailable.count == 1 && h.flags(CAMERA) == 0, "dialog failure changed permission history");
        for (int flag : new int[]{USER_FIXED, PackageManager.FLAG_PERMISSION_POLICY_FIXED, PackageManager.FLAG_PERMISSION_SYSTEM_FIXED}) {
            h.flags(CAMERA, USER_SET | flag);
            check(!h.rationale(CAMERA) && !h.policy.canRequest(NAME, CAMERA), "fixed permission remained requestable");
            AtomicInteger prompts = new AtomicInteger();
            Reply reply = h.request(choose(h, ALLOWED, prompts), CAMERA);
            check(prompts.get() == 0 && reply.grant(0) == PackageManager.PERMISSION_DENIED, "host dialog bypassed fixed flags");
        }
        h.reset();
        h.flags(BACKGROUND, USER_SET | PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION);
        check(!h.rationale(BACKGROUND) && !h.policy.canRequest(NAME, BACKGROUND), "hard-restricted permission could be requested");
        h.flags(STORAGE, USER_SET | PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION);
        check(h.rationale(STORAGE) && h.policy.canRequest(NAME, STORAGE), "soft restriction was treated as hard denial");
        Reply soft = h.request(choose(h, ALLOWED, new AtomicInteger()), STORAGE);
        check(soft.grant(0) == PackageManager.PERMISSION_GRANTED, "soft-restricted permission grant was fabricated or dropped");
        check(h.pm.appOps().checkOperationRaw(AppOpsManager.OP_READ_EXTERNAL_STORAGE, UID, NAME, null)
                == AppOpsManager.MODE_IGNORED, "permission consent removed the soft AppOps restriction");
        h.flags(CAMERA, USER_SET);
        h.asApp(() -> {
            check(!h.query.shouldShowRequestPermissionRationale(PEER, CAMERA, 0, 0), "rationale exposed another UID");
            check(h.query.getPermissionRequestState(PEER, CAMERA, 0) == Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE,
                    "request state exposed another UID");
            check(h.query.getPermissionRequestState(PEER, NORMAL, 0) == Context.PERMISSION_REQUEST_STATE_GRANTED,
                    "request state ignored the caller's existing grant before package requestability");
            check(!h.query.shouldShowRequestPermissionRationale(NAME, CAMERA, 0, 10), "rationale leaked to another user");
            check(!h.query.shouldShowRequestPermissionRationale(NAME, CAMERA, 91, 0), "rationale leaked to another device");
            check(h.query.getPermissionRequestState(NAME, CAMERA, 91) == Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE,
                    "unsupported device claimed request support");
            check(!h.query.shouldShowRequestPermissionRationale(NAME, UNKNOWN, 0, 0), "unknown permission has rationale");
            check(!h.query.shouldShowRequestPermissionRationale(NAME, NORMAL, 0, 0), "normal permission has rationale");
            check(!h.query.shouldShowRequestPermissionRationale("missing.package", CAMERA, 0, 0), "unknown package has rationale");
        });
        check(!h.policy.shouldShowRequestPermissionRationale(NAME, CAMERA, 0, 0), "host UID got another UID's self-rationale");
        long identity = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)UID << 32) | (PID + 20));
            check(!h.policy.shouldShowRequestPermissionRationale(NAME, CAMERA, 0, 0), "unknown PID borrowed an installed UID");
        } finally { Binder.restoreCallingIdentity(identity); }
        ApkPackage shared = pkg("fixture.permission.shared", UID, 36, (byte)7);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, shared);
        h.asApp(() -> check(h.query.shouldShowRequestPermissionRationale(shared.packageName, CAMERA, 0, 0),
                "shared UID lost its permission denial history"));
        call(h.pm, "remove", new Class<?>[]{String.class}, shared.packageName);
    }
    private static void lifecycle(Harness h) throws Exception {
        h.reset();
        List<Runnable> queue = new ArrayList<>();
        AtomicInteger prompts = new AtomicInteger();
        Object delayed = h.controller(queue::add, (pkg, names) -> { prompts.incrementAndGet(); return DENIED; });
        Reply original = h.request(delayed, CAMERA);
        Reply duplicate = h.request(delayed, CAMERA);
        check(original.count == 0 && duplicate.count == 1 && queue.size() == 1, "overlapping request lost its terminal result");
        check(h.flags(CAMERA) == 0, "rejected overlapping request recorded a user denial");
        queue.remove(0).run();
        check(original.count == 1 && prompts.get() == 1 && h.flags(CAMERA) == USER_SET,
                "overlapping request counted as two denials");
        for (int decision : new int[]{DENIED, ALLOWED}) {
            h.reset();
            ApkPackage captured = h.owner;
            Object stale = h.controller(Runnable::run, (pkg, names) -> {
                try { h.install(pkg(NAME, UID, 36, (byte)7)); }
                catch (Exception e) { throw new AssertionError(e); }
                return decision;
            });
            Reply reply = new Reply(); request(stale, captured, new String[]{CAMERA}, reply);
            check(reply.count == 1 && reply.grant(0) == PackageManager.PERMISSION_DENIED && h.flags(CAMERA) == 0,
                    "replacement APK inherited an in-flight user decision");
            h.reset();
            Object fixed = h.controller(Runnable::run, (pkg, names) -> {
                h.flags(CAMERA, PackageManager.FLAG_PERMISSION_POLICY_FIXED); return decision;
            });
            Reply blocked = h.request(fixed, CAMERA);
            check(blocked.grant(0) == PackageManager.PERMISSION_DENIED && h.flags(CAMERA) == PackageManager.FLAG_PERMISSION_POLICY_FIXED,
                    "in-flight dialog bypassed new policy restriction");
            h.reset();
            Object retiring = h.controller(Runnable::run, (pkg, names) -> {
                try { call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner); }
                catch (Exception e) { throw new AssertionError(e); }
                return decision;
            });
            try {
                Reply removed = h.request(retiring, CAMERA);
                check(removed.count == 1 && removed.grant(0) == PackageManager.PERMISSION_DENIED && h.flags(CAMERA) == 0,
                        "retiring APK acquired an in-flight user decision");
            } finally { call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.owner); }
        }
        h.reset();
        Object concurrentGrant = h.controller(Runnable::run, (pkg, names) -> {
            h.policy.grantRuntimePermission(NAME, CAMERA, "default:0", 0); return DENIED;
        });
        Reply granted = h.request(concurrentGrant, CAMERA);
        check(granted.grant(0) == PackageManager.PERMISSION_GRANTED && h.flags(CAMERA) == 0,
                "stale denial revoked a concurrently accepted grant");
        h.reset();
        for (int target : new int[]{29, 30, 36}) {
            h.install(pkg(NAME, UID, target, (byte)7)); h.flags(BACKGROUND, 0);
            check(h.rationale(BACKGROUND) == (target >= 30), "background rationale ignored target SDK transition");
        }
        h.reset();
        h.install(pkg(NAME, UID, 22, (byte)7));
        check(!h.policy.canRequest(NAME, CAMERA), "pre-runtime target entered modern consent flow");
        check(h.flags(ANSWER) == 0 && h.state(ANSWER) == Context.PERMISSION_REQUEST_STATE_REQUESTABLE,
                "public legacy runtime-only request state inherited the controller target gate");
        check(!h.policy.canRequest(NAME, ANSWER), "legacy runtime-only query enabled a modern dialog");
        AtomicInteger legacyPrompts = new AtomicInteger();
        Reply legacy = h.request(choose(h, ALLOWED, legacyPrompts), ANSWER);
        check(legacyPrompts.get() == 0 && legacy.grant(0) == PackageManager.PERMISSION_DENIED && h.flags(ANSWER) == 0,
                "requestable legacy query granted or prompted for a runtime-only permission");
        h.flags(ANSWER, USER_SET | USER_FIXED);
        check(h.state(ANSWER) == Context.PERMISSION_REQUEST_STATE_UNREQUESTABLE,
                "legacy public query ignored a fixed runtime-only permission");
        h.install(pkg(NAME, UID, 36, (byte)7));
        h.reset(); h.flush();
    }
    public static int run(boolean binderTransport) throws Exception {
        binder = binderTransport; checks = 0;
        Harness harness = new Harness();
        decisions(harness); groups(harness); exclusions(harness); lifecycle(harness);
        return checks;
    }
    public static void main(String[] args) throws Exception {
        int total = run(true);
        System.out.println("RuntimePermissionStateTest all passed: " + total + " checks; binder=true");
    }
}
