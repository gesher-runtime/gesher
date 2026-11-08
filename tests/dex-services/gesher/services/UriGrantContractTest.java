/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.Intent;
import android.content.UriPermission;
import android.content.pm.PackageManager;
import android.content.pm.PathPermission;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.PatternMatcher;
import android.os.RemoteException;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedPermissionImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import com.gesher.server.ActivityManagerService;
import com.gesher.server.ActivityTaskManagerService;
import com.gesher.server.ApkPackage;
import com.gesher.server.CallerIdentity;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
import com.gesher.server.StandIn;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import sun.misc.Unsafe;

/** Actual grant, provider and permission-policy code; constructed parsed manifest state. */
public final class UriGrantContractTest {
    private static final String OWNER = "fixture.uri.owner", DELEGATE = "fixture.uri.delegate";
    private static final String BOTH = "fixture.uri.both", CLIENT = "fixture.uri.client";
    private static final String OTHER = "fixture.uri.other", COLLISION = "fixture.uri.collision";
    private static final String READ_PERMISSION = "fixture.uri.permission.READ";
    private static final String WRITE_PERMISSION = "fixture.uri.permission.WRITE";
    private static final String BLOCKED_PERMISSION = "fixture.uri.permission.BLOCKED";
    private static final int READ = Intent.FLAG_GRANT_READ_URI_PERMISSION;
    private static final int WRITE = Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private static final int PREFIX = Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
    private static Unsafe unsafe;
    private static boolean binder;
    private static int checks;

    private interface Operation { void run() throws Exception; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }
    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method method = type.getDeclaredMethod(name, parameters); method.setAccessible(true); return method;
    }
    private static Object call(Object target, String name, Class<?>[] parameters, Object... args)
            throws Exception {
        try { return method(target.getClass(), name, parameters).invoke(target, args); }
        catch (InvocationTargetException wrapped) {
            Throwable cause = wrapped.getCause();
            if (cause instanceof Exception) throw (Exception) cause;
            if (cause instanceof Error) throw (Error) cause;
            throw wrapped;
        }
    }
    private static Uri uri(String authority, String path) {
        return Uri.parse("content://fixture.uri." + authority + path);
    }
    private static ParsedProviderImpl provider(String name, boolean exported, boolean grants,
            String read, String write) {
        ParsedProviderImpl provider = new ParsedProviderImpl();
        provider.setName(OWNER + "." + name);
        provider.setPackageName(OWNER);
        provider.setEnabled(true);
        provider.setExported(exported);
        provider.setAuthority("fixture.uri." + name);
        provider.setReadPermission(read);
        provider.setWritePermission(write);
        provider.setGrantUriPermissions(grants);
        return provider;
    }
    private static ApkPackage record(String name, int uid, String... requested) throws Exception {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setTargetSdkVersion(36);
        parsed.setEnabled(true);
        for (String permission : requested) {
            parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0, java.util.Collections.emptySet()));
        }
        if (OWNER.equals(name)) {
            for (String nameOfPermission : new String[]{READ_PERMISSION, WRITE_PERMISSION, BLOCKED_PERMISSION}) {
                ParsedPermissionImpl permission = new ParsedPermissionImpl();
                permission.setName(nameOfPermission);
                permission.setPackageName(OWNER);
                permission.setProtectionLevel(PermissionInfo.PROTECTION_NORMAL);
                parsed.addPermission(permission);
            }
            parsed.addProvider(provider("private", false, true, READ_PERMISSION, WRITE_PERMISSION));
            parsed.addProvider(provider("protected", true, true, READ_PERMISSION, WRITE_PERMISSION));
            parsed.addProvider(provider("noGrants", true, false, READ_PERMISSION, WRITE_PERMISSION));
            ParsedProviderImpl path = provider("path", true, true, BLOCKED_PERMISSION, BLOCKED_PERMISSION);
            path.addPathPermission(new PathPermission("/allowed", PatternMatcher.PATTERN_PREFIX,
                    READ_PERMISSION, WRITE_PERMISSION));
            parsed.addProvider(path);
            for (boolean broad : new boolean[]{false, true}) {
                ParsedProviderImpl limited = provider(broad ? "limited" : "patternOnly", true,
                        broad, READ_PERMISSION, WRITE_PERMISSION);
                limited.addUriPermissionPattern(new PatternMatcher("/allowed", PatternMatcher.PATTERN_PREFIX));
                parsed.addProvider(limited);
            }
            ParsedProviderImpl dynamic = provider("dynamic", true, true, READ_PERMISSION, WRITE_PERMISSION);
            dynamic.setForceUriPermissions(true);
            parsed.addProvider(dynamic);
        }
        parsed.hideAsFinal();
        ApkPackage record = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        set(record, "packageName", name); set(record, "parsed", parsed);
        set(record, "apkPath", "/fixture/" + name + "/base.apk"); set(record, "mUid", uid);
        set(record, "mDataDir", "/fixture/" + name + "/data");
        set(record, "sharedLibraries", SharedLibraryGraph.EMPTY);
        return record;
    }
    private static IBinder token() {
        if (binder) return new Binder();
        return (IBinder) Proxy.newProxyInstance(IBinder.class.getClassLoader(), new Class<?>[]{IBinder.class},
                (self, method, args) -> {
                    switch (method.getName()) {
                        case "equals": return self == args[0];
                        case "hashCode": return System.identityHashCode(self);
                        case "toString": return "Host application token";
                        default: throw new AssertionError("Unexpected host Binder operation: " + method.getName());
                    }
                });
    }
    private static final class ThreadToken extends IApplicationThread.Default {
        private final IBinder token = token();
        @Override public IBinder asBinder() { return token; }
    }
    private static final class Harness {
        final Map<String, ApkPackage> records = new LinkedHashMap<>();
        final Map<String, ThreadToken> threads = new LinkedHashMap<>();
        final Map<String, Integer> pids = new LinkedHashMap<>();
        final PackageManagerService pm;
        final ActivityTaskManagerService atm;
        final ActivityManagerService service;
        final IActivityManager am;
        final Object grants, providers;

        Harness() throws Exception {
            records.put(OWNER, record(OWNER, 12101));
            records.put(DELEGATE, record(DELEGATE, 12102, READ_PERMISSION));
            records.put(BOTH, record(BOTH, 12103, READ_PERMISSION, WRITE_PERMISSION));
            records.put(CLIENT, record(CLIENT, 12104));
            records.put(OTHER, record(OTHER, 12105));
            records.put(COLLISION, record(COLLISION, 12101));
            if (binder) {
                pm = new PackageManagerService(records.values());
            } else {
                // Omit only the unrelated PackageInstaller Binder construction.
                // Provider projection and the complete permission policy stay real.
                pm = (PackageManagerService) unsafe.allocateInstance(PackageManagerService.class);
                set(pm, "mPackages", new LinkedHashMap<>(records));
                set(pm, "mRemoving", new LinkedHashMap<String, ApkPackage>());
                set(pm, "mPackageRemoval", new ArrayList<>());
                Class<?> policy = Class.forName("com.gesher.server.PermissionPolicy");
                Constructor<?> policyConstructor = policy.getDeclaredConstructor(PackageManagerService.class);
                policyConstructor.setAccessible(true);
                set(pm, "mPermissions", policyConstructor.newInstance(pm));
            }
            atm = (ActivityTaskManagerService) unsafe.allocateInstance(ActivityTaskManagerService.class);
            set(atm, "mPm", pm);
            List<Object> processes = new ArrayList<>();
            set(atm, "mProcesses", processes);
            Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
            int pid = 871001;
            for (String name : records.keySet()) {
                ThreadToken thread = new ThreadToken(); threads.put(name, thread); pids.put(name, pid);
                Object process = unsafe.allocateInstance(processType);
                set(process, "pid", pid++); set(process, "uid", records.get(name).uid());
                set(process, "thread", thread); set(process, "packageName", name);
                set(process, "processName", name); set(process, "ready", true);
                processes.add(process);
            }
            Class<?> grantsType = Class.forName("com.gesher.server.UriGrants");
            Constructor<?> constructor = grantsType.getDeclaredConstructor(PackageManagerService.class,
                    ActivityTaskManagerService.class);
            constructor.setAccessible(true); grants = constructor.newInstance(pm, atm);
            providers = unsafe.allocateInstance(Class.forName("com.gesher.server.ContentProviders"));
            set(providers, "mPm", pm); set(providers, "mUriGrants", grants);
            service = (ActivityManagerService) unsafe.allocateInstance(ActivityManagerService.class);
            set(service, "mPm", pm); set(service, "mAtm", atm); set(service, "mProviders", providers);
            set(service, "mCallers", new CallerIdentity(pm, atm));
            if (binder) {
                StandIn.setStrict(true);
                IBinder target = StandIn.of(IActivityManager.class, service).asBinder();
                am = IActivityManager.Stub.asInterface(new Binder() {
                    @Override public IInterface queryLocalInterface(String descriptor) { return null; }
                    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                            throws RemoteException { return target.transact(code, data, reply, flags); }
                });
            } else am = service;
        }
        void as(String name, Operation operation) throws Exception {
            long previous = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) records.get(name).uid() << 32) | pids.get(name));
                if (Binder.getCallingPid() != pids.get(name)
                        || Binder.getCallingUid() != records.get(name).uid()) {
                    throw new AssertionError("Controlled caller identity unavailable");
                }
                operation.run();
            } finally { Binder.restoreCallingIdentity(previous); }
        }
        void grant(String sender, String target, Uri uri, int modes) throws Exception {
            as(sender, () -> am.grantUriPermission(threads.get(sender), target, uri, modes, 0));
        }
        void revoke(String sender, String target, Uri uri, int modes) throws Exception {
            as(sender, () -> am.revokeUriPermission(threads.get(sender), target, uri, modes, 0));
        }
        void access(String target, Uri uri, int modes, boolean expected) throws Exception {
            as(target, () -> check((am.checkContentUriPermissionFull(uri, pids.get(target),
                    records.get(target).uid(), modes, 0) == PackageManager.PERMISSION_GRANTED) == expected,
                    "Wrong access for " + target + " to " + uri + " modes=" + modes));
        }
        void explicit(String target, Uri uri, int modes, boolean expected) throws Exception {
            as(target, () -> check((am.checkUriPermission(uri, pids.get(target),
                    records.get(target).uid(), modes, 0, null) == PackageManager.PERMISSION_GRANTED) == expected,
                    "Wrong explicit grant for " + target + " to " + uri + " modes=" + modes));
        }
        ProviderInfo provider(Uri uri) throws Exception {
            Method resolve = method(providers.getClass(), "declaredProvider", PackageManagerService.class, String.class);
            return (ProviderInfo) resolve.invoke(null, pm, uri.getAuthority());
        }
        void acquire(String target, Uri uri) throws Exception {
            call(providers, "enforceAccess", new Class<?>[]{String.class, ProviderInfo.class}, target, provider(uri));
            check(true, "Provider acquisition policy accepted the real grant");
        }
        @SuppressWarnings("unchecked") List<UriPermission> listed(String target) throws Exception {
            return (List<UriPermission>) call(grants, "list",
                    new Class<?>[]{String.class, boolean.class, boolean.class}, target, true, false);
        }
        String snapshot() throws Exception {
            StringBuilder text = new StringBuilder();
            for (Object grant : (List<?>) field(grants.getClass(), "mGrants").get(grants)) {
                for (String key : new String[]{"owner", "target", "uri", "modes", "prefix"}) {
                    Object value = field(grant.getClass(), key).get(grant);
                    text.append(value instanceof ApkPackage ? ((ApkPackage) value).packageName : value).append('|');
                }
            }
            return text.toString();
        }
        void rejected(Class<? extends Throwable> type, Operation operation) throws Exception {
            String before = snapshot();
            try { operation.run(); throw new AssertionError("Invalid operation succeeded"); }
            catch (Throwable failure) {
                if (!type.isInstance(failure)) throw failure;
                check(true, "Expected rejection");
            }
            check(before.equals(snapshot()), "Rejected operation mutated existing grants");
        }
        @SuppressWarnings("unchecked") void replace(String name) throws Exception {
            ApkPackage previous = records.get(name);
            ApkPackage replacement = record(name, previous.uid(),
                    previous.parsed.getRequestedPermissions().toArray(new String[0]));
            Map<String, ApkPackage> current = new LinkedHashMap<>((Map<String, ApkPackage>) field(pm.getClass(), "mPackages").get(pm));
            current.put(name, replacement);
            synchronized (pm) { set(pm, "mPackages", current); }
            records.put(name, replacement);
        }
    }

    private static void delegation() throws Exception {
        Harness h = new Harness(); Uri protectedUri = uri("protected", "/item");
        h.access(DELEGATE, protectedUri, READ, true);
        h.explicit(DELEGATE, protectedUri, READ, false);
        h.access(CLIENT, protectedUri, READ, false);
        h.grant(DELEGATE, CLIENT, protectedUri, READ);
        h.explicit(CLIENT, protectedUri, READ, true); h.access(CLIENT, protectedUri, READ, true);
        h.access(CLIENT, protectedUri, WRITE, false); h.acquire(CLIENT, protectedUri);
        h.rejected(SecurityException.class, () -> h.grant(DELEGATE, OTHER, protectedUri, WRITE));
        h.rejected(SecurityException.class, () -> h.grant(DELEGATE, OTHER, protectedUri, READ | WRITE));
        h.grant(BOTH, OTHER, protectedUri, READ | WRITE);
        h.access(OTHER, protectedUri, READ | WRITE, true);
        Uri path = uri("path", "/allowed/item");
        h.access(DELEGATE, path, READ, true); h.grant(DELEGATE, CLIENT, path, READ);
        h.access(CLIENT, path, READ, true);
        h.rejected(SecurityException.class, () -> h.grant(DELEGATE, CLIENT, uri("path", "/outside"), READ));
        Uri privateUri = uri("private", "/item");
        h.rejected(SecurityException.class, () -> h.grant(BOTH, CLIENT, privateUri, READ));
        h.rejected(SecurityException.class, () -> h.grant(COLLISION, CLIENT, privateUri, READ));
        h.grant(OWNER, CLIENT, privateUri, READ); h.acquire(CLIENT, privateUri);
        h.grant(CLIENT, OTHER, privateUri, READ); h.access(OTHER, privateUri, READ, true);
        h.rejected(SecurityException.class, () -> h.grant(CLIENT, OTHER, privateUri, WRITE));
        String before = h.snapshot();
        h.grant(CLIENT, DELEGATE, uri("noGrants", "/item"), READ);
        check(before.equals(h.snapshot()), "Already-authorized basic target acquired a needless grant");
        h.rejected(SecurityException.class, () -> h.grant(CLIENT, DELEGATE, uri("noGrants", "/item"), READ | PREFIX));
        h.rejected(SecurityException.class, () -> h.grant(OWNER, CLIENT, uri("noGrants", "/item"), READ));
        Uri dynamic = uri("dynamic", "/item");
        h.rejected(SecurityException.class, () -> h.grant(DELEGATE, CLIENT, dynamic, READ));
        h.rejected(SecurityException.class, () -> h.grant(CLIENT, DELEGATE, dynamic, READ));
        h.grant(OWNER, CLIENT, dynamic, READ); h.grant(CLIENT, OTHER, dynamic, READ);
        h.access(OTHER, dynamic, READ, true);
    }
    private static void identity() throws Exception {
        Harness h = new Harness(); Uri exact = uri("private", "/item?record=1#part");
        h.grant(OWNER, CLIENT, exact, READ);
        h.access(CLIENT, exact, READ, true);
        h.access(CLIENT, uri("private", "/item?record=2#part"), READ, false);
        h.access(CLIENT, uri("private", "/item?record=1#other"), READ, false);
        h.access(CLIENT, uri("private", "/item?record=1"), READ, false);
        h.access(CLIENT, uri("private", "/item"), READ, false);
        check(h.listed(CLIENT).get(0).getUri().equals(exact), "Listed exact URI lost query or fragment");
        h.grant(OWNER, OTHER, Uri.parse("content://0@fixture.uri.private/item?record=1#part"), READ);
        h.access(OTHER, exact, READ, true);
        h.access(OTHER, Uri.parse("content://0@fixture.uri.private/item?record=1#part"), READ, true);
        h.access(OTHER, Uri.parse("content://10@fixture.uri.private/item?record=1#part"), READ, false);
        h.rejected(SecurityException.class, () -> h.grant(CLIENT, OTHER, exact, READ | PREFIX));
        h.grant(OWNER, CLIENT, uri("private", "/tree?version=1#root"), READ | PREFIX);
        h.access(CLIENT, uri("private", "/tree/child?version=2#leaf"), READ, true);
        h.access(CLIENT, uri("private", "/treehouse/child"), READ, false);
        h.grant(CLIENT, OTHER, uri("private", "/tree/sub?other=1"), READ | PREFIX);
        h.access(OTHER, uri("private", "/tree/sub/child"), READ, true);
        h.access(OTHER, uri("private", "/tree/sibling"), READ, false);
        h.revoke(OWNER, null, uri("private", "/item?unrelated=3"), READ);
        h.access(CLIENT, exact, READ, false); h.access(OTHER, exact, READ, false);
        h.revoke(OWNER, null, uri("private", "/tree/sub"), READ);
        h.access(CLIENT, uri("private", "/tree/sub/child"), READ, true);
        h.access(OTHER, uri("private", "/tree/sub/child"), READ, false);
    }
    private static void modes() throws Exception {
        Harness h = new Harness(); Uri leaf = uri("private", "/tree/leaf");
        h.grant(OWNER, CLIENT, leaf, READ);
        h.grant(OWNER, CLIENT, uri("private", "/tree"), WRITE | PREFIX);
        h.access(CLIENT, leaf, READ, true); h.access(CLIENT, leaf, WRITE, true);
        h.access(CLIENT, leaf, READ | WRITE, false);
        h.rejected(SecurityException.class, () -> h.grant(CLIENT, OTHER, leaf, READ | WRITE));
        h.grant(OWNER, CLIENT, leaf, WRITE);
        h.access(CLIENT, leaf, READ | WRITE, true);
        h.grant(CLIENT, OTHER, leaf, READ | WRITE);
        h.access(OTHER, leaf, READ | WRITE, true);
        check(h.listed(CLIENT).size() == 2, "Repeated same-identity grants were not coalesced");
        h.grant(OWNER, BOTH, uri("private", "/tree"), READ | PREFIX);
        h.grant(OWNER, BOTH, uri("private", "/tree/branch"), WRITE | PREFIX);
        h.access(BOTH, uri("private", "/tree/branch/leaf"), READ | WRITE, false);
        h.rejected(SecurityException.class, () -> h.grant(BOTH, OTHER,
                uri("private", "/tree/branch/leaf"), READ | WRITE | PREFIX));
        h.grant(OWNER, BOTH, uri("private", "/tree"), WRITE | PREFIX);
        h.grant(BOTH, OTHER, uri("private", "/tree/branch"), READ | WRITE | PREFIX);
        h.access(OTHER, uri("private", "/tree/branch/leaf"), READ | WRITE, true);
    }
    private static void patterns() throws Exception {
        Harness h = new Harness();
        for (String authority : new String[]{"patternOnly", "limited"}) {
            Uri allowed = uri(authority, "/allowed/item");
            ProviderInfo actual = h.provider(allowed);
            check(actual.uriPermissionPatterns != null && actual.uriPermissionPatterns.length == 1,
                    "Parsed grant-uri-permission pattern was stripped from provider policy");
            h.grant(OWNER, CLIENT, allowed, READ); h.access(CLIENT, allowed, READ, true);
            h.grant(DELEGATE, OTHER, allowed, READ); h.access(OTHER, allowed, READ, true);
            h.rejected(SecurityException.class, () -> h.grant(OWNER, CLIENT, uri(authority, "/outside"), READ));
            h.rejected(SecurityException.class, () -> h.grant(DELEGATE, OTHER, uri(authority, "/outside"), READ));
        }
    }
    private static void revocation() throws Exception {
        Harness h = new Harness(); Uri privateUri = uri("private", "/tree/item");
        h.grant(OWNER, CLIENT, privateUri, READ | WRITE);
        h.grant(OWNER, OTHER, privateUri, READ | WRITE);
        h.revoke(CLIENT, OTHER, privateUri, READ);
        h.access(CLIENT, privateUri, READ | WRITE, true); h.access(OTHER, privateUri, READ | WRITE, true);
        h.revoke(CLIENT, null, privateUri, READ);
        h.access(CLIENT, privateUri, READ, false); h.access(CLIENT, privateUri, WRITE, true);
        h.access(OTHER, privateUri, READ | WRITE, true);
        h.revoke(CLIENT, CLIENT, privateUri, WRITE); h.access(CLIENT, privateUri, WRITE, false);
        h.revoke(CLIENT, null, privateUri, READ | WRITE); // An already-relinquished grant is harmless.
        h.access(OTHER, privateUri, READ | WRITE, true);
        Uri protectedUri = uri("protected", "/tree/item");
        h.grant(OWNER, CLIENT, protectedUri, READ | WRITE); h.grant(OWNER, OTHER, protectedUri, READ | WRITE);
        h.revoke(DELEGATE, null, protectedUri, READ | WRITE);
        h.access(CLIENT, protectedUri, READ | WRITE, true); h.access(OTHER, protectedUri, READ | WRITE, true);
        h.revoke(DELEGATE, CLIENT, uri("protected", "/tree"), READ);
        h.access(CLIENT, protectedUri, READ, false); h.access(CLIENT, protectedUri, WRITE, true);
        h.access(OTHER, protectedUri, READ | WRITE, true);
        h.revoke(DELEGATE, null, uri("protected", "/tree"), READ);
        h.access(OTHER, protectedUri, READ, false); h.access(OTHER, protectedUri, WRITE, true);
        h.revoke(BOTH, null, protectedUri, WRITE);
        h.access(CLIENT, protectedUri, WRITE, false); h.access(OTHER, protectedUri, WRITE, false);
        h.grant(OWNER, CLIENT, privateUri, READ); h.grant(CLIENT, OTHER, privateUri, READ);
        h.revoke(CLIENT, null, privateUri, READ);
        h.access(CLIENT, privateUri, READ, false); h.access(OTHER, privateUri, READ, true);
    }
    private static void retirement() throws Exception {
        Harness h = new Harness(); Uri value = uri("private", "/item");
        h.grant(OWNER, CLIENT, value, READ); h.replace(CLIENT);
        h.access(CLIENT, value, READ, false); check(h.listed(CLIENT).isEmpty(), "Replacement inherited old target grants");
        h.grant(OWNER, CLIENT, value, READ); h.replace(OWNER);
        h.access(CLIENT, value, READ, false); check(h.listed(CLIENT).isEmpty(), "Old provider generation remained grantable");
        h.grant(OWNER, CLIENT, value, READ);
        call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, h.records.get(CLIENT));
        h.access(CLIENT, value, READ, false);
        h.rejected(SecurityException.class, () -> h.grant(OWNER, CLIENT, value, READ));
        call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.records.get(CLIENT));
        h.access(CLIENT, value, READ, true);
        call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, CLIENT);
        h.access(CLIENT, value, READ, false);
        h.grant(OWNER, CLIENT, value, READ);
        call(h.pm, "beginPackageRemoval", new Class<?>[]{ApkPackage.class}, h.records.get(OWNER));
        h.access(CLIENT, value, READ, false);
        String before = h.snapshot();
        h.grant(CLIENT, OTHER, value, READ);
        check(before.equals(h.snapshot()), "Unavailable provider published a grant during removal");
        call(h.pm, "endPackageRemoval", new Class<?>[]{ApkPackage.class}, h.records.get(OWNER));
        call(h.pm, "revokePackageCapabilities", new Class<?>[]{String.class}, OWNER);
        h.access(CLIENT, value, READ, false);
    }
    private static void validation() throws Exception {
        Harness h = new Harness(); Uri value = uri("private", "/item");
        h.grant(OWNER, CLIENT, value, READ);
        for (Uri invalid : new Uri[]{null, Uri.parse("file:///tmp/item"), Uri.parse("content:/item"),
                Uri.parse("content://10@fixture.uri.private/item"), uri("private", "/a/../item"),
                uri("private", "/a/%2e%2e/item")}) {
            h.rejected(SecurityException.class, () -> h.grant(OWNER, OTHER, invalid, READ));
        }
        for (int modes : new int[]{0, 0x40000000, READ | 0x40000000}) {
            h.rejected(IllegalArgumentException.class, () -> h.grant(OWNER, OTHER, value, modes));
        }
        h.rejected(UnsupportedOperationException.class, () -> h.grant(OWNER, OTHER, value,
                READ | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION));
        String before = h.snapshot();
        h.grant(OWNER, "fixture.uri.unknown", value, READ);
        h.grant(OWNER, OTHER, uri("unknown", "/item"), READ);
        h.revoke(OWNER, null, uri("unknown", "/item"), READ);
        check(before.equals(h.snapshot()), "Unknown provider or target changed existing grants");
        h.rejected(IllegalArgumentException.class, () -> h.grant(OWNER, null, value, READ));
        h.rejected(SecurityException.class, () -> h.as(OWNER,
                () -> h.am.grantUriPermission(h.threads.get(CLIENT), OTHER, value, READ, 0)));
        h.rejected(SecurityException.class, () -> h.as(OWNER,
                () -> h.am.grantUriPermission(h.threads.get(OWNER), OTHER, value, READ, 10)));
        h.access(CLIENT, value, READ, true); h.access(OTHER, value, READ, false);
    }

    public static void contracts(String selected, boolean throughBinder) throws Exception {
        Field singleton = Unsafe.class.getDeclaredField("theUnsafe"); singleton.setAccessible(true);
        unsafe = (Unsafe) singleton.get(null); binder = throughBinder;
        for (String group : new String[]{"delegation", "identity", "modes", "patterns", "revocation", "retirement", "validation"}) {
            if (selected != null && !selected.equals(group)) continue;
            switch (group) {
                case "delegation": delegation(); break;
                case "identity": identity(); break;
                case "modes": modes(); break;
                case "patterns": patterns(); break;
                case "revocation": revocation(); break;
                case "retirement": retirement(); break;
                case "validation": validation(); break;
                default: throw new AssertionError(group);
            }
        }
        if (checks == 0) throw new IllegalArgumentException("Unknown URI contract group: " + selected);
        System.out.println("UriGrantContractTest all passed: " + checks + " checks; binder=" + binder);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        android.content.res.AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        String previousData = System.getenv("ANDROID_DATA");
        String previousMode = System.getenv("GESHER_MICROG_SPOOF");
        java.nio.file.Path data = java.nio.file.Files.createTempDirectory("gesher-uri-contracts-");
        try {
            android.system.Os.setenv("ANDROID_DATA", data.toString(), true);
            android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
            contracts(args.length == 0 ? null : args[0], true);
        } finally {
            if (previousData == null) android.system.Os.unsetenv("ANDROID_DATA");
            else android.system.Os.setenv("ANDROID_DATA", previousData, true);
            if (previousMode == null) android.system.Os.unsetenv("GESHER_MICROG_SPOOF");
            else android.system.Os.setenv("GESHER_MICROG_SPOOF", previousMode, true);
            try (java.util.stream.Stream<java.nio.file.Path> paths = java.nio.file.Files.walk(data)) {
                for (java.nio.file.Path path : (Iterable<java.nio.file.Path>) paths.sorted(java.util.Comparator.reverseOrder())::iterator) {
                    java.nio.file.Files.delete(path);
                }
            }
        }
    }
}
