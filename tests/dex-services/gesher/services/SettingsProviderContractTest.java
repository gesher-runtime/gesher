/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IContentProvider;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.database.ContentObserver;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.provider.Settings;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.PrintStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicReference;

/** Same public provider contracts on the host JVM and Windows ART. */
public final class SettingsProviderContractTest {
    public static int checks;
    private static final Uri TABLE = Uri.parse("content://settings/system");
    private static void check(boolean value, String why) {
        if (!value) throw new AssertionError(why); checks++;
    }
    private interface Work { void run() throws Exception; }
    private static void rejects(Class<? extends Throwable> expected, Work work) throws Exception {
        try { work.run(); }
        catch (Throwable failure) { check(expected.isInstance(failure), "Wrong failure: " + failure); return; }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static ContentProvider provider(File file) throws Exception {
        return provider(file, null);
    }
    private static ContentProvider provider(File file, Object packages) throws Exception {
        Class<?> type = Class.forName("com.gesher.server.SettingsProvider");
        Constructor<?> constructor; Object[] arguments;
        try {
            constructor = type.getDeclaredConstructor(Class.forName("com.gesher.server.PackageManagerService"),
                    Class.forName("com.gesher.server.ActivityTaskManagerService"), File.class);
            arguments = new Object[]{packages, null, file};
        } catch (NoSuchMethodException baseline) {
            constructor = type.getDeclaredConstructor(); arguments = new Object[0];
        }
        constructor.setAccessible(true);
        try { return (ContentProvider) constructor.newInstance(arguments); }
        catch (InvocationTargetException failure) { throw new AssertionError(failure.getCause()); }
    }
    private static ContentValues values(String name, String value) {
        ContentValues result = new ContentValues(); result.put("name", name); result.put("value", value); return result;
    }
    private static Bundle value(String value) { Bundle b = new Bundle(); b.putString("value", value); return b; }
    private static Uri item(String name) { return Uri.withAppendedPath(TABLE, name); }
    private static String read(ContentProvider provider, String name) { return provider.call("GET_system", name, null).getString("value"); }
    private static long rowId(ContentProvider provider, String name) {
        try (Cursor cursor = provider.query(item(name), new String[]{"_id"}, null, null, null)) {
            check(cursor.moveToFirst(), "Missing row " + name); return cursor.getLong(0);
        }
    }
    private static void query(ContentProvider p) throws Exception {
        p.call("PUT_system", "contract", value("first"));
        try (Cursor c = p.query(item("contract"), new String[]{"value", "name", "value", "_id"}, null, null, null)) {
            check(Arrays.equals(c.getColumnNames(), new String[]{"value", "name", "value", "_id"}), "Projection/order/duplicates");
            check(c.getCount() == 1 && c.moveToFirst(), "Item query row count");
            check("first".equals(c.getString(0)) && "contract".equals(c.getString(1)) && "first".equals(c.getString(2)), "Projected row values");
        }
        try (Cursor c = p.query(item("contract"), null, null, null, null)) {
            check(Arrays.equals(c.getColumnNames(), new String[]{"_id", "name", "value", "is_preserved_in_restore"}), "Default projection");
        }
        for (String selection : new String[]{"name=?", " ( name = ? ) ", "name='contract'", "(name=\"contract\")"}) {
            try (Cursor c = p.query(TABLE, new String[]{"value"}, selection,
                    selection.contains("?") ? new String[]{"contract"} : null, "ignored")) {
                check(c.moveToFirst() && "first".equals(c.getString(0)) && c.getCount() == 1, "Supported selection " + selection);
            }
        }
        check("vnd.android.cursor.dir/system".equals(p.getType(TABLE)), "Directory MIME type");
        check("vnd.android.cursor.item/system".equals(p.getType(item("contract"))), "Item MIME type");
        rejects(IllegalArgumentException.class, () -> p.query(TABLE, new String[]{"unknown"}, null, null, null));
        rejects(IllegalArgumentException.class, () -> p.query(TABLE, null, "value=?", new String[]{"first"}, null));
        rejects(IllegalArgumentException.class, () -> p.query(item("contract"), null, "name=?", new String[]{"contract"}, null));
        rejects(IllegalArgumentException.class, () -> p.query(Uri.parse("content://settings/missing"), null, null, null, null));
    }
    private static void crud(ContentProvider p) throws Exception {
        check(item("crud").equals(p.insert(TABLE, values("crud", "one"))), "Insert URI");
        check("one".equals(read(p, "crud")), "Insert visible to GET");
        long before = rowId(p, "crud");
        check(p.insert(TABLE, values("crud", "one")) == null, "Unchanged insert");
        check(rowId(p, "crud") == before, "Unchanged generation");
        check(p.update(item("crud"), values("crud", "two"), null, null) == 1, "Update existing row");
        check("two".equals(read(p, "crud")) && rowId(p, "crud") > before, "Updated value and generation");
        check(p.update(item("absent"), values("absent", "x"), null, null) == 0, "Update does not insert");
        ContentValues noName = new ContentValues(); noName.put("value", "x");
        check(p.update(item("crud"), noName, null, null) == 0, "AOSP update requires valid values name");
        check(p.bulkInsert(TABLE, new ContentValues[]{values("bulk-a", "a"), values("bulk-b", "b"), values("", "bad")}) == 2, "Bulk row count");
        check(p.delete(TABLE, "name=?", new String[]{"crud"}) == 1, "Delete row count");
        check(p.delete(item("crud"), null, null) == 0 && read(p, "crud") == null, "Delete missing row");
        rejects(IllegalArgumentException.class, () -> p.delete(TABLE, null, null));
    }
    private static void call(ContentProvider p) throws Exception {
        check(p.call("PUT_system", "nullable", value(null)) == null, "AOSP PUT return envelope");
        try (Cursor c = p.query(item("nullable"), new String[]{"name", "value"}, null, null, null)) {
            check(c.getCount() == 1 && c.moveToFirst() && c.isNull(1), "Null value retains row");
        }
        ArrayList<String> rows = p.call("LIST_system", null, null).getStringArrayList("result_settings_list");
        check(rows != null && rows.contains("nullable=null"), "LIST key and name=value format");
        check(p.call("DELETE_system", "nullable", null).getInt("result_rows_deleted") == 1, "DELETE result count");
        check(p.call("DELETE_system", "nullable", null).getInt("result_rows_deleted") == 0, "DELETE absent result count");
        try (Cursor c = p.query(item("nullable"), null, null, null, null)) { check(c.getCount() == 0, "Deleted row absent"); }
        p.call("PUT_system", "literal-null", value("null"));
        check(read(p, "literal-null") == null, "Android literal null normalization");
        ByteArrayOutputStream captured = new ByteArrayOutputStream();
        PrintStream original = System.out;
        long before;
        try (PrintStream log = new PrintStream(captured, true, "UTF-8")) {
            System.setOut(log);
            p.call("PUT_system", "contract-log", value("7"));
            before = rowId(p, "contract-log");
            p.call("PUT_system", "contract-log", value("7"));
        } finally { System.setOut(original); }
        check("7".equals(read(p, "contract-log")) && rowId(p, "contract-log") == before,
                "Repeated accepted PUT preserves its value and generation");
        check(captured.toString("UTF-8").lines().filter(
                line -> line.equals("system_server: settings: updated system key contract-log")).count() == 2,
                "Accepted PUT diagnostic includes unchanged durable values");
    }
    private static void persistence(File file) throws Exception {
        ContentProvider p = provider(file);
        p.call("PUT_system", "persist", value("x\u0000\ud800"));
        p.call("PUT_secure", "nullable", value(null));
        p.call("DELETE_global", "device_name", null);
        long savedId = -1;
        try (Cursor c = p.query(item("persist"), new String[]{"_id"}, null, null, null)) {
            // Keep the old provider's missing projection support from masking this
            // separate persistence negative control.
            if (c.getColumnIndex("_id") >= 0 && c.moveToFirst()) savedId = c.getLong(c.getColumnIndex("_id"));
        }
        final long id = savedId;
        ContentProvider loaded = provider(file);
        check("x\u0000\ud800".equals(read(loaded, "persist")) && rowId(loaded, "persist") == id, "Durable UTF-16 row and generation");
        check(loaded.call("LIST_secure", null, null).getStringArrayList("result_settings_list").contains("nullable=null"), "Durable null row");
        check(loaded.call("GET_global", "device_name", null).getString("value") == null, "Deleted default remains deleted");
        File blocker = new File(file.getParentFile(), "not-a-directory"); Files.write(blocker.toPath(), new byte[]{1});
        ContentProvider broken = provider(new File(blocker, "settings.bin"));
        ByteArrayOutputStream captured = new ByteArrayOutputStream();
        PrintStream original = System.out;
        try (PrintStream log = new PrintStream(captured, true, "UTF-8")) {
            System.setOut(log);
            rejects(IllegalStateException.class, () -> broken.call("PUT_system", "failed", value("not-committed")));
        } finally { System.setOut(original); }
        check(!captured.toString("UTF-8").contains("settings: updated"), "Failed persistence must not report an accepted PUT");
        check(read(broken, "failed") == null, "Failed persistence rolls back insertion");
        String old = read(broken, "font_scale");
        rejects(IllegalStateException.class, () -> broken.call("PUT_system", "font_scale", value("9")));
        check(old.equals(read(broken, "font_scale")), "Failed persistence rolls back update");
        rejects(IllegalStateException.class, () -> broken.call("DELETE_system", "font_scale", null));
        check(old.equals(read(broken, "font_scale")), "Failed persistence rolls back deletion");
        checkerSubtypeGeneration(file, loaded);
        deviceName(new File(file.getParentFile(), "device-name.bin"));
    }
    /** Sets persist.gesher.device_name; false on a host JVM without system properties. */
    private static boolean derivedName(String name) {
        try {
            android.os.SystemProperties.set("persist.gesher.device_name", name);
            return name.equals(android.os.SystemProperties.get("persist.gesher.device_name", ""));
        } catch (RuntimeException | LinkageError hostJvm) {
            return false;
        }
    }
    private static String global(ContentProvider p, String name) {
        return p.call("GET_global", name, null).getString("value");
    }
    /** DEVICE_NAME is Windows' computer name; a stale derived or legacy name follows it, the user's stays. */
    private static void deviceName(File file) throws Exception {
        Class<?> type = Class.forName("com.gesher.server.SettingsProvider");
        java.lang.reflect.Method defaultName = type.getDeclaredMethod("defaultDeviceName");
        defaultName.setAccessible(true);
        String expected = (String) defaultName.invoke(null);
        check(expected != null && !expected.isEmpty(), "Device name default");
        String computer = (String) Class.forName("com.gesher.server.WindowsHost")
                .getMethod("computerName").invoke(null);
        String netbios = System.getenv("COMPUTERNAME");
        if (computer != null) {
            check(computer.equals(expected), "Device name is the computer name: " + expected);
            // NetBIOS is the host name's first 15 characters, upper case.
            if (netbios != null) {
                check(expected.toUpperCase(java.util.Locale.ROOT).startsWith(netbios.toUpperCase(java.util.Locale.ROOT)),
                        "Physical DNS host name " + expected + " matches COMPUTERNAME " + netbios);
            }
        }
        ContentProvider p = provider(file);
        check(expected.equals(global(p, "device_name")), "Fresh profile device name");
        // A profile from before the device name followed the computer name: no derived
        // name recorded, the fixed "Gesher" stored.
        p.call("PUT_global", "device_name", value("Gesher"));
        boolean properties = derivedName("");
        check(expected.equals(global(provider(file), "device_name")), "Legacy fixed default follows the computer name");
        if (properties) {
            check(expected.equals(android.os.SystemProperties.get("persist.gesher.device_name")), "Derived name recorded");
            // Once Gesher derived a name, "Gesher" is a name the user chose.
            provider(file).call("PUT_global", "device_name", value("Gesher"));
            check("Gesher".equals(global(provider(file), "device_name")), "User's device name Gesher stays");
            // As if Gesher derived OLD-PC-NAME before the PC was renamed.
            provider(file).call("PUT_global", "device_name", value("OLD-PC-NAME"));
            check(derivedName("OLD-PC-NAME"), "Derived name set");
            check(expected.equals(global(provider(file), "device_name")), "Renamed PC renames a derived device name");
            check(expected.equals(android.os.SystemProperties.get("persist.gesher.device_name")), "Derived name recorded again");
        }
        provider(file).call("PUT_global", "device_name", value("Kitchen tablet"));
        check("Kitchen tablet".equals(global(provider(file), "device_name")), "User's device name stays");
        provider(file).call("DELETE_global", "device_name", null);
        check(global(provider(file), "device_name") == null, "Deleted device name stays deleted");
    }
    private static void checkerSubtypeGeneration(File file, ContentProvider p) throws Exception {
        String checker = Settings.Secure.SELECTED_SPELL_CHECKER;
        String subtype = Settings.Secure.SELECTED_SPELL_CHECKER_SUBTYPE;
        long identity = Binder.clearCallingIdentity();
        try {
            p.call("PUT_secure", subtype, value("1002"));
            p.call("PUT_secure", checker, value(""));
            check("0".equals(p.call("GET_secure", subtype, null).getString("value")),
                    "New spell-checker selection resets the previous subtype");
            p.call("PUT_secure", subtype, value("1002"));
            check("1002".equals(p.call("GET_secure", subtype, null).getString("value")),
                    "Explicit same-ID subtype selection updates its generation");
            check("1002".equals(provider(file).call("GET_secure", subtype, null).getString("value")),
                    "Re-selected subtype survives settings reload");
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
    }
    private static void transientCheckerRows(File file) throws Exception {
        Class<?> packageType = Class.forName("com.gesher.server.PackageManagerService");
        Constructor<?> constructor = packageType.getDeclaredConstructor(java.util.Collection.class, boolean.class);
        constructor.setAccessible(true);
        ContentProvider p = provider(file, constructor.newInstance(java.util.List.of(), false));
        Class<?> type = p.getClass();
        var settings = type.getDeclaredMethod("spellCheckerSettings");
        settings.setAccessible(true);
        var initialize = type.getDeclaredMethod("initializeSpellChecker",
                Class.forName("com.gesher.server.SettingsProvider$SpellCheckerSettings"),
                Class.forName("com.gesher.server.SpellCheckerCatalog$Entry"));
        initialize.setAccessible(true);
        check((Boolean) initialize.invoke(p, settings.invoke(p), null), "Transient checker initialization");
        Uri secure = Uri.parse("content://settings/secure");
        java.util.Set<Long> ids = new java.util.HashSet<>();
        int textRows = 0;
        try (Cursor c = p.query(secure, null, null, null, null)) {
            while (c.moveToNext()) {
                check(ids.add(c.getLong(c.getColumnIndexOrThrow("_id"))), "Duplicate secure row ID");
                String name = c.getString(c.getColumnIndexOrThrow("name"));
                String value = c.getString(c.getColumnIndexOrThrow("value"));
                if (Settings.Secure.SELECTED_SPELL_CHECKER.equals(name)) {
                    check("".equals(value), "Transient checker row value"); textRows++;
                } else if (Settings.Secure.SELECTED_SPELL_CHECKER_SUBTYPE.equals(name)) {
                    check("0".equals(value), "Transient subtype row value"); textRows++;
                }
            }
        }
        check(textRows == 2, "Transient text-service rows missing from query");
        rejects(SecurityException.class, () -> p.call("PUT_secure",
                Settings.Secure.SELECTED_SPELL_CHECKER_SUBTYPE, value("42")));
        check(!new File(file.toString() + ".spell-checker").exists(),
                "Transient profile touched privileged checker storage");
    }
    private static void limits(ContentProvider p) throws Exception {
        p.call("PUT_system", "limit", value("x".repeat(32768)));
        check(read(p, "limit").length() == 32768, "Maximum accepted value");
        rejects(IllegalArgumentException.class, () -> p.call("PUT_system", "limit", value("x".repeat(32769))));
        rejects(IllegalArgumentException.class, () -> p.call("PUT_system", "n".repeat(32769), value("x")));
        check(read(p, "limit").length() == 32768, "Rejected value leaves previous state");
        check(p.insert(TABLE, values("bad\nkey", "x")) == null, "Invalid binary key");
    }
    private static void observers(ContentProvider provider) throws Exception {
        class TestContext extends ContextWrapper {
            ContentResolver resolver;
            TestContext() { super(null); }
            @Override public String getPackageName() { return "android"; }
            @Override public String getOpPackageName() { return "android"; }
            @Override public int getUserId() { return 0; }
            @Override public ApplicationInfo getApplicationInfo() { return new ApplicationInfo(); }
            @Override public Object getSystemService(String name) { return null; }
            @Override public ContentResolver getContentResolver() { return resolver; }
        }
        TestContext context = new TestContext(); ArrayList<Uri> seen = new ArrayList<>();
        context.resolver = new ContentResolver(context) {
            @Override protected IContentProvider acquireProvider(Context c, String name) { throw new AssertionError(); }
            @Override protected IContentProvider acquireUnstableProvider(Context c, String name) { throw new AssertionError(); }
            @Override public boolean releaseProvider(IContentProvider p) { return true; }
            @Override public boolean releaseUnstableProvider(IContentProvider p) { return true; }
            @Override public void unstableProviderDied(IContentProvider p) { }
            @Override public void notifyChange(Uri uri, ContentObserver observer) {
                check(!Thread.holdsLock(provider), "Observer called under provider monitor");
                check(Binder.getCallingUid() == android.os.Process.myUid(), "Observer identity not cleared");
                AtomicReference<Throwable> failed = new AtomicReference<>();
                Thread reenter = new Thread(() -> { try { read(provider, "observe"); } catch (Throwable e) { failed.set(e); } });
                reenter.setDaemon(true); reenter.start();
                try { reenter.join(2000); } catch (InterruptedException e) { throw new AssertionError(e); }
                check(!reenter.isAlive() && failed.get() == null, "Reentrant observer blocked"); seen.add(uri);
            }
        };
        Field info = provider.getClass().getDeclaredField("mInfo"); info.setAccessible(true);
        provider.attachInfo(context, (ProviderInfo) info.get(provider));
        provider.call("PUT_system", "observe", value("one"));
        provider.call("PUT_system", "observe", value("one"));
        provider.call("PUT_system", "observe", value("two"));
        provider.call("DELETE_system", "observe", null);
        check(seen.equals(Arrays.asList(item("observe"), item("observe"), item("observe"))), "Changed-row notifications only");
    }
    /** location_mode follows the location service's live state; the service owns it. */
    private static void location(ContentProvider provider) throws Exception {
        Class<?> type = provider.getClass();
        Class<?> stateType = Class.forName("com.gesher.server.SettingsProvider$LocationState");
        java.lang.reflect.Method set = type.getDeclaredMethod("setLocationState", stateType);
        java.lang.reflect.Method changed = type.getDeclaredMethod("locationStateChanged");
        set.setAccessible(true); changed.setAccessible(true);
        Uri secure = Uri.parse("content://settings/secure");
        String mode = Settings.Secure.LOCATION_MODE, allowed = "location_providers_allowed";
        java.util.function.Function<String, String> get = name ->
                provider.call("GET_secure", name, null).getString("value");
        // Without a location service the stored row answers, as before.
        set.invoke(null, (Object) null);
        check("0".equals(get.apply(mode)), "Stored location_mode without a location service");
        boolean[] enabled = {true}; boolean[] fail = {false};
        Object state = java.lang.reflect.Proxy.newProxyInstance(stateType.getClassLoader(),
                new Class<?>[]{stateType}, (proxy, method, arguments) -> {
                    if (!"locationEnabled".equals(method.getName())) throw new AssertionError(method);
                    // The location service may wait for Windows: never under the provider monitor.
                    check(!Thread.holdsLock(provider), "Location state asked under the provider monitor");
                    if (fail[0]) throw new IllegalStateException("location service unavailable");
                    return enabled[0];
                });
        try {
            set.invoke(null, state);
            check(Settings.Secure.LOCATION_MODE_ON == 3, "LOCATION_MODE_ON value");
            check("3".equals(get.apply(mode)), "location_mode on: LOCATION_MODE_ON");
            try (Cursor c = provider.query(Uri.withAppendedPath(secure, mode), new String[]{"name", "value"}, null, null, null)) {
                check(c.getCount() == 1 && c.moveToFirst() && "3".equals(c.getString(1)), "Item query: live location_mode");
            }
            try (Cursor c = provider.query(secure, new String[]{"value"}, "name=?", new String[]{mode}, null)) {
                check(c.moveToFirst() && "3".equals(c.getString(0)), "Selection query: live location_mode");
            }
            boolean listedMode = false;
            try (Cursor c = provider.query(secure, new String[]{"name", "value"}, null, null, null)) {
                while (c.moveToNext()) {
                    if (mode.equals(c.getString(0))) listedMode = "3".equals(c.getString(1));
                    check(!allowed.equals(c.getString(0)), "No location_providers_allowed row (Android S+)");
                }
            }
            check(listedMode, "Table query: live location_mode");
            ArrayList<String> list = provider.call("LIST_secure", null, null).getStringArrayList("result_settings_list");
            check(list.contains(mode + "=3"), "LIST: live location_mode " + list);
            check(get.apply(allowed) == null, "location_providers_allowed does not exist (Android S+)");
            // Windows turns location off: the next read says so.
            enabled[0] = false;
            check("0".equals(get.apply(mode)), "location_mode off: LOCATION_MODE_OFF");
            // Windows' location setting owns it: writes neither take effect nor are stored.
            enabled[0] = true;
            provider.call("PUT_secure", mode, value("0"));
            check("3".equals(get.apply(mode)), "Ignored location_mode PUT");
            check(provider.call("DELETE_secure", mode, null).getInt("result_rows_deleted") == 0, "Ignored location_mode DELETE");
            check(provider.insert(secure, values(mode, "0")) == null, "Ignored location_mode insert");
            check(provider.update(Uri.withAppendedPath(secure, mode), values(mode, "0"), null, null) == 0, "Ignored location_mode update");
            check(provider.delete(Uri.withAppendedPath(secure, mode), null, null) == 0, "Ignored location_mode delete");
            check("3".equals(get.apply(mode)), "Live location_mode after ignored writes");
            // Other secure rows are untouched.
            provider.call("PUT_secure", "contract_secure", value("kept"));
            check("kept".equals(get.apply("contract_secure")), "Other secure rows still stored");
            // A failing location service: the stored row answers instead of an error.
            fail[0] = true;
            check("0".equals(get.apply(mode)), "Stored location_mode when the service fails");
            fail[0] = false;
            // A state change notifies location_mode's observers, outside the provider monitor.
            ArrayList<Uri> seen = new ArrayList<>();
            class TestContext extends ContextWrapper {
                ContentResolver resolver;
                TestContext() { super(null); }
                @Override public String getPackageName() { return "android"; }
                @Override public String getOpPackageName() { return "android"; }
                @Override public int getUserId() { return 0; }
                @Override public ApplicationInfo getApplicationInfo() { return new ApplicationInfo(); }
                @Override public Object getSystemService(String name) { return null; }
                @Override public ContentResolver getContentResolver() { return resolver; }
            }
            TestContext context = new TestContext();
            context.resolver = new ContentResolver(context) {
                @Override protected IContentProvider acquireProvider(Context c, String name) { throw new AssertionError(); }
                @Override protected IContentProvider acquireUnstableProvider(Context c, String name) { throw new AssertionError(); }
                @Override public boolean releaseProvider(IContentProvider p) { return true; }
                @Override public boolean releaseUnstableProvider(IContentProvider p) { return true; }
                @Override public void unstableProviderDied(IContentProvider p) { }
                @Override public void notifyChange(Uri uri, ContentObserver observer) {
                    check(!Thread.holdsLock(provider), "Location observer called under provider monitor");
                    seen.add(uri);
                }
            };
            Field info = type.getDeclaredField("mInfo"); info.setAccessible(true);
            provider.attachInfo(context, (ProviderInfo) info.get(provider));
            seen.clear();
            changed.invoke(null);
            check(seen.equals(Arrays.asList(Uri.withAppendedPath(secure, mode))), "Location change notifies location_mode " + seen);
        } finally {
            set.invoke(null, (Object) null);
        }
        // Without the location service again, writes reach the stored row as before.
        provider.call("PUT_secure", mode, value("1"));
        check("1".equals(get.apply(mode)), "Stored location_mode writable without a location service");
    }
    public static void contracts(String group) throws Exception {
        Field clock = Class.forName("com.gesher.app.locale.SystemLocale").getDeclaredField("sIs24Hour");
        clock.setAccessible(true); clock.set(null, Boolean.TRUE); // Declared host locale boundary.
        File directory = Files.createTempDirectory("gesher-settings-contract-").toFile();
        try {
            for (String name : new String[]{"query", "crud", "call", "persistence", "transient", "limits", "observers", "location"}) {
                if (!group.equals("all") && !group.equals(name)) continue;
                File file = new File(directory, name + ".bin"); ContentProvider p = provider(file);
                switch (name) {
                    case "query": query(p); break; case "crud": crud(p); break;
                    case "call": call(p); break; case "persistence": persistence(file); break;
                    case "transient": transientCheckerRows(file); break;
                    case "limits": limits(p); break; case "observers": observers(p); break;
                    case "location": location(p); break;
                    default: throw new AssertionError(name);
                }
            }
        } finally { removeTree(directory); }
    }
    private static void removeTree(File file) {
        File[] children = file.listFiles(); if (children != null) for (File child : children) removeTree(child);
        file.delete();
    }
    public static void main(String[] args) throws Exception {
        contracts(args.length == 0 ? "all" : args[0]);
        System.out.println("Settings provider contracts passed: " + checks + " checks");
        System.out.println("Settings write access passed: " + SettingsWriteAccessTest.run(true) + " checks");
    }
}
