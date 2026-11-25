/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.IApplicationThread;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
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
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
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
    private static final String READ_LOG = Manifest.permission.READ_CALL_LOG;
    private static final String WRITE_LOG = Manifest.permission.WRITE_CALL_LOG;
    private static final String OUTGOING = Manifest.permission.PROCESS_OUTGOING_CALLS;
    private static final List<String> CALL_LOG = List.of(READ_LOG, WRITE_LOG, OUTGOING);
    private static final String NORMAL = Manifest.permission.VIBRATE;
    private static final String UNKNOWN = "fixture.permission.UNKNOWN";
    private static final String CUSTOM_READ = "fixture.permission.CUSTOM_READ";
    private static final String CUSTOM_WRITE = "fixture.permission.CUSTOM_WRITE";
    private static final String CUSTOM_OTHER = "fixture.permission.CUSTOM_OTHER";
    private static final String UNDEFINED_GROUP = "android.permission-group.UNDEFINED";
    private static final int USER_SET = PackageManager.FLAG_PERMISSION_USER_SET;
    private static final int USER_FIXED = PackageManager.FLAG_PERMISSION_USER_FIXED;
    private static final int APPLY = PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION;
    private static final int INSTALLER_EXEMPT = PackageManager.FLAG_PERMISSION_RESTRICTION_INSTALLER_EXEMPT;
    private static final int SYSTEM_EXEMPT = PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT;
    private static final int ANY_EXEMPT = PackageManager.FLAGS_PERMISSION_RESTRICTION_ANY_EXEMPT;
    private static final int GRANTED = PackageManager.PERMISSION_GRANTED, DENY = PackageManager.PERMISSION_DENIED;
    // These are the actual JNI/controller protocol values, not injected permission results.
    private static final int DISMISSED = 0, DENIED = 1, ALLOWED = 2, FOREGROUND_ONLY = 3, ALWAYS = 4;
    private static final List<String> RUNTIME = List.of(CAMERA, AUDIO, ANSWER, CONTACTS, WRITE_CONTACTS,
            FINE, COARSE, BACKGROUND, STORAGE, READ_LOG, WRITE_LOG, OUTGOING);
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
        return pkg(name, uid, target, signer, null);
    }
    private static ApkPackage pkg(String name, int uid, int target, byte signer, String sharedUid) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(target);
        if (sharedUid != null) parsed.setSharedUserId(sharedUid);
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
            // PM boots without the fixture packages; they are installed below. A boot never
            // invents a restriction exemption once the ledger's one-time migration has run,
            // and the data directory may hold a ledger a previous process already migrated.
            pm = (PackageManagerService)make(PackageManagerService.class,
                    new Class<?>[]{Collection.class, boolean.class}, List.of(), false);
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
                    if (name.equals(BACKGROUND) || CALL_LOG.contains(name)) info.flags = PermissionInfo.FLAG_HARD_RESTRICTED;
                    if (name.equals(STORAGE)) info.flags = PermissionInfo.FLAG_SOFT_RESTRICTED | PermissionInfo.FLAG_IMMUTABLY_RESTRICTED;
                    definitions.put(name, info);
                }
                PermissionInfo normal = new PermissionInfo(); normal.name = NORMAL; normal.packageName = "android";
                normal.protectionLevel = PermissionInfo.PROTECTION_NORMAL | PermissionInfo.PROTECTION_FLAG_INSTANT;
                definitions.put(NORMAL, normal);
                set(catalog, "mLoaded", true);
                call(policy, "initializePackages", new Class<?>[0]);
            }
            // The Installer's path: AOSP's default restricted-permission allowlist
            // (SessionParams, INSTALL_ALL_WHITELIST_RESTRICTED_PERMISSIONS) recorded on the
            // installed APK, then its publication.
            for (ApkPackage installed : List.of(owner, peer)) {
                call(installed, "setRestrictedPermissionAllowlist", new Class<?>[]{Collection.class}, (Object)null);
                call(pm, "add", new Class<?>[]{ApkPackage.class}, installed);
            }
            ((java.util.concurrent.ExecutorService)get(pm, "mPackageEventExecutor")).submit(() -> { }).get(5, java.util.concurrent.TimeUnit.SECONDS);
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
        Collection<String> selection(PackageInstaller.SessionParams params) {
            return (params.installFlags & PackageManager.INSTALL_ALL_WHITELIST_RESTRICTED_PERMISSIONS) != 0
                    ? null : params.whitelistedRestrictedPermissions == null ? List.of()
                    : List.copyOf(params.whitelistedRestrictedPermissions);
        }
        void publishSession(PackageInstaller.SessionParams params) throws Exception {
            // The identical admitted APK models a verified reinstall. This invokes the
            // production Installer's unchanged-publication boundary; session capture is
            // exercised independently by PackageInstallerSessionTest and its host test.
            call(policy, "addInstallerAllowlist", new Class<?>[]{ApkPackage.class, Collection.class},
                    owner, selection(params));
        }
        void freshInstall(PackageInstaller.SessionParams params) throws Exception {
            policy.forgetPackage(NAME);
            call(pm, "remove", new Class<?>[]{String.class}, NAME);
            ApkPackage replacement = pkg(NAME, UID, 36, (byte)7);
            call(replacement, "setRestrictedPermissionAllowlist", new Class<?>[]{Collection.class}, selection(params));
            call(pm, "add", new Class<?>[]{ApkPackage.class}, replacement);
            owner = replacement; set(process, "packageCode", owner);
        }
        Object controller(Executor executor, ToIntBiFunction<String, String> choice) throws Exception {
            return controller(executor, choice, choice);
        }
        /** Separate injected answers for the foreground and the background dialog. */
        Object controller(Executor executor, ToIntBiFunction<String, String> choice,
                ToIntBiFunction<String, String> background) throws Exception {
            Class<?> requests = type("PermissionRequests"), decisions = type("PermissionRequests$Decision");
            Object decision = Proxy.newProxyInstance(decisions.getClassLoader(), new Class<?>[]{decisions},
                    (proxy, method, args) -> {
                        int result = (method.getName().equals("decideBackground") ? background : choice)
                                .applyAsInt((String)args[0], (String)args[1]);
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
        // foreground and background halves of a split permission group. The
        // background half has its own choice, offered once the foreground is granted. A
        // target below R gets the request's upgrade dialog, whose refusals are counted.
        final int decisionFlags = USER_SET | USER_FIXED;
        final int exempt = PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT;
        Object foregroundOnly = h.controller(Runnable::run, (owner, names) -> DENIED, (owner, names) -> FOREGROUND_ONLY);
        h.install(pkg(NAME, UID, 29, (byte)7));
        h.reset();
        h.flags(BACKGROUND, exempt);
        h.request(deny, FINE);
        h.policy.grantRuntimePermission(NAME, FINE, "default:0", 0);
        h.request(foregroundOnly, BACKGROUND);
        check((h.flags(BACKGROUND) & decisionFlags) == USER_SET,
                "Foreground denial fixed the first background decision");
        check(h.state(BACKGROUND) == Context.PERMISSION_REQUEST_STATE_REQUESTABLE,
                "First background denial remained unavailable");
        h.request(foregroundOnly, BACKGROUND);
        check((h.flags(BACKGROUND) & decisionFlags) == decisionFlags,
                "Repeated background denial failed to retain its own history");
        h.reset();
        h.flags(BACKGROUND, exempt);
        h.policy.grantRuntimePermission(NAME, COARSE, "default:0", 0);
        h.request(foregroundOnly, BACKGROUND);
        check((h.flags(BACKGROUND) & decisionFlags) == USER_SET, "Background-only choice lost its user decision");
        h.policy.revokeRuntimePermission(NAME, COARSE, "default:0", 0, "fixture");
        h.request(deny, COARSE);
        check((h.flags(COARSE) & decisionFlags) == USER_SET,
                "Background denial fixed the first foreground decision");
        h.request(deny, FINE);
        check((h.flags(FINE) & decisionFlags) == decisionFlags,
                "Fine and coarse lost their shared foreground denial history");
        h.install(pkg(NAME, UID, 36, (byte)7));
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
        // AOSP SoftRestrictedPermissionPolicy: from target Q the soft restriction limits only
        // OP_LEGACY_STORAGE; the permission's own AppOp follows the grant.
        check(h.pm.appOps().checkOperationRaw(AppOpsManager.OP_READ_EXTERNAL_STORAGE, UID, NAME, null)
                == AppOpsManager.MODE_ALLOWED, "soft restriction withheld a Q+ target's storage AppOp");
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


    private static PackageInstaller.SessionParams session(Set<String> names) {
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setWhitelistedRestrictedPermissions(names);
        return params;
    }
    private static void withIdentity(int uid, int pid, Checked action) throws Exception {
        long identity = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)uid << 32) | pid); action.run(); }
        finally { Binder.restoreCallingIdentity(identity); }
    }
    private static void queryDenied(Checked action, String message) throws Exception {
        try { action.run(); throw new AssertionError(message); }
        catch (SecurityException expected) { checks++; }
    }
    private static Object installSource(ApkPackage installer) throws Exception {
        return make(type("PackageInstallSource"), new Class<?>[]{String.class, String.class,
                int.class, android.content.pm.SigningDetails.class}, installer.packageName,
                call(installer, "enabledStateOwner", new Class<?>[0]), PackageInstaller.PACKAGE_SOURCE_STORE, null);
    }
    @SuppressWarnings("unchecked")
    private static void allowlistQueries(Harness h) throws Exception {
        int category = PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER;
        call(h.pm, "attachInstallSourceCallers", new Class<?>[]{h.atm.getClass()}, h.atm);
        check(call(h.pm, "getVisibleSearchPackage", new Class<?>[]{int.class, String.class, String.class},
                UID, NAME, PEER) == null, "allowlist visibility fixture must hide the installed peer");
        h.asApp(() -> check(h.query.getAllowlistedRestrictedPermissions(PEER, category, 0) == null
                && h.query.getAllowlistedRestrictedPermissions("fixture.permission.absent", category, 0) == null,
                "allowlist query distinguished an invisible package from a missing package"));
        Object originalSource = call(h.owner, "installSource", new Class<?>[0]);
        call(h.owner, "setInstallSource", new Class<?>[]{type("PackageInstallSource")}, installSource(h.owner));
        h.asApp(() -> check(h.query.getAllowlistedRestrictedPermissions(NAME, category, 0).containsAll(CALL_LOG),
                "current recorded installer could not query its allowlist"));
        h.asApp(() -> queryDenied(() -> h.query.getAllowlistedRestrictedPermissions(NAME,
                PackageManager.FLAG_PERMISSION_WHITELIST_SYSTEM, 0), "installer read the privileged system allowlist"));
        withIdentity(1001, PID + 20, () -> queryDenied(
                () -> h.query.getAllowlistedRestrictedPermissions(NAME, category, 0),
                "an unattached reserved UID acquired allowlist authority"));

        String group = "fixture.permission.installers";
        ApkPackage recorded = pkg(group + ".owner", UID + 10, 36, (byte)9, group);
        ApkPackage shared = pkg(group + ".peer", UID + 10, 36, (byte)9, group);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, recorded);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, shared);
        IApplicationThread thread = new IApplicationThread.Default() {
            final Binder token = new Binder(); @Override public IBinder asBinder() { return token; }
        };
        Object[] process = new Object[1];
        withIdentity(UID + 10, PID + 30, () -> process[0] = make(type("ActivityTaskManagerService$ProcessRecord"),
                new Class<?>[]{h.atm.getClass(), int.class, IApplicationThread.class}, h.atm, PID + 30, thread));
        set(process[0], "packageName", shared.packageName); set(process[0], "processName", shared.packageName);
        set(process[0], "packageCode", shared); set(process[0], "ready", true);
        List<Object> processes = (List<Object>)get(h.atm, "mProcesses"); processes.add(process[0]);
        try {
            call(h.owner, "setInstallSource", new Class<?>[]{type("PackageInstallSource")}, installSource(recorded));
            withIdentity(UID + 10, PID + 30, () -> check(
                    h.query.getAllowlistedRestrictedPermissions(NAME, category, 0).containsAll(CALL_LOG),
                    "admitted shared-UID installer peer lost query authority"));
            java.util.concurrent.atomic.AtomicReference<Throwable> failure = new java.util.concurrent.atomic.AtomicReference<>();
            Thread pending = new Thread(() -> {
                try {
                    withIdentity(UID + 10, PID + 30, () -> {
                        try {
                            h.policy.removeAllowlistedRestrictedPermission(NAME, READ_LOG, category, 0);
                            throw new AssertionError("in-flight retired installer mutation was accepted");
                        } catch (SecurityException expected) { }
                    });
                } catch (Throwable problem) { failure.set(problem); }
            }, "allowlist-retired-installer");
            pending.setDaemon(true);
            synchronized (h.pm) {
                pending.start();
                blockedAtMonitor(pending);
                // Replacement after authentication must not be adopted by the pending call.
                call(h.pm, "add", new Class<?>[]{ApkPackage.class},
                        pkg(shared.packageName, UID + 10, 36, (byte)9, group));
            }
            joined(pending, failure);
            check(h.policy.getAllowlistedRestrictedPermissions(NAME, category, 0).contains(READ_LOG),
                    "in-flight retired installer mutation changed permission state");
            withIdentity(UID + 10, PID + 30, () -> queryDenied(
                    () -> h.query.getAllowlistedRestrictedPermissions(NAME, category, 0),
                    "retired installer code borrowed its replacement's allowlist authority"));
            withIdentity(UID + 10, PID + 30, () -> queryDenied(
                    () -> h.query.removeAllowlistedRestrictedPermission(NAME, READ_LOG, category, 0),
                    "retired installer code removed a replacement's allowlist entry"));
            withIdentity(UID + 10, PID + 30, () -> queryDenied(
                    () -> h.query.addAllowlistedRestrictedPermission(NAME, READ_LOG, category, 0),
                    "retired installer code added a replacement's allowlist entry"));
        } finally {
            processes.remove(process[0]);
            call(h.owner, "setInstallSource", new Class<?>[]{type("PackageInstallSource")}, originalSource);
            for (String name : List.of(recorded.packageName, shared.packageName)) {
                h.policy.forgetPackage(name); call(h.pm, "remove", new Class<?>[]{String.class}, name);
            }
        }
    }
    private static void blockedAtMonitor(Thread thread) throws Exception {
        long until = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(5);
        while (thread.isAlive() && thread.getState() != Thread.State.BLOCKED && System.nanoTime() < until) {
            Thread.sleep(1);
        }
        check(thread.getState() == Thread.State.BLOCKED, "allowlist worker did not reach the held monitor");
    }
    private static void joined(Thread thread, java.util.concurrent.atomic.AtomicReference<Throwable> failure) throws Exception {
        thread.join(5000);
        check(!thread.isAlive(), "allowlist worker deadlocked");
        if (failure.get() != null) throw new AssertionError("allowlist worker failed", failure.get());
    }
    private static void concurrentAllowlists(Harness h) throws Exception {
        final int category = PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER;
        h.freshInstall(session(Set.of()));
        for (boolean add : new boolean[]{true, false}) {
            java.util.concurrent.atomic.AtomicReference<Throwable> failure = new java.util.concurrent.atomic.AtomicReference<>();
            List<Thread> workers = new ArrayList<>();
            for (String permission : List.of(READ_LOG, WRITE_LOG)) {
                Thread thread = new Thread(() -> {
                    try {
                        boolean changed = add ? h.policy.addAllowlistedRestrictedPermission(NAME, permission, category, 0)
                                : h.policy.removeAllowlistedRestrictedPermission(NAME, permission, category, 0);
                        if (!changed) throw new AssertionError("independent allowlist update reported no change");
                    } catch (Throwable problem) { failure.compareAndSet(null, problem); }
                }, "allowlist-" + permission);
                thread.setDaemon(true); workers.add(thread);
            }
            // The old implementation read both lists before blocking on PM to write,
            // deterministically losing the first update. The fixed implementation blocks
            // before reading, so the second update includes the first one's result.
            synchronized (h.pm) {
                for (Thread thread : workers) thread.start();
                for (Thread thread : workers) blockedAtMonitor(thread);
            }
            for (Thread thread : workers) joined(thread, failure);
            List<String> result = h.policy.getAllowlistedRestrictedPermissions(NAME, category, 0);
            check(add ? result != null && result.containsAll(List.of(READ_LOG, WRITE_LOG)) : result == null,
                    "concurrent independent allowlist updates overwrote one another");
        }
        h.flush();
    }

    private static void installerAllowlist(Harness h) throws Exception {
        final int installer = PackageManager.FLAG_PERMISSION_RESTRICTION_INSTALLER_EXEMPT;
        final int restricted = PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION;
        final int installerList = PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER;
        final String[] calls = CALL_LOG.toArray(new String[0]);
        PackageInstaller.SessionParams defaults = new PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL);

        // An explicit empty installer decision remains restricted. The real controller
        // must return denial without showing a dialog that cannot grant the permission.
        h.freshInstall(session(Set.of()));
        AtomicInteger prompts = new AtomicInteger();
        Reply blocked = h.request(choose(h, ALLOWED, prompts), calls);
        check(blocked.count == 1 && prompts.get() == 0, "empty allowlist showed futile call-log consent");
        for (int i = 0; i < calls.length; i++) {
            check(blocked.grant(i) == PackageManager.PERMISSION_DENIED && !h.policy.canRequest(NAME, calls[i])
                    && (h.flags(calls[i]) & restricted) != 0, "empty installer allowlist enabled " + calls[i]);
            check(h.pm.appOps().checkOperationRaw(AppOpsManager.permissionToOpCode(calls[i]), UID, NAME, null)
                    == AppOpsManager.MODE_IGNORED, "restricted call-log AppOp was allowed without consent");
        }

        // The framework's default session enables eligibility for the installed
        // manifest, while every modern runtime permission still requires consent.
        h.publishSession(defaults);
        List<String> listed = h.query.getAllowlistedRestrictedPermissions(NAME, installerList, 0);
        check(listed.containsAll(CALL_LOG) && !listed.contains(CAMERA),
                "installer allowlist query omitted restricted entries or included a normal runtime permission");
        for (String name : calls) {
            check(h.policy.canRequest(NAME, name) && (h.flags(name) & (installer | restricted)) == installer,
                    "default installer session left call-log permission unrequestable: " + name);
            check(h.policy.checkPermission(NAME, name) == PackageManager.PERMISSION_DENIED,
                    "installer eligibility granted a modern runtime permission without consent");
        }
        h.asApp(() -> {
            try {
                h.query.getAllowlistedRestrictedPermissions(NAME, installerList, 0);
                throw new AssertionError("an app without installer authority read the installer allowlist");
            } catch (SecurityException expected) { checks++; }
        });
        Reply approved = h.request(choose(h, ALLOWED, prompts), calls);
        check(approved.count == 1 && prompts.get() == 1, "call-log group did not produce one real controller decision");
        for (int i = 0; i < calls.length; i++) {
            check(approved.grant(i) == PackageManager.PERMISSION_GRANTED
                    && h.policy.checkPermission(NAME, calls[i]) == PackageManager.PERMISSION_GRANTED
                    && (h.flags(calls[i]) & USER_SET) != 0, "Allow did not reach the call-log callback and policy");
            check(h.pm.appOps().checkOperationRaw(AppOpsManager.permissionToOpCode(calls[i]), UID, NAME, null)
                    == AppOpsManager.MODE_ALLOWED, "call-log consent did not enable its derived AppOp");
        }
        allowlistQueries(h);
        h.publishSession(session(Set.of()));
        check(h.query.getAllowlistedRestrictedPermissions(NAME, installerList, 0).containsAll(CALL_LOG)
                && h.policy.checkPermission(NAME, READ_LOG) == PackageManager.PERMISSION_GRANTED,
                "an update's empty list revoked existing installer eligibility or consent");
        h.flush();
        PermissionPolicy restored = h.reopen();
        check(api(restored).getAllowlistedRestrictedPermissions(NAME, installerList, 0).containsAll(CALL_LOG)
                && restored.checkPermission(NAME, READ_LOG) == PackageManager.PERMISSION_GRANTED,
                "installer eligibility or explicit consent was lost after permission-store restart");
        int upgrade = PackageManager.FLAG_PERMISSION_RESTRICTION_UPGRADE_EXEMPT;
        int policyFixed = PackageManager.FLAG_PERMISSION_POLICY_FIXED;
        h.flags(READ_LOG, upgrade | policyFixed);
        h.publishSession(defaults);
        check(h.policy.checkPermission(NAME, READ_LOG) == PackageManager.PERMISSION_GRANTED
                && (h.flags(READ_LOG) & (installer | upgrade | restricted | policyFixed)) == (installer | upgrade | policyFixed),
                "installer eligibility discarded the existing upgrade exemption or fixed grant policy");

        // Only requested restricted permissions from an explicit subset can become
        // eligible. Ordinary publication/relaunch does not replace that decision.
        h.freshInstall(session(Set.of(READ_LOG, CAMERA, UNKNOWN, Manifest.permission.RECEIVE_SMS)));
        check(h.query.getAllowlistedRestrictedPermissions(NAME, installerList, 0).equals(List.of(READ_LOG)),
                "explicit subset included an unknown, nonrestricted or undeclared permission");
        call(h.pm, "publishPrepared", new Class<?>[]{ApkPackage.class, Object.class}, h.owner, h.pm);
        check(h.policy.canRequest(NAME, READ_LOG) && !h.policy.canRequest(NAME, WRITE_LOG)
                && !h.policy.canRequest(NAME, OUTGOING), "ordinary publication reset the installer subset");
        h.publishSession(session(null));
        check(h.policy.canRequest(NAME, READ_LOG) && !h.policy.canRequest(NAME, WRITE_LOG),
                "an explicit null list removed prior installer exemptions or became ALL");
        prompts.set(0);
        Object deny = choose(h, DENIED, prompts);
        Reply firstDenial = h.request(deny, READ_LOG);
        check(firstDenial.grant(0) == PackageManager.PERMISSION_DENIED && prompts.get() == 1
                && h.rationale(READ_LOG), "restricted permission denial did not preserve retry history");
        check(h.pm.appOps().checkOperationRaw(AppOpsManager.OP_READ_CALL_LOG, UID, NAME, null)
                == AppOpsManager.MODE_IGNORED, "call-log denial enabled its derived AppOp");
        h.request(deny, READ_LOG);
        check((h.flags(READ_LOG) & (USER_SET | USER_FIXED)) == (USER_SET | USER_FIXED),
                "repeated call-log denial did not become fixed");
        h.publishSession(defaults);
        Reply stillDenied = h.request(choose(h, ALLOWED, prompts), READ_LOG);
        check(stillDenied.grant(0) == PackageManager.PERMISSION_DENIED && prompts.get() == 2
                && !h.policy.canRequest(NAME, READ_LOG) && (h.flags(READ_LOG) & USER_FIXED) != 0,
                "installer eligibility overrode the user's fixed denial");
        check(h.policy.canRequest(NAME, WRITE_LOG) && h.policy.checkPermission(NAME, WRITE_LOG) == PackageManager.PERMISSION_DENIED,
                "update failed to add newly eligible permissions or granted them without consent");

        // A completed uninstall retires both consent and installer eligibility.
        // Null and empty are both explicit restrictions, not SessionParams.ALL.
        h.freshInstall(session(null));
        check(h.query.getAllowlistedRestrictedPermissions(NAME, installerList, 0) == null
                && !h.policy.canRequest(NAME, READ_LOG) && (h.flags(READ_LOG) & (USER_SET | USER_FIXED)) == 0,
                "reinstallation inherited the removed install's eligibility or user history");
        prompts.set(0);
        Reply removed = h.request(choose(h, ALLOWED, prompts), calls);
        check(prompts.get() == 0 && removed.grant(0) == PackageManager.PERMISSION_DENIED,
                "a fresh explicitly restricted installation reused an earlier grant");
        h.publishSession(session(PackageInstaller.SessionParams.RESTRICTED_PERMISSIONS_ALL));
        check(h.policy.canRequest(NAME, READ_LOG) && h.policy.checkPermission(NAME, READ_LOG) == PackageManager.PERMISSION_DENIED,
                "explicit ALL did not restore requestability without restoring a retired grant");
        h.reset(); h.flush();
    }

    private static int allowlist(Harness h, String pkg, int flags) {
        List<String> list = h.policy.getAllowlistedRestrictedPermissions(pkg, flags, 0);
        int result = 0;
        if (list != null) {
            if (list.contains(BACKGROUND)) result |= 1;
            if (list.contains(STORAGE)) result |= 2;
        }
        return result;
    }
    private static void rejects(Class<? extends Throwable> type, Checked action, String message) throws Exception {
        try { action.run(); }
        catch (Throwable failure) {
            Throwable cause = failure;
            while (cause instanceof java.lang.reflect.InvocationTargetException wrapped && wrapped.getCause() != null) cause = wrapped.getCause();
            if (type.isInstance(cause)) { checks++; return; }
            throw new AssertionError(message + ": " + cause, cause);
        }
        throw new AssertionError(message);
    }
    private static int flagsOf(PermissionPolicy policy, String pkg, String permission) {
        return policy.getPermissionFlags(pkg, permission, "default:0", 0);
    }
    /** A new APK with its own UID, published through PM like a completed install. */
    private static ApkPackage admit(Harness h, String name, int uid, java.util.Collection<String> allowlist,
            boolean recorded, boolean systemAddon) throws Exception {
        ApkPackage apk = pkg(name, uid, 36, (byte)9);
        if (recorded) {
            call(apk, "setRestrictedPermissionAllowlist", new Class<?>[]{java.util.Collection.class}, allowlist);
        }
        // Fixture boundary: an already verified microG system addon (no APK bytes here).
        if (systemAddon) set(apk, "mSystemAddon", true);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, apk);
        return apk;
    }
    private static void retire(Harness h, ApkPackage apk) throws Exception {
        h.policy.forgetPackage(apk.packageName);
        call(h.pm, "remove", new Class<?>[]{String.class}, apk.packageName);
    }
    /** AOSP install-time restriction exemptions, the boot migration and the allowlist API. */
    private static void exemptions(Harness h) throws Exception {
        // The Harness installed the owner with the installer allowlist (AOSP's default),
        // which exempts every requested restricted permission.
        check((h.flags(BACKGROUND) & (INSTALLER_EXEMPT | APPLY)) == INSTALLER_EXEMPT
                && (h.flags(STORAGE) & (INSTALLER_EXEMPT | APPLY)) == INSTALLER_EXEMPT,
                "installed package kept APPLY_RESTRICTION without an installer exemption");
        check((h.flags(CAMERA) & (ANY_EXEMPT | APPLY)) == 0, "unrestricted permission received a restriction flag");
        check(allowlist(h, NAME, PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER) == 3
                && allowlist(h, NAME, PackageManager.FLAG_PERMISSION_WHITELIST_SYSTEM) == 0,
                "installer allowlist does not report the admitted exemptions");
        check(h.state(BACKGROUND) == Context.PERMISSION_REQUEST_STATE_REQUESTABLE && h.policy.canRequest(NAME, BACKGROUND),
                "exempt hard-restricted permission remained unrequestable");
        check(h.policy.checkPermission(NAME, BACKGROUND) == DENY, "an exemption granted a permission without consent");

        // Session installs: AOSP's default allowlists everything; an explicit list
        // (pm install --restrict-permissions: empty) keeps the others restricted.
        ApkPackage all = admit(h, "fixture.permission.install.all", UID + 11, null, true, false);
        check((flagsOf(h.policy, all.packageName, BACKGROUND) & (INSTALLER_EXEMPT | APPLY)) == INSTALLER_EXEMPT
                && (flagsOf(h.policy, all.packageName, STORAGE) & (INSTALLER_EXEMPT | APPLY)) == INSTALLER_EXEMPT,
                "default install allowlist did not exempt restricted permissions");
        ApkPackage restricted = admit(h, "fixture.permission.install.restricted", UID + 12, List.of(), true, false);
        check((flagsOf(h.policy, restricted.packageName, BACKGROUND) & (ANY_EXEMPT | APPLY)) == APPLY
                && (flagsOf(h.policy, restricted.packageName, STORAGE) & (ANY_EXEMPT | APPLY)) == APPLY,
                "an install opting out of the allowlist lost its restriction");
        check(!h.policy.canRequest(restricted.packageName, BACKGROUND),
                "an opted-out hard-restricted permission became requestable");
        h.policy.grantRuntimePermission(restricted.packageName, BACKGROUND, "default:0", 0);
        check(h.policy.checkPermission(restricted.packageName, BACKGROUND) == DENY,
                "a restricted permission was granted without an exemption");
        ApkPackage partial = admit(h, "fixture.permission.install.partial", UID + 13, List.of(BACKGROUND), true, false);
        check((flagsOf(h.policy, partial.packageName, BACKGROUND) & (ANY_EXEMPT | APPLY)) == INSTALLER_EXEMPT
                && (flagsOf(h.policy, partial.packageName, STORAGE) & (ANY_EXEMPT | APPLY)) == APPLY,
                "an explicit allowlist was not applied exactly");
        // An update with an opt-out keeps existing exemptions (AOSP adds, never removes).
        ApkPackage update = pkg(partial.packageName, partial.uid(), 36, (byte)9);
        call(update, "setRestrictedPermissionAllowlist", new Class<?>[]{java.util.Collection.class}, List.of());
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, update);
        check((flagsOf(h.policy, partial.packageName, BACKGROUND) & (ANY_EXEMPT | APPLY)) == INSTALLER_EXEMPT,
                "an update's install request removed an existing exemption");
        // A verified system addon (microG) has the system exemption without any name rule.
        ApkPackage addon = admit(h, "fixture.permission.addon", UID + 14, List.of(), true, true);
        check((flagsOf(h.policy, addon.packageName, BACKGROUND) & (ANY_EXEMPT | APPLY)) == SYSTEM_EXEMPT,
                "system addon did not receive the system restriction exemption");
        check(allowlist(h, addon.packageName, PackageManager.FLAG_PERMISSION_WHITELIST_SYSTEM) == 3,
                "system allowlist does not report the system addon's exemptions");

        // The allowlist API: AOSP flag semantics, restoring the restriction on removal.
        check(h.policy.removeAllowlistedRestrictedPermission(all.packageName, BACKGROUND,
                PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER, 0), "system could not remove an allowlist entry");
        check((flagsOf(h.policy, all.packageName, BACKGROUND) & (ANY_EXEMPT | APPLY)) == APPLY
                && !h.policy.canRequest(all.packageName, BACKGROUND),
                "removing the last exemption did not restore APPLY_RESTRICTION");
        check(!h.policy.removeAllowlistedRestrictedPermission(all.packageName, BACKGROUND,
                PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER, 0), "removing an absent entry reported a change");
        check(h.policy.addAllowlistedRestrictedPermission(all.packageName, BACKGROUND,
                PackageManager.FLAG_PERMISSION_WHITELIST_UPGRADE, 0)
                && (flagsOf(h.policy, all.packageName, BACKGROUND) & (ANY_EXEMPT | APPLY))
                        == PackageManager.FLAG_PERMISSION_RESTRICTION_UPGRADE_EXEMPT,
                "upgrade allowlist did not record its own exemption");
        check(!h.policy.addAllowlistedRestrictedPermission(all.packageName, UNKNOWN,
                PackageManager.FLAG_PERMISSION_WHITELIST_UPGRADE, 0), "an unknown permission was allowlisted");
        rejects(IllegalArgumentException.class, () -> h.policy.addAllowlistedRestrictedPermission(all.packageName,
                CAMERA, PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER | PackageManager.FLAG_PERMISSION_WHITELIST_UPGRADE, 0),
                "a change to two allowlists at once was accepted");
        rejects(IllegalArgumentException.class, () -> h.policy.getAllowlistedRestrictedPermissions(NAME, 64, 0),
                "an unknown allowlist flag was accepted");
        check(h.policy.getAllowlistedRestrictedPermissions("missing.package", PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER, 0) == null,
                "a missing package reported an allowlist");
        // Caller checks: an app that is neither the installer of record nor holds
        // WHITELIST_RESTRICTED_PERMISSIONS cannot read or change any allowlist.
        h.asApp(() -> {
            IPermissionManager query = h.query;
            rejects(SecurityException.class, () -> query.getAllowlistedRestrictedPermissions(NAME,
                    PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER, 0), "an app read its installer allowlist");
            rejects(SecurityException.class, () -> query.getAllowlistedRestrictedPermissions(NAME,
                    PackageManager.FLAG_PERMISSION_WHITELIST_SYSTEM, 0), "an app read the system allowlist");
            rejects(SecurityException.class, () -> query.addAllowlistedRestrictedPermission(NAME, BACKGROUND,
                    PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER, 0), "an app allowlisted its own permission");
            rejects(SecurityException.class, () -> query.removeAllowlistedRestrictedPermission(NAME, STORAGE,
                    PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER, 0), "an app changed an immutably restricted permission");
        });
        check((h.flags(BACKGROUND) & INSTALLER_EXEMPT) != 0, "a rejected caller changed the allowlist");
        for (ApkPackage apk : List.of(all, restricted, partial, addon)) retire(h, apk);

        // One-time boot migration of a ledger written before exemptions existed: the
        // exemption is added, APPLY_RESTRICTION cleared and the user's decisions kept.
        h.flags(BACKGROUND, USER_SET | USER_FIXED | APPLY);
        h.flags(STORAGE, APPLY);
        h.policy.grantRuntimePermission(NAME, STORAGE, "default:0", 0);
        java.util.Properties ledger = (java.util.Properties)get(h.policy, "mDecisions");
        synchronized (h.policy) {
            check(ledger.remove("restriction-exemptions") != null, "migration version was not recorded");
            call(h.policy, "scheduleWrite", new Class<?>[0]);
        }
        h.flush();
        PermissionPolicy migrated = h.reopen();
        call(migrated, "initializePackages", new Class<?>[0]);
        check((Boolean)call(get(migrated, "mWriter"), "flush", new Class<?>[0]), "migration was not persisted");
        check(flagsOf(migrated, NAME, BACKGROUND) == (USER_SET | USER_FIXED | INSTALLER_EXEMPT),
                "migration lost a fixed user denial or kept APPLY_RESTRICTION: " + flagsOf(migrated, NAME, BACKGROUND));
        check(!migrated.canRequest(NAME, BACKGROUND) && migrated.checkPermission(NAME, BACKGROUND) == DENY,
                "migration overrode the user's fixed denial");
        check((flagsOf(migrated, NAME, STORAGE) & (INSTALLER_EXEMPT | APPLY)) == INSTALLER_EXEMPT
                && migrated.checkPermission(NAME, STORAGE) == GRANTED, "migration lost an explicit grant");
        // Versioned: later boots never add an exemption to a recorded restriction.
        migrated.updatePermissionFlags(NAME, BACKGROUND, PackageManager.MASK_PERMISSION_FLAGS_ALL, APPLY, true, "default:0", 0);
        call(migrated, "initializePackages", new Class<?>[0]);
        check(flagsOf(migrated, NAME, BACKGROUND) == APPLY, "the migration ran again on a later boot");
        check((Boolean)call(get(migrated, "mWriter"), "flush", new Class<?>[0]), "restored ledger flush failed");
        h.reset();
        h.flags(BACKGROUND, INSTALLER_EXEMPT);
        h.flags(STORAGE, INSTALLER_EXEMPT);
        h.flush();
    }
    private static int appOp(PermissionPolicy policy, int op, int uid) throws Exception {
        return (Integer)call(policy, "runtimeOpMode", new Class<?>[]{int.class, int.class}, op, uid);
    }
    /**
     * A legacy (target < 23) requester: AOSP restorePermissionState always grants its
     * permissions, restricted ones included, and expresses the restriction through the
     * AppOp; PermissionPolicyService leaves a REVIEW_REQUIRED permission's AppOp untouched and
     * gates the app through the review screen instead. Gesher has no review screen and
     * withholds the AppOp as the stand-in for that gate, so an exemption must never make a
     * legacy restricted permission effective (AppOp allowed) without the review cleared.
     */
    private static void legacyRestricted(Harness h) throws Exception {
        final int review = PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED;
        // A fresh install with AOSP's default allowlist (pm install, SessionParams).
        ApkPackage legacy = pkg("fixture.permission.legacy", UID + 15, 22, (byte)9);
        call(legacy, "setRestrictedPermissionAllowlist", new Class<?>[]{Collection.class}, (Object)null);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, legacy);
        String name = legacy.packageName;
        int uid = legacy.uid();
        for (int round = 0; round < 2; round++) {
            for (String restricted : List.of(BACKGROUND, STORAGE)) {
                int flags = flagsOf(h.policy, name, restricted);
                check(h.policy.checkPermission(name, restricted) == GRANTED
                        && (flags & (ANY_EXEMPT | APPLY | review)) == (INSTALLER_EXEMPT | review),
                        "a legacy app's exempt " + restricted + " lost its legacy grant or review: " + flags);
            }
            check(h.policy.checkPermission(name, FINE) == GRANTED && h.policy.checkPermission(name, CAMERA) == GRANTED,
                    "a legacy app lost its unrestricted compatibility grants");
            check(h.pm.appOps().checkOperationRaw(AppOpsManager.OP_FINE_LOCATION, uid, name, null) == AppOpsManager.MODE_FOREGROUND
                    && h.pm.appOps().checkOperationRaw(AppOpsManager.OP_READ_EXTERNAL_STORAGE, uid, name, null)
                            == AppOpsManager.MODE_IGNORED,
                    "an installer exemption gave a legacy app's unreviewed restricted permissions their AppOps");
            // The runtime grant API is a no-op for a legacy requester, and an update with
            // the default allowlist again adds an exemption without activating anything.
            h.policy.grantRuntimePermission(name, BACKGROUND, "default:0", 0);
            ApkPackage update = pkg(name, uid, 22, (byte)9);
            call(update, "setRestrictedPermissionAllowlist", new Class<?>[]{Collection.class}, (Object)null);
            call(h.pm, "add", new Class<?>[]{ApkPackage.class}, update);
        }
        // Only an explicit review (the system clearing REVIEW_REQUIRED) makes the grant effective.
        h.policy.updatePermissionFlags(name, BACKGROUND, review, 0, true, "default:0", 0);
        check(h.policy.checkPermission(name, BACKGROUND) == GRANTED
                && h.pm.appOps().checkOperationRaw(AppOpsManager.OP_FINE_LOCATION, uid, name, null) == AppOpsManager.MODE_ALLOWED
                && h.pm.appOps().checkOperationRaw(AppOpsManager.OP_READ_EXTERNAL_STORAGE, uid, name, null)
                        == AppOpsManager.MODE_IGNORED,
                "a reviewed, exempt legacy grant was not effective, or the review leaked to another permission");
        retire(h, legacy);

        // An install that opted out: the legacy grant stays, the restriction withholds the AppOp.
        ApkPackage old = pkg("fixture.permission.legacy.old", UID + 16, 22, (byte)9);
        call(old, "setRestrictedPermissionAllowlist", new Class<?>[]{Collection.class}, List.of());
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, old);
        ApkPackage modern = admit(h, "fixture.permission.optout", UID + 17, List.of(), true, false);
        check((flagsOf(h.policy, old.packageName, BACKGROUND) & (ANY_EXEMPT | APPLY | review)) == (APPLY | review)
                && h.policy.checkPermission(old.packageName, BACKGROUND) == GRANTED
                && h.pm.appOps().checkOperationRaw(AppOpsManager.OP_FINE_LOCATION, old.uid(), old.packageName, null)
                        == AppOpsManager.MODE_FOREGROUND,
                "a restricted legacy permission without an exemption lost its grant or supplied its AppOp");
        for (String restricted : List.of(BACKGROUND, STORAGE)) {
            Object row = reviewRow(h.policy, old, restricted);
            check(!(Boolean)call(row, "changeable", new Class<?>[0])
                    && !(Boolean)call(row, "enabled", new Class<?>[0])
                    && List.of(restricted).equals(call(row, "permissions", new Class<?>[0])),
                    "Unexempt legacy restriction appeared enabled or adjustable in permission review: " + restricted);
        }
        // Even a review made while the restriction applied does not survive dropping it: AOSP
        // restorePermissionState requires a new review for a legacy app's exempted permission.
        h.policy.updatePermissionFlags(old.packageName, BACKGROUND, review, 0, true, "default:0", 0);
        check(h.pm.appOps().checkOperationRaw(AppOpsManager.OP_FINE_LOCATION, old.uid(), old.packageName, null)
                        == AppOpsManager.MODE_FOREGROUND,
                "a reviewed but restricted legacy permission supplied its AppOp");
        // The one-time migration of a legacy app admitted before exemptions existed.
        java.util.Properties ledger = (java.util.Properties)get(h.policy, "mDecisions");
        synchronized (h.policy) {
            ledger.remove("restriction-exemptions");
            call(h.policy, "scheduleWrite", new Class<?>[0]);
        }
        h.flush();
        PermissionPolicy migrated = h.reopen();
        synchronized (h.policy) { ledger.setProperty("restriction-exemptions", "1"); }
        call(migrated, "initializePackages", new Class<?>[0]);
        for (String restricted : List.of(BACKGROUND, STORAGE)) {
            check(migrated.checkPermission(old.packageName, restricted) == GRANTED
                    && (flagsOf(migrated, old.packageName, restricted) & (ANY_EXEMPT | APPLY | review))
                            == (INSTALLER_EXEMPT | review),
                    "the migration's exemption dropped a legacy app's " + restricted + " grant or its review");
        }
        check(appOp(migrated, AppOpsManager.OP_FINE_LOCATION, old.uid()) == AppOpsManager.MODE_FOREGROUND
                && appOp(migrated, AppOpsManager.OP_READ_EXTERNAL_STORAGE, old.uid()) == AppOpsManager.MODE_IGNORED
                && migrated.checkPermission(old.packageName, FINE) == GRANTED,
                "the migration gave a legacy app restricted AppOps or removed its location grant");

        // After the migration, a restricted permission without recorded state (an install's
        // lost ledger write, here one that opted out) stays restricted at boot.
        java.util.Properties migratedLedger = (java.util.Properties)get(migrated, "mDecisions");
        synchronized (migrated) {
            check("1".equals(migratedLedger.getProperty("restriction-exemptions")), "migration version was not recorded");
            for (String restricted : List.of(BACKGROUND, STORAGE)) for (String prefix : List.of("uid:", "flags:", "legacy-grant:")) {
                migratedLedger.remove(prefix + modern.uid() + ":" + restricted);
            }
        }
        call(migrated, "initializePackages", new Class<?>[0]);
        check((flagsOf(migrated, modern.packageName, BACKGROUND) & (ANY_EXEMPT | APPLY)) == APPLY
                && (flagsOf(migrated, modern.packageName, STORAGE) & (ANY_EXEMPT | APPLY)) == APPLY
                && !migrated.canRequest(modern.packageName, BACKGROUND),
                "a boot outside the migration exempted a restricted permission without recorded state");
        check((Boolean)call(get(migrated, "mWriter"), "flush", new Class<?>[0]), "migrated ledger flush failed");
        retire(h, old);
        retire(h, modern);
        h.flush();
    }

    /** Legacy pre-launch review: logical group listing, confirm/toggle-off transitions, and AppOps. */
    private static void legacyReview(Harness h) throws Exception {
        final int review = PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED;
        final int revokedCompat = PackageManager.FLAG_PERMISSION_REVOKED_COMPAT;
        ApkPackage modern = admit(h, "fixture.permission.modern.noreview", UID + 21, List.of(), true, false);
        check(!(Boolean)call(h.policy, "isReviewRequired", new Class<?>[]{ApkPackage.class}, modern)
                && ((List<?>)call(h.policy, "pendingReviewGroups", new Class<?>[]{ApkPackage.class}, modern)).isEmpty(),
                "modern targetSdk >= 23 package required legacy permission review");
        retire(h, modern);

        ApkPackage legacy = pkg("fixture.permission.legacy.review", UID + 22, 22, (byte)9);
        call(legacy, "setRestrictedPermissionAllowlist", new Class<?>[]{Collection.class}, (Object)null);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, legacy);
        check((Boolean)call(h.policy, "isReviewRequired", new Class<?>[]{ApkPackage.class}, legacy),
                "newly admitted legacy package did not require permission review");
        List<?> groups = (List<?>)call(h.policy, "pendingReviewGroups", new Class<?>[]{ApkPackage.class}, legacy);
        List<String> groupNames = new ArrayList<>();
        for (Object group : groups) {
            groupNames.add((String)call(group, "groupName", new Class<?>[]{}));
        }
        check(groupNames.contains(Manifest.permission_group.LOCATION)
                && groupNames.contains(Manifest.permission_group.CAMERA)
                && groupNames.contains(Manifest.permission_group.STORAGE)
                && groupNames.size() < RUNTIME.size(),
                "legacy review groups were not collapsed into logical RuntimePermissionGroups");

        // Keep LOCATION and STORAGE enabled; turn CAMERA off.
        Set<String> enabled = new HashSet<>(groupNames);
        enabled.remove(Manifest.permission_group.CAMERA);
        call(h.policy, "recordPermissionReview", new Class<?>[]{ApkPackage.class, List.class, Set.class},
                legacy, groups, enabled);

        check(!(Boolean)call(h.policy, "isReviewRequired", new Class<?>[]{ApkPackage.class}, legacy)
                && ((List<?>)call(h.policy, "pendingReviewGroups", new Class<?>[]{ApkPackage.class}, legacy)).isEmpty(),
                "confirmed legacy review left REVIEW_REQUIRED pending");
        int camFlags = flagsOf(h.policy, legacy.packageName, CAMERA);
        check((camFlags & review) == 0 && (camFlags & (revokedCompat | USER_SET)) == (revokedCompat | USER_SET)
                && h.policy.checkPermission(legacy.packageName, CAMERA) == GRANTED
                && h.pm.appOps().checkOperationRaw(AppOpsManager.OP_CAMERA, legacy.uid(), legacy.packageName, null)
                        == AppOpsManager.MODE_IGNORED,
                "turned-off review group did not set REVOKED_COMPAT | USER_SET or deny its AppOp");
        int bgFlags = flagsOf(h.policy, legacy.packageName, BACKGROUND);
        int storageFlags = flagsOf(h.policy, legacy.packageName, STORAGE);
        check((bgFlags & (review | revokedCompat)) == 0 && (storageFlags & (review | revokedCompat)) == 0
                && h.pm.appOps().checkOperationRaw(AppOpsManager.OP_FINE_LOCATION, legacy.uid(), legacy.packageName, null)
                        == AppOpsManager.MODE_ALLOWED
                && h.pm.appOps().checkOperationRaw(AppOpsManager.OP_READ_EXTERNAL_STORAGE, legacy.uid(),
                        legacy.packageName, null) == AppOpsManager.MODE_ALLOWED,
                "enabled legacy review groups did not clear REVIEW_REQUIRED or publish AppOps");
        h.flush();
        PermissionPolicy restored = h.reopen();
        call(restored, "initializePackages", new Class<?>[0]);
        check(!(Boolean)call(restored, "isReviewRequired", new Class<?>[]{ApkPackage.class}, legacy)
                && flagsOf(restored, legacy.packageName, CAMERA) == camFlags
                && flagsOf(restored, legacy.packageName, BACKGROUND) == bgFlags
                && flagsOf(restored, legacy.packageName, STORAGE) == storageFlags
                && appOp(restored, AppOpsManager.OP_CAMERA, legacy.uid()) == AppOpsManager.MODE_IGNORED,
                "Permission review decisions did not survive ledger reload and package initialization");
        check((Boolean)call(get(restored, "mWriter"), "flush", new Class<?>[0]),
                "Reloaded permission review ledger did not flush");
        retire(h, legacy);
    }

    private interface ReviewChoice {
        int decide(String owner, String[] groups, boolean[] enabled, boolean[] changeable);
    }

    private static Object reviewRow(PermissionPolicy policy, ApkPackage owner, String permission) throws Exception {
        for (Object row : (List<?>)call(policy, "pendingReviewGroups", new Class<?>[]{ApkPackage.class}, owner)) {
            if (((List<?>)call(row, "permissions", new Class<?>[0])).contains(permission)) return row;
        }
        throw new AssertionError("Permission review omitted " + permission);
    }

    private static Object reviewController(Harness h, Executor ordinary, Executor review,
            ReviewChoice choice) throws Exception {
        Class<?> decisions = type("PermissionRequests$Decision");
        Object decision = Proxy.newProxyInstance(decisions.getClassLoader(), new Class<?>[]{decisions},
                (proxy, method, args) -> method.getName().equals("decideReview")
                        ? choice.decide((String)args[0], (String[])args[1], (boolean[])args[2], (boolean[])args[3]) : DENIED);
        return make(type("PermissionRequests"),
                new Class<?>[]{PackageManagerService.class, Executor.class, Executor.class, decisions},
                h.pm, ordinary, review, decision);
    }

    @SuppressWarnings("unchecked")
    private static java.util.concurrent.CompletableFuture<Boolean> review(Object controller,
            ApkPackage target) throws Exception {
        return (java.util.concurrent.CompletableFuture<Boolean>)call(controller, "reviewPermissions",
                new Class<?>[]{ApkPackage.class}, target);
    }

    /** The actual controller's queue and package-generation boundaries, without host dialogs. */
    private static void legacyReviewCoordinator(Harness h) throws Exception {
        h.reset();
        String name = "fixture.permission.legacy.coordinator";
        ApkPackage old = pkg(name, UID + 23, 22, (byte)11);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, old);
        List<Runnable> ordinary = new ArrayList<>(), reviews = new ArrayList<>();
        AtomicInteger prompts = new AtomicInteger();
        Object requests = reviewController(h, ordinary::add, reviews::add, (owner, groups, enabled, changeable) -> {
            check(name.equals(owner) && groups.length > 0, "review lost its captured package");
            prompts.incrementAndGet(); return ALLOWED;
        });
        var oldFuture = review(requests, old);
        check(oldFuture == review(requests, old) && reviews.size() == 1,
                "same-generation launches did not share the pending review");
        // Ordinary permission work must retain its existing serialized consent queue.
        Reply ordinaryReply = h.request(requests, CAMERA);
        check(ordinary.size() == 1 && reviews.size() == 1 && ordinaryReply.count == 0,
                "runtime consent was moved onto the legacy review executor");
        ordinary.remove(0).run();
        check(ordinaryReply.count == 1 && ordinaryReply.grant(0) == DENY,
                "ordinary permission consent did not finish independently of pending legacy review");

        ApkPackage replacement = pkg(name, old.uid(), 22, (byte)11);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, replacement);
        var newFuture = review(requests, replacement);
        check(newFuture != oldFuture && reviews.size() == 2,
                "replacement joined a review belonging to retired package code");
        reviews.remove(0).run();
        check(!oldFuture.get() && !newFuture.isDone() && prompts.get() == 0,
                "retired review was presented or completed the replacement's launch");
        reviews.remove(0).run();
        check(newFuture.get() && prompts.get() == 1
                && !(Boolean)call(h.policy, "isReviewRequired", new Class<?>[]{ApkPackage.class}, replacement),
                "replacement did not receive and commit its own review");
        retire(h, replacement);
        h.reset();
    }

    /** Managed grants/denials must be visible and immutable, including within a mixed group. */
    private static void legacyReviewManaged(Harness h) throws Exception {
        int reviewFlag = PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED;
        int revoked = PackageManager.FLAG_PERMISSION_REVOKED_COMPAT;
        int systemFixed = PackageManager.FLAG_PERMISSION_SYSTEM_FIXED;
        int policyFixed = PackageManager.FLAG_PERMISSION_POLICY_FIXED;
        ApkPackage legacy = pkg("fixture.permission.legacy.managed", UID + 24, 22, (byte)12);
        call(legacy, "setRestrictedPermissionAllowlist", new Class<?>[]{Collection.class}, (Object)null);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, legacy);
        h.policy.updatePermissionFlags(legacy.packageName, CAMERA, systemFixed, systemFixed, true, "default:0", 0);
        h.policy.updatePermissionFlags(legacy.packageName, AUDIO, policyFixed | revoked,
                policyFixed | revoked, true, "default:0", 0);
        // Fine location was already reviewed; new adjustable siblings must not
        // make this existing fixed grant disappear from the group's explanation.
        h.policy.updatePermissionFlags(legacy.packageName, FINE, policyFixed | reviewFlag,
                policyFixed, true, "default:0", 0);
        List<?> rows = (List<?>)call(h.policy, "pendingReviewGroups", new Class<?>[]{ApkPackage.class}, legacy);
        int fixedRows = 0, mixedRows = 0;
        for (Object row : rows) {
            List<?> permissions = (List<?>)call(row, "permissions", new Class<?>[0]);
            boolean changeable = (Boolean)call(row, "changeable", new Class<?>[0]);
            if (!changeable) {
                fixedRows++;
                check(permissions.size() == 1 && List.of(CAMERA, AUDIO, FINE).contains(permissions.get(0)),
                        "Managed permission was merged into an editable group");
                check((Boolean)call(row, "enabled", new Class<?>[0]) == !permissions.contains(AUDIO),
                        "Managed review row hid its enforced on/off state");
            } else if (Manifest.permission_group.LOCATION.equals(call(row, "groupName", new Class<?>[0]))) {
                mixedRows++;
                check(!permissions.contains(FINE) && permissions.contains(COARSE)
                        && (Boolean)call(row, "mixed", new Class<?>[0]),
                        "Mixed review group did not identify only its adjustable members");
            }
        }
        check(fixedRows == 3 && mixedRows == 1, "Review omitted managed or mixed-group rows");
        List<Runnable> queue = new ArrayList<>();
        Object requests = reviewController(h, queue::add, queue::add, (owner, labels, enabled, changeable) -> {
            int immutable = 0, mixed = 0;
            for (int i = 0; i < labels.length; i++) {
                if (!changeable[i]) {
                    immutable++;
                    check(enabled[i] == !labels[i].contains(AUDIO),
                            "Controller did not pass the immutable permission's current state");
                } else if (labels[i].contains(Manifest.permission_group.LOCATION)) {
                    mixed++;
                    check(labels[i].contains("adjustable permissions")
                            || labels[i].contains("הרשאות הניתנות לשינוי"),
                            "Mixed review group was presented as controlling its fixed members");
                }
            }
            check(immutable == 3 && mixed == 1, "Controller made managed rows editable or omitted mixed explanation");
            java.util.Arrays.fill(enabled, false);
            java.util.Arrays.fill(changeable, true); // A backend cannot override captured policy authority.
            return ALLOWED;
        });
        var future = review(requests, legacy); queue.remove(0).run();
        check(future.get() && !(Boolean)call(h.policy, "isReviewRequired", new Class<?>[]{ApkPackage.class}, legacy),
                "Confirmed managed review did not complete its reviewed state");
        check((flagsOf(h.policy, legacy.packageName, CAMERA) & (reviewFlag | revoked | systemFixed)) == systemFixed
                && (flagsOf(h.policy, legacy.packageName, AUDIO) & (reviewFlag | revoked | policyFixed))
                        == (revoked | policyFixed)
                && (flagsOf(h.policy, legacy.packageName, FINE) & (reviewFlag | revoked | policyFixed)) == policyFixed
                && (flagsOf(h.policy, legacy.packageName, COARSE) & (reviewFlag | revoked)) == revoked,
                "Managed choices changed fixed grants/denials or retained an unchecked adjustable sibling");
        check(appOp(h.policy, AppOpsManager.OP_CAMERA, legacy.uid()) != AppOpsManager.MODE_IGNORED
                && appOp(h.policy, AppOpsManager.OP_RECORD_AUDIO, legacy.uid()) == AppOpsManager.MODE_IGNORED,
                "Managed row states differed from enforced camera/microphone AppOps");
        retire(h, legacy);

        ApkPackage racing = pkg("fixture.permission.legacy.managedrace", UID + 25, 22, (byte)13);
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, racing);
        AtomicInteger attempts = new AtomicInteger();
        Object concurrent = reviewController(h, queue::add, queue::add, (owner, labels, enabled, changeable) -> {
            java.util.Arrays.fill(enabled, false);
            if (attempts.getAndIncrement() == 0) {
                // An administrator changes policy after this dialog's snapshot.
                h.policy.updatePermissionFlags(racing.packageName, CAMERA, policyFixed, policyFixed,
                        true, "default:0", 0);
            } else {
                boolean found = false;
                for (int i = 0; i < labels.length; i++) if (labels[i].contains(CAMERA)) {
                    found = true; check(!changeable[i], "Retried review did not expose the new managed permission");
                }
                check(found, "Retried managed review lost the camera permission");
            }
            return ALLOWED;
        });
        var obsolete = review(concurrent, racing); queue.remove(0).run();
        check(!obsolete.get() && (Boolean)call(h.policy, "isReviewRequired", new Class<?>[]{ApkPackage.class}, racing)
                && (flagsOf(h.policy, racing.packageName, CAMERA) & (reviewFlag | policyFixed | revoked))
                        == (reviewFlag | policyFixed),
                "Changed administrative policy silently committed a stale editable consent screen");
        var updated = review(concurrent, racing); queue.remove(0).run();
        check(updated.get() && attempts.get() == 2, "Fresh managed-state review could not complete after retry");
        retire(h, racing);
    }

    /** Restrictions are enforced states, not editable grants or fabricated user denials. */
    private static void legacyReviewRestricted(Harness h) throws Exception {
        int reviewFlag = PackageManager.FLAG_PERMISSION_REVIEW_REQUIRED;
        int revoked = PackageManager.FLAG_PERMISSION_REVOKED_COMPAT;
        List<String> restricted = List.of(BACKGROUND, STORAGE, READ_LOG, WRITE_LOG, OUTGOING);
        ApkPackage legacy = pkg("fixture.permission.legacy.reviewrestricted", UID + 26, 22, (byte)14);
        call(legacy, "setRestrictedPermissionAllowlist", new Class<?>[]{Collection.class}, List.of());
        call(h.pm, "add", new Class<?>[]{ApkPackage.class}, legacy);
        // Preserve an existing explicit denial on one restricted permission, and
        // the absence of any user decision on all the others.
        h.policy.updatePermissionFlags(legacy.packageName, STORAGE, revoked | USER_SET,
                revoked | USER_SET, true, "default:0", 0);
        h.policy.updatePermissionFlags(legacy.packageName, BACKGROUND, reviewFlag, 0, true, "default:0", 0);
        Object blockedSibling = reviewRow(h.policy, legacy, BACKGROUND);
        Object location = reviewRow(h.policy, legacy, FINE);
        check(!(Boolean)call(blockedSibling, "changeable", new Class<?>[0])
                && !(Boolean)call(blockedSibling, "enabled", new Class<?>[0])
                && (Boolean)call(location, "changeable", new Class<?>[0])
                && (Boolean)call(location, "mixed", new Class<?>[0])
                && !((List<?>)call(location, "permissions", new Class<?>[0])).contains(BACKGROUND),
                "Mixed review concealed a previously reviewed restriction or included it in the adjustable group");
        h.policy.updatePermissionFlags(legacy.packageName, BACKGROUND, reviewFlag, reviewFlag, true, "default:0", 0);
        Map<String, Integer> before = new java.util.LinkedHashMap<>();
        for (String permission : restricted) before.put(permission, flagsOf(h.policy, legacy.packageName, permission));
        List<Runnable> queue = new ArrayList<>();
        Object requests = reviewController(h, queue::add, queue::add, (owner, labels, enabled, changeable) -> {
            int enforced = 0;
            for (int i = 0; i < labels.length; i++) {
                if (!changeable[i]) {
                    enforced++;
                    check(!enabled[i] && restricted.stream().anyMatch(labels[i]::contains),
                            "Restricted review row was not visibly disabled with its own permission label");
                    enabled[i] = true; // Even an inconsistent backend cannot grant this row.
                } else {
                    enabled[i] = false;
                }
            }
            check(enforced == restricted.size(), "Review did not disclose each enforced restriction");
            return ALLOWED;
        });
        var accepted = review(requests, legacy); queue.remove(0).run();
        check(accepted.get() && !(Boolean)call(h.policy, "isReviewRequired", new Class<?>[]{ApkPackage.class}, legacy),
                "Acknowledging enforced restrictions did not complete review");
        for (String permission : restricted) {
            check(flagsOf(h.policy, legacy.packageName, permission) == (before.get(permission) & ~reviewFlag),
                    "Acknowledging an enforced restriction fabricated or erased a user choice: " + permission);
        }
        check(appOp(h.policy, AppOpsManager.OP_READ_EXTERNAL_STORAGE, legacy.uid()) == AppOpsManager.MODE_IGNORED
                && appOp(h.policy, AppOpsManager.OP_READ_CALL_LOG, legacy.uid()) == AppOpsManager.MODE_IGNORED,
                "Acknowledging restricted review enabled a withheld storage or call-log AppOp");
        retire(h, legacy);

        for (boolean exemptAfterPrompt : List.of(true, false)) {
            ApkPackage racing = pkg("fixture.permission.legacy.reviewexemption" + exemptAfterPrompt,
                    UID + (exemptAfterPrompt ? 27 : 28), 22, (byte)15);
            call(racing, "setRestrictedPermissionAllowlist", new Class<?>[]{Collection.class},
                    exemptAfterPrompt ? List.of() : List.of(BACKGROUND, STORAGE));
            call(h.pm, "add", new Class<?>[]{ApkPackage.class}, racing);
            AtomicInteger attempts = new AtomicInteger();
            Object concurrent = reviewController(h, queue::add, queue::add, (owner, labels, enabled, changeable) -> {
                if (attempts.getAndIncrement() == 0) {
                    for (String permission : List.of(BACKGROUND, STORAGE)) {
                        boolean changed = exemptAfterPrompt
                                ? h.policy.addAllowlistedRestrictedPermission(racing.packageName, permission,
                                        PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER, 0)
                                : h.policy.removeAllowlistedRestrictedPermission(racing.packageName, permission,
                                        PackageManager.FLAG_PERMISSION_WHITELIST_INSTALLER, 0);
                        check(changed, "Exemption race did not change the actual installer allowlist");
                    }
                }
                return ALLOWED;
            });
            int cameraBefore = flagsOf(h.policy, racing.packageName, CAMERA);
            var stale = review(concurrent, racing); queue.remove(0).run();
            check(!stale.get() && flagsOf(h.policy, racing.packageName, CAMERA) == cameraBefore
                    && (Boolean)call(h.policy, "isReviewRequired", new Class<?>[]{ApkPackage.class}, racing),
                    "Installer exemption change silently committed an obsolete permission-review screen");
            for (String permission : List.of(BACKGROUND, STORAGE)) {
                Object row = reviewRow(h.policy, racing, permission);
                check((Boolean)call(row, "changeable", new Class<?>[0]) == exemptAfterPrompt
                        && (Boolean)call(row, "enabled", new Class<?>[0]) == exemptAfterPrompt,
                        "Fresh review did not reflect the changed installer exemption: " + permission);
            }
            var retry = review(concurrent, racing); queue.remove(0).run();
            check(retry.get() && attempts.get() == 2,
                    "Fresh review could not proceed after the installer exemption changed");
            check(appOp(h.policy, AppOpsManager.OP_FINE_LOCATION, racing.uid())
                            == (exemptAfterPrompt ? AppOpsManager.MODE_ALLOWED : AppOpsManager.MODE_FOREGROUND)
                    && appOp(h.policy, AppOpsManager.OP_READ_EXTERNAL_STORAGE, racing.uid())
                            == (exemptAfterPrompt ? AppOpsManager.MODE_ALLOWED : AppOpsManager.MODE_IGNORED),
                    "Reviewed hard/legacy-soft restrictions disagreed with their current exemption");
            retire(h, racing);
        }
    }
    private static final class Prompts {
        final List<String> foreground = new ArrayList<>(), background = new ArrayList<>();
    }
    private static Object background(Harness h, Prompts prompts, int foregroundChoice, int backgroundChoice) throws Exception {
        return h.controller(Runnable::run, (owner, names) -> { prompts.foreground.add(names); return foregroundChoice; },
                (owner, names) -> { prompts.background.add(names); return backgroundChoice; });
    }
    private static void resetLocation(Harness h) throws Exception {
        h.reset();
        h.flags(BACKGROUND, INSTALLER_EXEMPT);
    }
    /** Android 16 BackgroundGrantBehavior with the separate background dialog. */
    private static void backgroundConsent(Harness h) throws Exception {
        // Target R+: foreground and background in one request reject everything, without UI.
        resetLocation(h);
        Prompts prompts = new Prompts();
        Reply combined = h.request(background(h, prompts, ALLOWED, ALWAYS), FINE, COARSE, BACKGROUND, CAMERA);
        check(prompts.foreground.isEmpty() && prompts.background.isEmpty() && combined.count == 1
                && combined.grant(0) == DENY && combined.grant(2) == DENY && combined.grant(3) == DENY,
                "target R+ combined foreground/background request showed UI or granted");
        check(h.flags(FINE) == 0 && (h.flags(BACKGROUND) & (USER_SET | USER_FIXED)) == 0,
                "a request rejected without UI recorded a user decision");
        // Background alone without its foreground: rejected without UI.
        Reply alone = h.request(background(h, prompts, ALLOWED, ALWAYS), BACKGROUND);
        check(prompts.background.isEmpty() && alone.grant(0) == DENY, "background without foreground was offered");
        // The foreground Allow never grants background access.
        Reply foreground = h.request(background(h, prompts, ALLOWED, ALWAYS), FINE, COARSE);
        check(prompts.foreground.size() == 1 && prompts.background.isEmpty() && foreground.grant(0) == GRANTED
                && h.policy.checkPermission(NAME, BACKGROUND) == DENY, "foreground Allow granted background access");
        check(h.pm.appOps().checkOperationRaw(AppOpsManager.OP_FINE_LOCATION, UID, NAME, null) == AppOpsManager.MODE_FOREGROUND,
                "foreground grant did not limit location to the foreground");
        // Target R+, background alone after the foreground: its own dialog (Settings on a phone).
        Reply always = h.request(background(h, prompts, DENIED, ALWAYS), BACKGROUND);
        check(prompts.foreground.size() == 1 && prompts.background.size() == 1
                && prompts.background.get(0).contains(BACKGROUND) && !prompts.background.get(0).contains(FINE),
                "background request did not show only the background dialog");
        check(always.grant(0) == GRANTED && (h.flags(BACKGROUND) & (USER_SET | USER_FIXED)) == USER_SET
                && h.pm.appOps().checkOperationRaw(AppOpsManager.OP_FINE_LOCATION, UID, NAME, null) == AppOpsManager.MODE_ALLOWED,
                "\"Allow all the time\" did not grant background location");
        // That choice is Android's Settings page (sendDirectlyToSettings): "Allow only while
        // using the app" leaves the ungranted background permission as it is, and returning
        // without a choice counts against the app: USER_SET, then USER_FIXED, then no dialog.
        final int decisionFlags = USER_SET | USER_FIXED;
        resetLocation(h);
        h.policy.grantRuntimePermission(NAME, FINE, "default:0", 0);
        prompts = new Prompts();
        Reply whileUsing = h.request(background(h, prompts, DENIED, FOREGROUND_ONLY), BACKGROUND);
        check(prompts.background.size() == 1 && whileUsing.grant(0) == DENY && h.flags(BACKGROUND) == INSTALLER_EXEMPT
                && h.policy.checkPermission(NAME, FINE) == GRANTED,
                "Settings' \"Allow only while using the app\" changed the ungranted background permission");
        h.request(background(h, prompts, DENIED, DISMISSED), BACKGROUND);
        check(prompts.background.size() == 2 && (h.flags(BACKGROUND) & decisionFlags) == USER_SET,
                "returning from the background choice without a decision was not counted");
        h.request(background(h, prompts, DENIED, DISMISSED), BACKGROUND);
        check(prompts.background.size() == 3 && (h.flags(BACKGROUND) & decisionFlags) == decisionFlags,
                "a repeated return without a decision did not fix the background permission");
        Reply looped = h.request(background(h, prompts, DENIED, ALWAYS), BACKGROUND);
        check(prompts.background.size() == 3 && looped.grant(0) == DENY
                && h.policy.checkPermission(NAME, FINE) == GRANTED,
                "an app reopened a background choice it had used up");
        // A dialog that could not be shown is not a return from Settings.
        resetLocation(h);
        h.policy.grantRuntimePermission(NAME, FINE, "default:0", 0);
        h.request(background(h, prompts, DENIED, -1), BACKGROUND);
        check(prompts.background.size() == 4 && h.flags(BACKGROUND) == INSTALLER_EXEMPT,
                "an unavailable background dialog was counted against the app");
        // Settings' "Don't allow" removes all location access, with USER_SET only.
        h.flags(BACKGROUND, INSTALLER_EXEMPT | USER_SET);
        Reply settingsDeny = h.request(background(h, prompts, DENIED, DENIED), BACKGROUND);
        check(settingsDeny.grant(0) == DENY && h.policy.checkPermission(NAME, FINE) == DENY
                && (h.flags(BACKGROUND) & decisionFlags) == USER_SET && (h.flags(FINE) & decisionFlags) == USER_SET
                && h.pm.appOps().checkOperationRaw(AppOpsManager.OP_FINE_LOCATION, UID, NAME, null) == AppOpsManager.MODE_IGNORED,
                "Settings' \"Don't allow\" fixed a decision or kept location access");

        // Target < R (microG targets 29): the foreground dialog, then the background dialog.
        h.install(pkg(NAME, UID, 29, (byte)7));
        resetLocation(h);
        prompts = new Prompts();
        Reply legacy = h.request(background(h, prompts, ALLOWED, FOREGROUND_ONLY), COARSE, FINE, BACKGROUND);
        check(prompts.foreground.size() == 1 && prompts.background.size() == 1
                && prompts.foreground.get(0).contains(FINE) && !prompts.foreground.get(0).contains(BACKGROUND),
                "target 29 did not ask foreground and background separately");
        check(legacy.grant(0) == GRANTED && legacy.grant(1) == GRANTED && legacy.grant(2) == DENY,
                "\"Allow only while using the app\" results are wrong");
        check((h.flags(BACKGROUND) & (USER_SET | USER_FIXED)) == USER_SET && (h.flags(FINE) & USER_FIXED) == 0,
                "\"Allow only while using the app\" did not record the first background decision");
        // UPGRADE_SETTINGS_LINK: foreground granted, the background is asked again; a
        // repeated refusal is fixed, and a fixed choice is never shown again.
        h.request(background(h, prompts, ALLOWED, FOREGROUND_ONLY), COARSE, FINE, BACKGROUND);
        check(prompts.foreground.size() == 1 && prompts.background.size() == 2
                && (h.flags(BACKGROUND) & (USER_SET | USER_FIXED)) == (USER_SET | USER_FIXED),
                "repeated background refusal was not fixed");
        h.request(background(h, prompts, ALLOWED, ALWAYS), BACKGROUND);
        check(prompts.background.size() == 2 && h.policy.checkPermission(NAME, BACKGROUND) == DENY,
                "a fixed background refusal was asked again");
        // A foreground denial leaves the background unasked.
        resetLocation(h);
        prompts = new Prompts();
        Reply denied = h.request(background(h, prompts, DENIED, ALWAYS), COARSE, FINE, BACKGROUND);
        check(prompts.foreground.size() == 1 && prompts.background.isEmpty() && denied.grant(2) == DENY
                && (h.flags(BACKGROUND) & USER_SET) == 0, "background was asked after a foreground denial");
        // A dismissed background dialog is not a decision.
        resetLocation(h);
        h.request(background(h, prompts, ALLOWED, DISMISSED), COARSE, FINE, BACKGROUND);
        check(h.policy.checkPermission(NAME, FINE) == GRANTED && h.flags(BACKGROUND) == INSTALLER_EXEMPT,
                "a dismissed background dialog recorded a decision");
        // "Don't allow" denies the whole group, each half with its own history.
        Reply deny = h.request(background(h, prompts, ALLOWED, DENIED), BACKGROUND);
        check(deny.grant(0) == DENY && h.policy.checkPermission(NAME, FINE) == DENY
                && h.policy.checkPermission(NAME, COARSE) == DENY
                && (h.flags(BACKGROUND) & (USER_SET | USER_FIXED)) == USER_SET
                && (h.flags(FINE) & (USER_SET | USER_FIXED)) == USER_SET,
                "\"Don't allow\" did not deny foreground and background");

        // microG's path: its own App info while a background choice is pending. The
        // dialog replaces the Control Center, and Settings returns after the choice.
        resetLocation(h);
        h.policy.grantRuntimePermission(NAME, FINE, "default:0", 0);
        List<Runnable> queue = new ArrayList<>();
        prompts = new Prompts();
        Prompts settingsPrompts = prompts;
        Object settings = h.controller(queue::add, (owner, names) -> DENIED,
                (owner, names) -> { settingsPrompts.background.add(names); return ALWAYS; });
        AtomicInteger returned = new AtomicInteger();
        boolean shown = (Boolean)call(settings, "backgroundSettings", new Class<?>[]{ApkPackage.class, Runnable.class},
                h.owner, (Runnable)returned::incrementAndGet);
        check(shown && returned.get() == 0 && queue.size() == 1,
                "App info did not wait for the background choice off the caller thread");
        queue.remove(0).run();
        check(returned.get() == 1 && prompts.background.size() == 1
                && h.policy.checkPermission(NAME, BACKGROUND) == GRANTED,
                "App info result was not delivered after the background choice");
        boolean again = (Boolean)call(settings, "backgroundSettings", new Class<?>[]{ApkPackage.class, Runnable.class},
                h.owner, (Runnable)returned::incrementAndGet);
        check(!again && queue.isEmpty() && returned.get() == 1,
                "App info without a pending background choice did not fall back to the Control Center");
        // App info follows the Settings page, not the request dialog (AOSP
        // AppPermissionViewModel): it still offers a background permission the user fixed
        // by refusing it twice, and its "Don't allow" revokes location without fixing.
        Class<?>[] settingsSignature = {ApkPackage.class, Runnable.class};
        resetLocation(h);
        h.policy.grantRuntimePermission(NAME, FINE, "default:0", 0);
        h.flags(BACKGROUND, INSTALLER_EXEMPT | USER_SET | USER_FIXED);
        Object pageDeny = h.controller(queue::add, (owner, names) -> DENIED,
                (owner, names) -> { settingsPrompts.background.add(names); return DENIED; });
        AtomicInteger denyReturned = new AtomicInteger();
        check((Boolean)call(pageDeny, "backgroundSettings", settingsSignature, h.owner, (Runnable)denyReturned::incrementAndGet)
                && queue.size() == 1, "App info did not offer a background permission the user had fixed");
        queue.remove(0).run();
        check(denyReturned.get() == 1 && h.policy.checkPermission(NAME, FINE) == DENY
                && h.policy.checkPermission(NAME, BACKGROUND) == DENY
                && (h.flags(BACKGROUND) & (USER_SET | USER_FIXED)) == USER_SET
                && (h.flags(FINE) & (USER_SET | USER_FIXED)) == USER_SET,
                "App info's \"Don't allow\" fixed a decision or kept location access");
        // "Allow only while using the app" there revokes a fixed background permission again
        // with userFixed=false (AOSP requestChange: GRANT_FOREGROUND_ONLY on a USER_FIXED
        // background): USER_SET only, foreground kept. Closing it records nothing.
        for (int choice : new int[]{FOREGROUND_ONLY, DISMISSED}) {
            h.policy.grantRuntimePermission(NAME, FINE, "default:0", 0);
            h.flags(BACKGROUND, INSTALLER_EXEMPT | USER_SET | USER_FIXED);
            Object page = h.controller(queue::add, (owner, names) -> DENIED, (owner, names) -> choice);
            AtomicInteger pageReturned = new AtomicInteger();
            check((Boolean)call(page, "backgroundSettings", settingsSignature, h.owner, (Runnable)pageReturned::incrementAndGet)
                    && queue.size() == 1, "App info did not offer the background choice");
            queue.remove(0).run();
            int expected = INSTALLER_EXEMPT | USER_SET | (choice == DISMISSED ? USER_FIXED : 0);
            check(pageReturned.get() == 1 && h.flags(BACKGROUND) == expected
                    && h.policy.checkPermission(NAME, FINE) == GRANTED && h.policy.checkPermission(NAME, BACKGROUND) == DENY,
                    "App info's background choice left the wrong state: " + choice + " flags " + h.flags(BACKGROUND));
        }
        // Policy or system fixed background permissions are not offered there.
        for (int fixed : new int[]{PackageManager.FLAG_PERMISSION_POLICY_FIXED, PackageManager.FLAG_PERMISSION_SYSTEM_FIXED}) {
            h.flags(BACKGROUND, INSTALLER_EXEMPT | fixed);
            check(!(Boolean)call(settings, "backgroundSettings", settingsSignature, h.owner, (Runnable)returned::incrementAndGet)
                    && queue.isEmpty(), "App info offered a policy or system fixed background permission");
        }
        appInfo(h);
        h.install(pkg(NAME, UID, 36, (byte)7));
        h.reset(); h.flush();
    }
    /**
     * The production activity starter: an app's own App info with a pending background
     * choice shows the background dialog instead of the Control Center, and its
     * startActivityForResult result arrives only after the choice.
     */
    @SuppressWarnings("unchecked")
    private static void appInfo(Harness h) throws Exception {
        resetLocation(h);
        h.policy.grantRuntimePermission(NAME, FINE, "default:0", 0);
        Class<?> recordType = type("ActivityTaskManagerService$ActivityRecord");
        android.content.pm.ActivityInfo info = new android.content.pm.ActivityInfo();
        info.packageName = NAME; info.name = NAME + ".AskPermissionActivity";
        Object activity = make(recordType, new Class<?>[]{h.atm.getClass(), android.content.pm.ActivityInfo.class,
                Intent.class, int.class, int.class, String.class, recordType, String.class, int.class},
                h.atm, info, new Intent(), 1, PID, NAME, null, null, 0);
        set(activity, "app", h.process);
        ((List<Object>)get(h.atm, "mActivities")).add(activity);
        ((List<Object>)get(h.process, "activities")).add(activity);
        List<Runnable> queue = new ArrayList<>();
        Prompts prompts = new Prompts();
        Object requests = h.controller(queue::add, (owner, names) -> DENIED,
                (owner, names) -> { prompts.background.add(names); return ALWAYS; });
        Field requestsField = field(h.atm.getClass(), "mPermissionRequests");
        Object previousRequests = requestsField.get(h.atm);
        requestsField.set(h.atm, requests);
        Field center = field(type("ControlCenter"), "sOpen");
        Object previousCenter = center.get(null);
        List<String> centers = new ArrayList<>();
        center.set(null, (java.util.function.Predicate<String>)pkg -> centers.add(String.valueOf(pkg)));
        // App info for a result opens the Control Center with a SettingsResults token and
        // answers when the details close (the special app access switches live there).
        Field centerForResult = field(type("ControlCenter"), "sOpenForResult");
        Object previousCenterForResult = centerForResult.get(null);
        List<String> tokens = new ArrayList<>();
        centerForResult.set(null, java.lang.reflect.Proxy.newProxyInstance(centerForResult.getType().getClassLoader(),
                new Class<?>[]{centerForResult.getType()}, (proxy, called, args) -> {
                    if (called.getDeclaringClass() == Object.class) {
                        return called.getName().equals("equals") ? (Object)(proxy == args[0])
                                : called.getName().equals("hashCode") ? (Object)System.identityHashCode(proxy) : "opener";
                    }
                    centers.add(String.valueOf(args[0])); tokens.add((String)args[1]); return true;
                }));
        Method start = null;
        for (Method method : h.atm.getClass().getDeclaredMethods()) {
            if (method.getName().equals("start") && method.getParameterCount() == 8) start = method;
        }
        start.setAccessible(true);
        Method starter = start;
        IApplicationThread thread = (IApplicationThread)get(h.process, "thread");
        IBinder token = (IBinder)get(activity, "token");
        List<Object> pending = (List<Object>)get(activity, "pendingResults");
        Intent details = new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                android.net.Uri.fromParts("package", NAME, null));
        try {
            Object[] status = new Object[1];
            h.asApp(() -> status[0] = starter.invoke(h.atm, thread, details, null, NAME, token, null, 121, null));
            check(Integer.valueOf(android.app.ActivityManager.START_SUCCESS).equals(status[0]) && centers.isEmpty()
                    && queue.size() == 1 && pending.isEmpty(),
                    "App info with a pending background choice opened the Control Center or returned at once");
            queue.remove(0).run();
            check(prompts.background.size() == 1 && h.policy.checkPermission(NAME, BACKGROUND) == GRANTED
                    && pending.size() == 1, "App info did not return its result after the background choice");
            h.asApp(() -> status[0] = starter.invoke(h.atm, thread, details, null, NAME, token, null, 122, null));
            check(Integer.valueOf(android.app.ActivityManager.START_SUCCESS).equals(status[0])
                    && centers.equals(List.of(NAME)) && tokens.size() == 1 && queue.isEmpty() && pending.size() == 1,
                    "App info without a pending choice did not keep opening the Control Center");
            check(h.atm.getClass().getMethod("settingsScreensClosed", String[].class)
                        .invoke(h.atm, (Object)tokens.toArray(new String[0])).equals(1) && pending.size() == 2,
                    "App info in the Control Center did not return its result when the details closed");
        } finally {
            center.set(null, previousCenter);
            centerForResult.set(null, previousCenterForResult);
            requestsField.set(h.atm, previousRequests);
            ((List<Object>)get(h.atm, "mActivities")).remove(activity);
            ((List<Object>)get(h.process, "activities")).remove(activity);
        }
    }
    public static int run(boolean binderTransport) throws Exception {
        binder = binderTransport; checks = 0;
        Harness harness = new Harness();
        exemptions(harness); legacyRestricted(harness); legacyReview(harness); legacyReviewCoordinator(harness);
        legacyReviewManaged(harness); legacyReviewRestricted(harness);
        decisions(harness); groups(harness); exclusions(harness); lifecycle(harness);
        backgroundConsent(harness);
        installerAllowlist(harness);
        concurrentAllowlists(harness);

        return checks;
    }
    public static void main(String[] args) throws Exception {
        int total = run(true);
        System.out.println("RuntimePermissionStateTest all passed: " + total + " checks; binder=true");
    }
}
