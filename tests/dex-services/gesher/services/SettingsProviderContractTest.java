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
        Class<?> type = Class.forName("com.gesher.server.SettingsProvider");
        Constructor<?> constructor; Object[] arguments;
        try {
            constructor = type.getDeclaredConstructor(Class.forName("com.gesher.server.PackageManagerService"),
                    Class.forName("com.gesher.server.ActivityTaskManagerService"), File.class);
            arguments = new Object[]{null, null, file};
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
    public static void contracts(String group) throws Exception {
        Field clock = Class.forName("com.gesher.app.locale.SystemLocale").getDeclaredField("sIs24Hour");
        clock.setAccessible(true); clock.set(null, Boolean.TRUE); // Declared host locale boundary.
        File directory = Files.createTempDirectory("gesher-settings-contract-").toFile();
        try {
            for (String name : new String[]{"query", "crud", "call", "persistence", "limits", "observers"}) {
                if (!group.equals("all") && !group.equals(name)) continue;
                File file = new File(directory, name + ".bin"); ContentProvider p = provider(file);
                switch (name) {
                    case "query": query(p); break; case "crud": crud(p); break;
                    case "call": call(p); break; case "persistence": persistence(file); break;
                    case "limits": limits(p); break; case "observers": observers(p); break;
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
    }
}
