/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentProviderResult;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IContentProvider;
import android.content.OperationApplicationException;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.database.ContentObserver;
import android.database.Cursor;
import android.database.SQLException;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.provider.UserDictionary.Words;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;

/** Real framework SQLite, provider CRUD and public client helper, on ART/Android. */
public final class UserDictionaryContractTest {
    private static int checks;
    private static void check(boolean result, String why) {
        if (!result) throw new AssertionError(why);
        checks++;
    }
    private interface Work { void run() throws Exception; }
    private static void rejects(Class<? extends Throwable> type, Work work) throws Exception {
        try { work.run(); }
        catch (Throwable problem) {
            check(type.isInstance(problem), "Expected " + type + ", observed " + problem);
            return;
        }
        throw new AssertionError("Expected " + type.getName());
    }

    static final class TestContext extends ContextWrapper {
        final File database;
        final ArrayList<Uri> changes = new ArrayList<>();
        final ContentResolver resolver;
        ContentProvider provider;
        TestContext(File database) {
            super(null);
            this.database = database;
            resolver = new ContentResolver(this) {
                @Override protected IContentProvider acquireProvider(Context context, String authority) {
                    return provider.getIContentProvider();
                }
                @Override protected IContentProvider acquireUnstableProvider(Context context, String authority) {
                    return provider.getIContentProvider();
                }
                @Override public boolean releaseProvider(IContentProvider value) { return true; }
                @Override public boolean releaseUnstableProvider(IContentProvider value) { return true; }
                @Override public void unstableProviderDied(IContentProvider value) { }
                @Override public void notifyChange(Uri uri, ContentObserver observer) {
                    changes.add(uri);
                }
            };
        }
        @Override public String getPackageName() { return "android"; }
        @Override public String getOpPackageName() { return "android"; }
        @Override public int getUserId() { return 0; }
        @Override public ApplicationInfo getApplicationInfo() {
            ApplicationInfo info = new ApplicationInfo();
            info.packageName = getPackageName();
            info.uid = android.os.Process.myUid();
            info.targetSdkVersion = 36;
            return info;
        }
        @Override public android.content.AttributionSource getAttributionSource() {
            return new android.content.AttributionSource(android.os.Process.myUid(), getPackageName(), null);
        }
        @Override public Object getSystemService(String name) { return null; }
        @Override public String getSystemServiceName(Class<?> type) {
            if (type == android.view.inputmethod.InputMethodManager.class) return Context.INPUT_METHOD_SERVICE;
            if (type == android.view.textservice.TextServicesManager.class) return Context.TEXT_SERVICES_MANAGER_SERVICE;
            return null;
        }
        @Override public ContentResolver getContentResolver() { return resolver; }
        @Override public File getDatabasePath(String name) { return database; }
        @Override public SQLiteDatabase openOrCreateDatabase(String name, int mode,
                SQLiteDatabase.CursorFactory factory) {
            return SQLiteDatabase.openOrCreateDatabase(database, factory);
        }
        @Override public SQLiteDatabase openOrCreateDatabase(String name, int mode,
                SQLiteDatabase.CursorFactory factory, android.database.DatabaseErrorHandler errorHandler) {
            return SQLiteDatabase.openOrCreateDatabase(database.getPath(), factory, errorHandler);
        }
    }

    private static ContentProvider create(String className, TestContext context) throws Exception {
        Class<?> type = Class.forName(className);
        ContentProvider provider;
        if (className.equals("com.gesher.server.UserDictionaryProvider")) {
            Constructor<?> constructor = type.getDeclaredConstructor(
                    Class.forName("com.gesher.server.PackageManagerService"),
                    Class.forName("com.gesher.server.ActivityTaskManagerService"), File.class);
            constructor.setAccessible(true);
            provider = (ContentProvider) constructor.newInstance(null, null, context.database);
        } else {
            provider = (ContentProvider) type.getDeclaredConstructor().newInstance();
        }
        ProviderInfo info = new ProviderInfo();
        info.authority = "user_dictionary";
        info.packageName = "android";
        info.name = className;
        info.exported = true;
        info.applicationInfo = context.getApplicationInfo();
        context.provider = provider;
        provider.attachInfo(context, info);
        return provider;
    }

    private static void close(ContentProvider provider) throws Exception {
        // Also closes the unmodified AOSP reference, whose default shutdown only logs.
        Field helper = provider.getClass().getDeclaredField("mOpenHelper");
        helper.setAccessible(true);
        ((SQLiteOpenHelper)helper.get(provider)).close();
    }
    private static ContentValues values(String word) {
        ContentValues values = new ContentValues(); values.put(Words.WORD, word); return values;
    }
    private static int count(ContentProvider provider) {
        try (Cursor cursor = provider.query(Words.CONTENT_URI, new String[]{Words._ID}, null, null, null)) {
            return cursor.getCount();
        }
    }
    private static void queryAndCrud(ContentProvider p, TestContext context) throws Exception {
        check(Words.CONTENT_TYPE.equals(p.getType(Words.CONTENT_URI)), "Collection MIME");
        check(Words.CONTENT_ITEM_TYPE.equals(p.getType(ContentUris.withAppendedId(Words.CONTENT_URI, 7))), "Item MIME");
        check(count(p) == 0, "Fresh database is empty");
        ContentValues input = values("שלום"); input.put(Words.APP_ID, 991);
        Uri first = p.insert(Words.CONTENT_URI, input);
        check(first != null && ContentUris.parseId(first) > 0, "Inserted row URI");
        check(input.getAsInteger(Words.APP_ID) == 991 && !input.containsKey(Words.FREQUENCY), "Insertion does not mutate input");
        try (Cursor c = p.query(first, null, null, null, null)) {
            check(c.moveToFirst() && c.getCount() == 1, "Item query");
            check(new HashSet<>(Arrays.asList(c.getColumnNames())).equals(new HashSet<>(Arrays.asList(
                    Words._ID, Words.WORD, Words.FREQUENCY, Words.LOCALE, Words.APP_ID, Words.SHORTCUT))), "All six columns");
            check("שלום".equals(c.getString(c.getColumnIndexOrThrow(Words.WORD))), "Unicode word");
            check(c.getInt(c.getColumnIndexOrThrow(Words.FREQUENCY)) == 1, "Default frequency");
            check(c.getInt(c.getColumnIndexOrThrow(Words.APP_ID)) == 0, "APP_ID reset on insert");
            check(c.isNull(c.getColumnIndexOrThrow(Words.LOCALE)) && c.isNull(c.getColumnIndexOrThrow(Words.SHORTCUT)), "Nullable locale and shortcut");
            check(first.equals(c.getNotificationUri()), "Cursor watches selected URI");
        }
        input = values("alpha"); input.put(Words.FREQUENCY, 400); input.put(Words.LOCALE, "he_IL"); input.put(Words.SHORTCUT, "al");
        Uri second = p.insert(Words.CONTENT_URI, input);
        try (Cursor c = p.query(Words.CONTENT_URI, new String[]{Words.WORD, Words.FREQUENCY, Words.WORD},
                "locale = ? AND frequency > ?", new String[]{"he_IL", "200"}, "frequency DESC")) {
            check(c.getCount() == 1 && c.moveToFirst(), "SQL selection and arguments");
            check(Arrays.equals(c.getColumnNames(), new String[]{Words.WORD, Words.FREQUENCY, Words.WORD}), "Projection order and duplicates");
            check("alpha".equals(c.getString(0)) && c.getInt(1) == 400 && "alpha".equals(c.getString(2)), "Provider retains direct frequency, not helper clamping");
        }
        try (Cursor c = p.query(Words.CONTENT_URI, new String[]{Words.WORD}, null, null, null)) {
            check(c.moveToFirst() && "alpha".equals(c.getString(0)), "AOSP default descending frequency order");
        }
        ContentValues update = new ContentValues(); update.put(Words.SHORTCUT, "updated"); update.put(Words.APP_ID, 7);
        check(p.update(second, update, "word = ?", new String[]{"different"}) == 0, "Item AND selection no match");
        check(p.update(second, update, "word = ?", new String[]{"alpha"}) == 1, "Item AND selection update");
        try (Cursor c = p.query(second, new String[]{Words.SHORTCUT, Words.APP_ID}, null, null, null)) {
            check(c.moveToFirst() && "updated".equals(c.getString(0)) && c.getInt(1) == 7, "AOSP update does not apply insert defaults");
        }
        check(p.delete(second, "word = ?", new String[]{"different"}) == 0, "Item delete selection no match");
        check(p.delete(second, null, null) == 1 && p.delete(second, null, null) == 0, "Delete and missing row");
        check(context.changes.equals(Arrays.asList(first, second, second, second, second, second, second)),
                "AOSP notifications include zero-row updates/deletes and use operation URI");
        ContentValues nullable = values(null);
        Uri nullWord = p.insert(Words.CONTENT_URI, nullable);
        try (Cursor c = p.query(nullWord, new String[]{Words.WORD}, null, null, null)) {
            check(c.moveToFirst() && c.isNull(0), "Explicit null word follows SQLite schema");
        }
        rejects(SQLException.class, () -> p.insert(Words.CONTENT_URI, new ContentValues()));
        rejects(IllegalArgumentException.class, () -> p.insert(first, values("bad URI")));
        rejects(IllegalArgumentException.class, () -> p.getType(Uri.parse("content://user_dictionary/missing")));
        rejects(IllegalArgumentException.class, () -> p.query(Uri.parse("content://user_dictionary/missing"), null, null, null, null));
        rejects(IllegalArgumentException.class, () -> p.query(Words.CONTENT_URI, new String[]{"missing"}, null, null, null));
        rejects(IllegalArgumentException.class, () -> p.query(Words.CONTENT_URI, null, "frequency = ?", new String[]{"1", "extra"}, null));
    }

    private static void clientAndBatch(ContentProvider p, TestContext context) throws Exception {
        int before = count(p);
        Words.addWord(context, "helper-high", 999, "hh", Locale.forLanguageTag("he-IL"));
        Words.addWord(context, "helper-low", -5, null, null);
        Words.addWord(context, "", 10, null, null);
        check(count(p) == before + 2, "Public helper skips empty word");
        try (Cursor c = p.query(Words.CONTENT_URI, new String[]{Words.FREQUENCY, Words.LOCALE, Words.SHORTCUT},
                "word = ?", new String[]{"helper-high"}, null)) {
            check(c.moveToFirst() && c.getInt(0) == 255 && "he_IL".equals(c.getString(1)) && "hh".equals(c.getString(2)), "Public helper clamps high frequency and locale");
        }
        try (Cursor c = p.query(Words.CONTENT_URI, new String[]{Words.FREQUENCY, Words.LOCALE},
                "word = ?", new String[]{"helper-low"}, null)) {
            check(c.moveToFirst() && c.getInt(0) == 0 && c.isNull(1), "Public helper clamps low frequency and all locales");
        }
        check(p.bulkInsert(Words.CONTENT_URI, new ContentValues[]{values("bulk-a"), values("bulk-b")}) == 2, "Inherited bulk insert");
        ArrayList<ContentProviderOperation> operations = new ArrayList<>();
        operations.add(ContentProviderOperation.newInsert(Words.CONTENT_URI).withValues(values("batch-word")).build());
        operations.add(ContentProviderOperation.newUpdate(Words.CONTENT_URI).withValue(Words.FREQUENCY, 88)
                .withSelection("_id=?", new String[]{""}).withSelectionBackReference(0, 0).withExpectedCount(1).build());
        ContentProviderResult[] results = p.applyBatch(operations);
        check(results.length == 2 && results[0].uri != null && results[1].count == 1, "Batch URI back-reference");
        try (Cursor c = p.query(results[0].uri, new String[]{Words.FREQUENCY}, null, null, null)) {
            check(c.moveToFirst() && c.getInt(0) == 88, "Batch update follows actual inserted row");
        }
        operations.clear();
        operations.add(ContentProviderOperation.newInsert(Words.CONTENT_URI).withValues(values("batch-prefix-survives")).build());
        operations.add(ContentProviderOperation.newAssertQuery(Words.CONTENT_URI)
                .withSelection("word=?", new String[]{"definitely-absent"}).withExpectedCount(1).build());
        rejects(OperationApplicationException.class, () -> p.applyBatch(operations));
        try (Cursor c = p.query(Words.CONTENT_URI, new String[]{Words._ID}, "word=?",
                new String[]{"batch-prefix-survives"}, null)) {
            check(c.getCount() == 1, "AOSP inherited batch has partial side effects, not invented atomicity");
        }
        CancellationSignal canceled = new CancellationSignal(); canceled.cancel();
        try (Cursor c = p.query(Words.CONTENT_URI, new String[]{Words._ID}, null, null, null, canceled)) {
            check(c != null && c.getCount() == count(p), "AOSP legacy provider ignores query cancellation");
        }
        Bundle args = new Bundle();
        args.putString(ContentResolver.QUERY_ARG_SQL_SELECTION, "word=?");
        args.putStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS, new String[]{"batch-word"});
        try (Cursor c = p.query(Words.CONTENT_URI, new String[]{Words.FREQUENCY}, args, null)) {
            check(c.moveToFirst() && c.getInt(0) == 88, "Bundle query uses normal SQL selection");
        }
    }

    private static void persistenceAndUpgrade(String className, File directory) throws Exception {
        File file = new File(directory, "persistent.db");
        ContentProvider p = create(className, new TestContext(file));
        ContentValues values = values("persist-שלום"); values.put(Words.LOCALE, "he"); values.put(Words.SHORTCUT, "ps");
        Uri uri = p.insert(Words.CONTENT_URI, values); close(p);
        p = create(className, new TestContext(file));
        try (Cursor c = p.query(uri, new String[]{Words.WORD, Words.LOCALE, Words.SHORTCUT}, null, null, null)) {
            check(c.moveToFirst() && "persist-שלום".equals(c.getString(0)) && "he".equals(c.getString(1)) && "ps".equals(c.getString(2)), "SQLite persistence after close/reopen");
        } finally { close(p); }
        File old = new File(directory, "version1.db");
        try (SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(old, null)) {
            db.execSQL("CREATE TABLE words (_id INTEGER PRIMARY KEY, word TEXT, frequency INTEGER, locale TEXT, appid INTEGER)");
            db.execSQL("INSERT INTO words VALUES(31, 'old-word', 17, 'en', 0)");
            db.setVersion(1);
        }
        p = create(className, new TestContext(old));
        try (Cursor c = p.query(ContentUris.withAppendedId(Words.CONTENT_URI, 31), null, null, null, null)) {
            check(c.moveToFirst() && "old-word".equals(c.getString(c.getColumnIndexOrThrow(Words.WORD))), "v1 upgrade preserves row");
            check(c.isNull(c.getColumnIndexOrThrow(Words.SHORTCUT)), "v1 upgrade adds nullable shortcut");
        } finally { close(p); }
        try (SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(old, null)) {
            check(db.getVersion() == 2, "Database schema version");
        }
    }

    public static void main(String[] args) throws Exception {
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            error.printStackTrace(System.err); System.exit(1);
        });
        String className = args.length > 0 ? args[0] : "com.gesher.server.UserDictionaryProvider";
        System.out.println("UserDictionaryContractTest starting: " + className); System.out.flush();
        // Cursor observer registration is the declared local service boundary.
        // SQLite, CursorWindow and provider/ContentResolver methods are real ART.
        Field contentService = ContentResolver.class.getDeclaredField("sContentService");
        contentService.setAccessible(true);
        contentService.set(null, new android.content.IContentService.Default());
        // Never enqueue work in the reference device's real backup service.
        android.app.backup.BackupManager.sService = new android.app.backup.IBackupManager.Default();
        File directory = Files.createTempDirectory("user-dictionary-contract-").toFile();
        ContentProvider provider = null;
        try {
            TestContext context = new TestContext(new File(directory, "dictionary.db"));
            provider = create(className, context);
            System.out.println("UserDictionaryContractTest provider attached"); System.out.flush();
            queryAndCrud(provider, context);
            System.out.println("UserDictionaryContractTest CRUD passed"); System.out.flush();
            clientAndBatch(provider, context);
            System.out.println("UserDictionaryContractTest client and batch passed"); System.out.flush();
            persistenceAndUpgrade(className, directory);
            System.out.println("UserDictionaryContractTest all passed: " + checks + " checks; real SQLite; " + className);
        } finally {
            if (provider != null) close(provider);
            File[] files = directory.listFiles();
            if (files != null) for (File file : files) file.delete();
            directory.delete();
        }
        // app_process otherwise waits for Android framework worker threads that
        // outlive a standalone SQLite fixture. All owned rows/files are closed.
        System.exit(0);
    }
}
