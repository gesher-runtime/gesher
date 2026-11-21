/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ContentProviderHolder;
import android.app.AppOpsManager;
import android.content.ComponentName;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Binder;
import android.os.Process;
import android.provider.Settings;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The {@code settings} provider, as a stand-in for SettingsProvider: the
 * {@code Settings.System}, {@code Secure} and {@code Global} tables in memory, with the
 * values a freshly set up device has for the settings apps and the framework read most,
 * answered through the call() fast path and the same durable rows for CRUD. Changed
 * values notify observers after publication. No generation tracking: every read asks
 * the provider. Per-user state and restore metadata remain outside this provider.
 *
 * <p>It is a framework ContentProvider in the system server, so its Binder is the
 * framework's own transport ({@code ContentProvider.Transport}), served across processes.
 */
final class SettingsProvider extends ContentProvider {
    static final String AUTHORITY = Settings.AUTHORITY;
    static final String SELECT_DEFAULT_CHECKER = "gesher:select-default-spell-checker";

    private static final String SYSTEM = "system";
    private static final String SECURE = "secure";
    private static final String GLOBAL = "global";

    // Guarded by this. Table name -> setting -> value.
    private final Map<String, Map<String, String>> mTables = new LinkedHashMap<>();
    private final SettingsStore mStore;
    // Older unprotected profiles could write arbitrary Secure rows. Spell-checker
    // consent is read only from this store, whose keys require privileged writes.
    private final Map<String, Map<String, String>> mTextTables = new LinkedHashMap<>();
    private final SettingsStore mTextStore;
    // Guarded by this. An automatic choice must not replace a concurrent privileged write.
    private long mTextRevision;
    private static final String SELECTED_CHECKER = Settings.Secure.SELECTED_SPELL_CHECKER;
    private static final String CHECKER_SUBTYPE = Settings.Secure.SELECTED_SPELL_CHECKER_SUBTYPE;
    private static final String CHECKER_ENABLED = Settings.Secure.SPELL_CHECKER_ENABLED;
    private static final String[] COLUMNS = {"_id", "name", "value", "is_preserved_in_restore"};
    private static final Pattern PARAM = Pattern.compile("\\s*name\\s*=\\s*\\?\\s*");
    private static final Pattern LITERAL = Pattern.compile("\\s*name\\s*=\\s*(['\"])(.*)\\1\\s*");
    private final ProviderInfo mInfo = new ProviderInfo();
    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private volatile java.util.function.BooleanSupplier mAutomaticCheckerSelector;

    void setAutomaticCheckerSelector(java.util.function.BooleanSupplier selector) {
        mAutomaticCheckerSelector = selector;
    }

    static final class SpellCheckerSettings {
        final String component;
        final String identity;
        final int subtype;
        final boolean enabled;
        final long revision;

        SpellCheckerSettings(String component, String identity, int subtype, boolean enabled) {
            this(component, identity, subtype, enabled, -1);
        }

        SpellCheckerSettings(String component, String identity, int subtype, boolean enabled,
                long revision) {
            this.component = component;
            this.identity = identity;
            this.subtype = subtype;
            this.enabled = enabled;
            this.revision = revision;
        }
    }

    synchronized SpellCheckerSettings spellCheckerSettings() {
        Map<String, String> rows = mTextTables.get(SECURE);
        String selected = rows.get(SELECTED_CHECKER);
        int delimiter = selected == null ? -1 : selected.indexOf('\n');
        return new SpellCheckerSettings(delimiter < 0 ? null : selected.substring(0, delimiter),
                delimiter < 0 ? null : selected.substring(delimiter + 1),
                number(visible(SECURE, CHECKER_SUBTYPE, rows.get(CHECKER_SUBTYPE)), 0),
                number(rows.get(CHECKER_ENABLED), 1) == 1, mTextRevision);
    }

    /** Server-local default selection, using the same durable rows as an explicit choice. */
    boolean initializeSpellChecker(SpellCheckerSettings expected, SpellCheckerCatalog.Entry entry) {
        String value = "";
        if (entry != null) {
            if (!entry.owner.isSystemAddon()) throw new SecurityException("Automatic checker is not a system provider");
            String identity = entry.codeIdentity();
            if (identity == null) return false;
            value = entry.info.getId() + "\n" + identity;
        }
        synchronized (this) {
            if (mTextRevision != expected.revision) return false;
            if (entry != null && (mPm == null || mPm.get(entry.owner.packageName) != entry.owner
                    || mPm.isRemovalPending(entry.owner.packageName)
                    || !entry.owner.componentEnabled(entry.info.getComponent().getClassName(),
                            ApkPackage.ComponentKind.SERVICE))) return false;
            if (mTextStore == null) {
                // Single-process profiles have no persistent privileged consent store.
                mTextTables.get(SECURE).put(SELECTED_CHECKER, value);
                mTextTables.get(SECURE).put(CHECKER_SUBTYPE, "0");
            } else {
                // A newer selection row also invalidates an older subtype atomically.
                mTextStore.put(SECURE, SELECTED_CHECKER, value, false);
            }
            mTextRevision++;
        }
        notifySetting(SECURE, SELECTED_CHECKER);
        return true;
    }

    private static int number(String value, int fallback) {
        try { return value == null ? fallback : Integer.parseInt(value); }
        catch (NumberFormatException invalid) { return fallback; }
    }

    private static boolean textKey(String table, String name) {
        return SECURE.equals(table) && (SELECTED_CHECKER.equals(name) || CHECKER_SUBTYPE.equals(name)
                || CHECKER_ENABLED.equals(name));
    }

    private boolean checkerSubtypePredatesSelection() {
        Map<String, String> rows = mTextTables.get(SECURE);
        return mTextStore != null && rows.containsKey(CHECKER_SUBTYPE)
                && rows.containsKey(SELECTED_CHECKER)
                && mTextStore.id(SECURE, CHECKER_SUBTYPE) < mTextStore.id(SECURE, SELECTED_CHECKER);
    }

    private String visible(String table, String name, String value) {
        if (value == null || !SECURE.equals(table)) return value;
        if (SELECTED_CHECKER.equals(name)) {
            int delimiter = value.indexOf('\n');
            return delimiter < 0 ? value : value.substring(0, delimiter);
        }
        if (CHECKER_SUBTYPE.equals(name) && checkerSubtypePredatesSelection()) return "0";
        return value;
    }

    private String verifiedCheckerSelection(String value) {
        if (value == null || value.isEmpty() || "null".equals(value)) return value;
        ComponentName component = ComponentName.unflattenFromString(value);
        ApkPackage owner = component == null || mPm == null ? null : mPm.get(component.getPackageName());
        if (owner == null || mPm.isRemovalPending(owner.packageName)
                || owner.serviceInfo(component.getClassName(), PackageManager.GET_META_DATA) == null) {
            throw new IllegalArgumentException("Spell-checker component is unavailable");
        }
        String identity = owner.codeIdentity();
        if (identity == null || mPm.get(owner.packageName) != owner || mPm.isRemovalPending(owner.packageName)) {
            throw new IllegalArgumentException("Spell-checker package changed during selection");
        }
        return component.flattenToShortString() + "\n" + identity;
    }

    /** The provider of this process (the system server's), for {@link #putSystem}. */
    private static volatile SettingsProvider sInstance;

    /** Server-local view of the actual secure setting; absent configuration stays absent. */
    static String secureString(String name) {
        SettingsProvider provider = sInstance;
        if (provider == null) return null;
        synchronized (provider) { return provider.mTables.get(SECURE).get(name); }
    }

    /** Server-local numeric global setting, with the same malformed-value fallback as Settings. */
    static long globalLong(String name, long defaultValue) {
        SettingsProvider provider = sInstance;
        if (provider == null) return defaultValue;
        String value;
        synchronized (provider) {
            value = provider.mTables.get(GLOBAL).get(name);
        }
        if (value == null) return defaultValue;
        try {
            return Long.parseLong(value);
        } catch (NumberFormatException invalid) {
            return defaultValue;
        }
    }

    /**
     * Sets a Settings.System value from the system server (locale track: TIME_12_24 when
     * Windows' time format changes; ADR 0049).
     */
    static void putSystem(String name, String value) {
        SettingsProvider p = sInstance;
        if (p == null) return;
        p.put(SYSTEM, name, value, false);
    }

    SettingsProvider() {
        this(null, null);
    }

    SettingsProvider(PackageManagerService pm, ActivityTaskManagerService atm) {
        this(pm, atm, new File(Device.dataDir(), "system/settings.bin"));
    }

    SettingsProvider(PackageManagerService pm, ActivityTaskManagerService atm, File file) {
        mPm = pm;
        mCallers = pm == null ? null : new CallerIdentity(pm, atm);
        for (String table : new String[] {SYSTEM, SECURE, GLOBAL}) {
            mTables.put(table, new LinkedHashMap<>());
            mTextTables.put(table, new LinkedHashMap<>());
        }
        Map<String, String> system = mTables.get(SYSTEM);
        system.put(Settings.System.FONT_SCALE, "1.0");
        system.put(Settings.System.TIME_12_24,  // Windows' time format (rtl track)
                com.gesher.app.locale.SystemLocale.is24Hour() ? "24" : "12");
        system.put(Settings.System.SCREEN_OFF_TIMEOUT, "60000");
        system.put(Settings.System.SCREEN_BRIGHTNESS, "128");
        system.put(Settings.System.HAPTIC_FEEDBACK_ENABLED, "0");
        system.put(Settings.System.SOUND_EFFECTS_ENABLED, "0");
        system.put(Settings.System.ACCELEROMETER_ROTATION, "0");
        system.put(Settings.System.TEXT_AUTO_CAPS, "1");
        system.put(Settings.System.TEXT_AUTO_PUNCTUATE, "1");
        system.put(Settings.System.TEXT_SHOW_PASSWORD, "1");
        AudioService.putDefaultSounds(system);  // the default ringtones (docs/media.md)
        Map<String, String> secure = mTables.get(SECURE);
        secure.put(Settings.Secure.ANDROID_ID, "6765736865720000");  // "gesher" in hex
        secure.put(Settings.Secure.USER_SETUP_COMPLETE, "1");
        secure.put(Settings.Secure.ACCESSIBILITY_ENABLED, "0");
        secure.put(Settings.Secure.LOCATION_MODE, "0");
        secure.put(Settings.Secure.SHOW_IME_WITH_HARD_KEYBOARD, "0");
        secure.put("install_non_market_apps", "1");
        secure.put("ui_night_mode", "1");
        Map<String, String> global = mTables.get(GLOBAL);
        global.put(Settings.Global.DEVICE_PROVISIONED, "1");
        global.put("package_verifier_enable", "0");
        global.put("package_verifier_user_consent", "1");
        global.put(Settings.Global.AUTO_TIME, "1");
        global.put(Settings.Global.AUTO_TIME_ZONE, "1");
        global.put(Settings.Global.AIRPLANE_MODE_ON, "0");
        global.put(Settings.Global.ADB_ENABLED, "0");
        global.put(Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, "0");
        global.put(Settings.Global.ALWAYS_FINISH_ACTIVITIES, "0");
        global.put(Settings.Global.ANIMATOR_DURATION_SCALE, "1.0");
        global.put(Settings.Global.TRANSITION_ANIMATION_SCALE, "1.0");
        global.put(Settings.Global.WINDOW_ANIMATION_SCALE, "1.0");
        global.put(Settings.Global.STAY_ON_WHILE_PLUGGED_IN, "0");
        global.put(Settings.Global.WIFI_ON, "0");
        global.put(Settings.Global.BLUETOOTH_ON, "0");
        global.put(Settings.Global.DATA_ROAMING, "0");
        global.put(Settings.Global.DEVICE_NAME, "Gesher");

        mStore = new SettingsStore(file, mTables);
        // Single-process mode executes app code in the server's own PID and cannot
        // authenticate a privileged writer. Never expose its process to host selection state.
        mTextStore = mPm != null && mPm.hasTransientPackageState() ? null
                : new SettingsStore(new File(file.toString() + ".spell-checker"), mTextTables);
        if (mPm != null && mTextStore != null) mPm.onPackageRemoval(this::removeCheckerSelection);
        ApplicationInfo ai = new ApplicationInfo();
        ai.packageName = "com.android.providers.settings";
        ai.processName = "system";
        ai.uid = Process.SYSTEM_UID;
        ai.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_PERSISTENT;
        ai.enabled = true;
        mInfo.applicationInfo = ai;
        mInfo.packageName = ai.packageName;
        mInfo.name = "com.android.providers.settings.SettingsProvider";
        mInfo.authority = AUTHORITY;
        mInfo.processName = ai.processName;
        mInfo.exported = true;
        mInfo.enabled = true;
        mInfo.multiprocess = false;
        mInfo.flags = ProviderInfo.FLAG_SINGLE_USER;
        // Publish only after all tables are available to server-local readers.
        sInstance = this;
    }

    /** What getContentProvider answers for {@code settings}. */
    synchronized ContentProviderHolder holder() {
        if (getContext() == null) attachInfo(android.app.GesherSettingsContext.get(), mInfo);
        ContentProviderHolder h = new ContentProviderHolder(mInfo);
        h.provider = getIContentProvider();
        h.noReleaseNeeded = true;  // the provider lives as long as the system server
        return h;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    /** The table of a call method: GET_system → system. */
    private static String table(String method) {
        int i = method.indexOf('_');
        return i < 0 ? null : method.substring(i + 1);
    }

    private void enforceWrite(String table, String name) {
        boolean text = textKey(table, name);
        if (text && mTextStore == null) {
            throw new SecurityException("Spell-checker settings require the device system server");
        }
        int pid = Binder.getCallingPid();
        // A standalone APK and this provider share a PID. Its authenticated
        // application identity must take precedence over server-local authority.
        String actual = mCallers == null ? null : mCallers.packageForPid(pid);
        if (actual == null) {
            boolean local = pid == Process.myPid();
            if (local && (mPm == null || !mPm.hasTransientPackageState()
                    || Process.myUid() < Process.FIRST_APPLICATION_UID)) return;
            if (mCallers == null) throw new SecurityException("Settings writer identity unavailable");
            if (!local && mCallers.isSystem()) return;
            // An ordinary standalone process before binding or after retirement
            // does not become a privileged writer when its attachment is absent.
            throw new SecurityException("Unrecognized settings writer identity");
        }
        ApkPackage owner = mPm.get(actual);
        if (owner == null || mPm.isRemovalPending(actual) || owner.uid() != Binder.getCallingUid()) {
            throw new SecurityException("Unrecognized application Binder identity");
        }
        // AOSP's system-table special access is an AppOp for ordinary apps.
        // Secure/global still require the actual privileged permission; its holders
        // may also write system settings independently of the special-access AppOp.
        if (mPm.permissions().checkPermission(actual, android.Manifest.permission.WRITE_SECURE_SETTINGS)
                == android.content.pm.PackageManager.PERMISSION_GRANTED) return;
        if (SYSTEM.equals(table)) {
            int uid = Binder.getCallingUid();
            String attribution = getCallingAttributionTag();
            int mode;
            long identity = Binder.clearCallingIdentity();
            try {
                mode = mPm.appOps().noteOperation(AppOpsManager.OP_WRITE_SETTINGS, uid, actual,
                        attribution, false, null, false).getOpMode();
            } finally { Binder.restoreCallingIdentity(identity); }
            if (mode == AppOpsManager.MODE_ALLOWED || mode == AppOpsManager.MODE_DEFAULT
                    && mPm.permissions().checkPermission(actual, android.Manifest.permission.WRITE_SETTINGS)
                        == android.content.pm.PackageManager.PERMISSION_GRANTED) return;
        }
        throw new SecurityException("Settings write permission denied");
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        if (method == null) return null;
        if (SELECT_DEFAULT_CHECKER.equals(method)) {
            enforceWrite(SECURE, SELECTED_CHECKER);
            java.util.function.BooleanSupplier selector = mAutomaticCheckerSelector;
            if (selector == null) throw new IllegalStateException("Text services are unavailable");
            Bundle result = new Bundle();
            result.putBoolean("result", selector.getAsBoolean());
            return result;
        }
        String table = table(method);
        if (!mTables.containsKey(table)) {
            return method.startsWith("GET_") ? valueBundle(null) : null;
        }
        if (method.equals("GET_" + table)) {
            synchronized (this) {
                Map<String, String> rows = (textKey(table, arg) ? mTextTables : mTables).get(table);
                return valueBundle(visible(table, arg, rows.get(arg)));
            }
        }
        if (method.equals("PUT_" + table)) {
            enforceWrite(table, arg);
            put(table, arg, extras == null ? null : extras.getString("value"), false);
            // Retain the accepted-PUT diagnostic even when a durable value is unchanged.
            if (SettingsStore.validKey(arg)) SystemServer.log("settings: updated " + table + " key " + arg);
            return null;
        }
        if (method.equals("DELETE_" + table)) {
            enforceWrite(table, arg);
            Bundle result = new Bundle();
            result.putInt("result_rows_deleted", remove(table, arg) ? 1 : 0);
            return result;
        }
        if (method.equals("LIST_" + table)) {
            ArrayList<String> all = new ArrayList<>();
            synchronized (this) {
                for (Map.Entry<String, String> e : rows(table).entrySet()) {
                    all.add(e.getKey() + "=" + visible(table, e.getKey(), e.getValue()));
                }
            }
            Bundle result = new Bundle();
            result.putStringArrayList("result_settings_list", all);
            return result;
        }
        return null;
    }

    private static Bundle valueBundle(String value) {
        Bundle result = new Bundle(); result.putString("value", value); return result;
    }

    // Caller holds this monitor. Keep arbitrary historical checker rows out of all read paths.
    private Map<String, String> rows(String table) {
        Map<String, String> rows = mTables.get(table);
        if (!SECURE.equals(table)) return rows;
        Map<String, String> result = new LinkedHashMap<>(rows);
        result.keySet().removeIf(name -> textKey(table, name));
        result.putAll(mTextTables.get(table));
        return result;
    }

    private SettingsStore store(String table, String name) {
        return textKey(table, name) ? mTextStore : mStore;
    }

    private long rowId(String table, String name) {
        SettingsStore store = store(table, name);
        // Single-process profiles expose the server's initial text-service rows
        // without opening the privileged device store. Give those read-only rows
        // distinct IDs too; a normal default-projection query must remain usable.
        long id = store != null ? store.id(table, name)
                : SELECTED_CHECKER.equals(name) ? 0 : CHECKER_SUBTYPE.equals(name) ? 1 : 2;
        return textKey(table, name) ? (1L << 62) | id : id;
    }

    private boolean put(String table, String name, String value, boolean existingOnly) {
        synchronized (this) {
            if (SECURE.equals(table) && SELECTED_CHECKER.equals(name)) {
                value = verifiedCheckerSelection(value);
            }
            boolean newGeneration = SECURE.equals(table) && CHECKER_SUBTYPE.equals(name)
                    && checkerSubtypePredatesSelection();
            if (!store(table, name).put(table, name, value, existingOnly, newGeneration)) return false;
            if (textKey(table, name)) mTextRevision++;
        }
        notifySetting(table, name); return true;
    }

    private boolean remove(String table, String name) {
        synchronized (this) {
            if (SECURE.equals(table) && SELECTED_CHECKER.equals(name)
                    && mTextTables.get(SECURE).containsKey(SELECTED_CHECKER)) {
                mTextStore.remove(SECURE, CHECKER_SUBTYPE);
            }
            if (!store(table, name).remove(table, name)) return false;
            if (textKey(table, name)) mTextRevision++;
        }
        notifySetting(table, name); return true;
    }

    private void removeCheckerSelection(String packageName) {
        synchronized (this) {
            String component = visible(SECURE, SELECTED_CHECKER,
                    mTextTables.get(SECURE).get(SELECTED_CHECKER));
            if (component == null || !component.startsWith(packageName + "/")) return;
            // Keep a new selection generation, invalidating the previous subtype.
            mTextStore.put(SECURE, SELECTED_CHECKER, "", false);
            mTextRevision++;
        }
        notifySetting(SECURE, SELECTED_CHECKER);
    }

    private void notifySetting(String table, String name) {
        if (getContext() == null) return;
        long identity = Binder.clearCallingIdentity();
        try {
            getContext().getContentResolver().notifyChange(
                    Uri.withAppendedPath(Uri.parse("content://settings/" + table), name), null);
        } catch (RuntimeException failure) {
            SystemServer.log("settings: observer notification failed: " + failure);
        } finally { Binder.restoreCallingIdentity(identity); }
    }

    private static final class Arguments {
        final String table, name;
        Arguments(Uri uri, String selection, String[] args, boolean all) {
            List<String> segments = uri.getPathSegments();
            if (segments.isEmpty() || segments.size() > 2
                    || !(SYSTEM.equals(segments.get(0)) || SECURE.equals(segments.get(0))
                         || GLOBAL.equals(segments.get(0)))) {
                throw new IllegalArgumentException("Invalid settings URI: " + uri);
            }
            table = segments.get(0);
            if (segments.size() == 2 && selection == null && args == null) {
                name = segments.get(1); return;
            }
            if (segments.size() == 1) {
                if (selection == null && args == null && all) { name = null; return; }
                if (selection != null) {
                    String where = selection.trim();
                    if (where.startsWith("(") && where.endsWith(")")) {
                        where = where.substring(1, where.length() - 1);
                    }
                    if (PARAM.matcher(where).matches() && args != null && args.length == 1) {
                        name = args[0]; return;
                    }
                    Matcher literal = LITERAL.matcher(where);
                    if (literal.matches()) { name = literal.group(2); return; }
                }
            }
            throw new IllegalArgumentException("Settings supports only a single name selection");
        }
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder) {
        Arguments args = new Arguments(uri, selection, selectionArgs, true);
        String[] columns = projection == null ? COLUMNS.clone() : projection.clone();
        for (String column : columns) {
            if (!java.util.Arrays.asList(COLUMNS).contains(column)) {
                throw new IllegalArgumentException("Invalid settings column: " + column);
            }
        }
        MatrixCursor cursor = new MatrixCursor(columns);
        synchronized (this) {
            for (Map.Entry<String, String> entry : rows(args.table).entrySet()) {
                if (args.name != null && !args.name.equals(entry.getKey())) continue;
                Object[] row = new Object[columns.length];
                for (int i = 0; i < columns.length; i++) {
                    switch (columns[i]) {
                        case "_id":
                            row[i] = Long.toString(rowId(args.table, entry.getKey()));
                            break;
                        case "name": row[i] = entry.getKey(); break;
                        case "value": row[i] = visible(args.table, entry.getKey(), entry.getValue()); break;
                        case "is_preserved_in_restore": row[i] = "false"; break;
                        default: throw new AssertionError(columns[i]);
                    }
                }
                cursor.addRow(row);
            }
        }
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        Arguments args = new Arguments(uri, null, null, true);
        return "vnd.android.cursor." + (args.name == null || args.name.isEmpty() ? "dir/" : "item/")
                + args.table;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        Arguments args = new Arguments(uri, null, null, true);
        String name = values.getAsString("name");
        enforceWrite(args.table, name);
        return put(args.table, name, values.getAsString("value"), false)
                ? Uri.withAppendedPath(Uri.parse("content://settings/" + args.table), name) : null;
    }

    @Override
    public int bulkInsert(Uri uri, ContentValues[] values) {
        int count = 0;
        for (ContentValues row : values) if (insert(uri, row) != null) count++;
        return count;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        Arguments args = new Arguments(uri, selection, selectionArgs, false);
        enforceWrite(args.table, args.name);
        return remove(args.table, args.name) ? 1 : 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        Arguments args = new Arguments(uri, selection, selectionArgs, false);
        enforceWrite(args.table, args.name);
        if (!SettingsStore.validKey(values.getAsString("name"))) return 0;
        return put(args.table, args.name, values.getAsString("value"), true) ? 1 : 0;
    }
}
