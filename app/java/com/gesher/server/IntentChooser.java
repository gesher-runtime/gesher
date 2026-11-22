/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.ClipData;
import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.LabeledIntent;
import android.content.pm.ResolveInfo;
import android.os.Bundle;
import android.os.Parcelable;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.function.BooleanSupplier;

/** ACTION_CHOOSER's immutable resolved choices; selection is always a trusted host UI action. */
final class IntentChooser {
    static final int MAX_OPTIONS = 256;
    static final int CANCELED = -1;
    static final int UNAVAILABLE = -3;
    interface Dialog {
        boolean available();
        int choose(String requester, String title, String[] labels, boolean[] enabled, BooleanSupplier active);
    }
    static final Dialog HOST = new Dialog() {
        public boolean available() {
            try { AccountSecretStore.ensureLoaded(); return nativeAvailable(); }
            catch (LinkageError | SecurityException unavailable) { return false; }
        }
        public int choose(String requester, String title, String[] labels, boolean[] enabled, BooleanSupplier active) {
            return nativeChoose(requester, title, labels, enabled, active);
        }
    };
    private static native boolean nativeAvailable();
    private static native int nativeChoose(String requester, String title, String[] labels,
            boolean[] enabled, BooleanSupplier active);

    static boolean handles(Intent intent) {
        // An explicit component/package keeps ordinary app routing, even if that
        // app uses ACTION_CHOOSER for an activity of its own.
        return intent != null && Intent.ACTION_CHOOSER.equals(intent.getAction())
                && intent.getComponent() == null && intent.getPackage() == null;
    }

    static final class Option {
        final Intent intent;
        final ComponentName component;
        final ApkPackage code;
        final ActivityInfo info;
        final String label;
        Option(Intent intent, ResolveInfo resolved, ApkPackage code, CharSequence suppliedLabel) {
            this.intent = intent;
            this.info = new ActivityInfo(resolved.activityInfo);
            this.component = new ComponentName(info.packageName, info.name);
            this.intent.setComponent(component);
            this.code = code;
            CharSequence display = suppliedLabel != null ? suppliedLabel
                    : resolved.nonLocalizedLabel != null ? resolved.nonLocalizedLabel
                    : info.nonLocalizedLabel != null ? info.nonLocalizedLabel : info.name;
            this.label = text(display, info.name, 1200) + " (" + info.packageName + ")"
                    + (suspended() ? " — suspended" : "");
        }
        boolean suspended() {
            return info.applicationInfo != null
                    && (info.applicationInfo.flags & ApplicationInfo.FLAG_SUSPENDED) != 0;
        }
        boolean current(PackageManagerService pm, String caller) {
            synchronized (pm) {
                if (suspended() || code == null || pm.get(component.getPackageName()) != code
                        || pm.isRemovalPending(component.getPackageName())) return false;
                ActivityInfo now = pm.getActivityInfo(component, 0, 0);
                return eligible(pm, caller, now) && now.applicationInfo != null
                        && (now.applicationInfo.flags & ApplicationInfo.FLAG_SUSPENDED) == 0;
            }
        }
    }

    final Intent chooser;
    final List<Option> options = new ArrayList<>();
    final String title;
    final IntentSender callback;
    final Intent documentTarget;
    final boolean autoLaunch;
    final boolean unsupported;
    private final android.content.ContentResolver resolver;

    IntentChooser(PackageManagerService pm, String caller, Intent incoming) {
        this(pm, caller, incoming, null);
    }

    IntentChooser(PackageManagerService pm, String caller, Intent incoming,
            android.content.ContentResolver resolver) {
        this.resolver = resolver;
        chooser = snapshot(incoming);
        Object raw = chooser.getExtra(Intent.EXTRA_INTENT);
        if (!(raw instanceof Intent)) throw new IllegalArgumentException("ACTION_CHOOSER requires an Intent");
        Intent target = (Intent) raw;
        rejectRecursive(target);
        title = text(chooser.getCharSequenceExtra(Intent.EXTRA_TITLE), "Choose an app", 1024);
        Object sender = chooser.getExtra(Intent.EXTRA_CHOSEN_COMPONENT_INTENT_SENDER);
        if (sender != null && !(sender instanceof IntentSender)) {
            throw new IllegalArgumentException("Invalid chosen component IntentSender");
        }
        callback = (IntentSender) sender;
        documentTarget = target.getSelector() == null && DocumentPicker.handles(target)
                && callback == null && !requested(chooser, "android.intent.extra.CHOOSER_RESULT_INTENT_SENDER")
                ? snapshot(target) : null;
        // These extras change the application's flow. Until their contracts are implemented,
        // fail the request rather than silently skipping refinement or custom actions.
        unsupported = requested(chooser, Intent.EXTRA_CHOOSER_REFINEMENT_INTENT_SENDER)
                || requested(chooser, "android.intent.extra.CHOOSER_CUSTOM_ACTIONS")
                || requested(chooser, "android.intent.extra.CHOOSER_MODIFY_SHARE_ACTION")
                || requested(chooser, "android.intent.extra.CHOOSER_TARGETS");
        Set<ComponentName> excluded = new HashSet<>();
        for (Parcelable item : array(chooser, Intent.EXTRA_EXCLUDE_COMPONENTS)) {
            if (!(item instanceof ComponentName)) throw new IllegalArgumentException("Invalid excluded component");
            excluded.add((ComponentName) item);
        }
        Parcelable[] initials = array(chooser, Intent.EXTRA_INITIAL_INTENTS);
        // Android documents a maximum of two initial choices. Resolve each; unresolved
        // actions are not executable menu entries. Entries after the limit are ignored.
        for (int i = 0; i < Math.min(2, initials.length); ++i) {
            if (!(initials[i] instanceof Intent)) throw new IllegalArgumentException("Invalid initial intent");
            Intent initial = (Intent) initials[i];
            rejectRecursive(initial);
            query(pm, initial, matches -> {
                for (ResolveInfo resolved : matches) {
                    if (add(pm, caller, initial, resolved, excluded)) break;
                }
            });
        }
        Set<ComponentName> targetComponents = new HashSet<>();
        addTargets(pm, caller, target, excluded, targetComponents);
        for (Parcelable extra : array(chooser, Intent.EXTRA_ALTERNATE_INTENTS)) {
            if (!(extra instanceof Intent)) throw new IllegalArgumentException("Invalid alternate intent");
            rejectRecursive((Intent) extra);
            addTargets(pm, caller, (Intent) extra, excluded, targetComponents);
        }
        autoLaunch = shouldAutoLaunch(chooser, options);
    }

    static boolean shouldAutoLaunch(Intent chooser, List<Option> options) {
        return options.size() == 1 && !options.get(0).suspended()
                && chooser.getBooleanExtra(Intent.EXTRA_AUTO_LAUNCH_SINGLE_CHOICE, true);
    }

    private void addTargets(PackageManagerService pm, String caller, Intent target,
            Set<ComponentName> excluded, Set<ComponentName> components) {
        query(pm, target, matches -> {
            for (ResolveInfo ri : matches) {
                if (ri.activityInfo == null) continue;
                ComponentName component = new ComponentName(ri.activityInfo.packageName, ri.activityInfo.name);
                if (!components.contains(component) && add(pm, caller, target, ri, excluded)) components.add(component);
            }
        });
    }

    private boolean add(PackageManagerService pm, String caller, Intent base, ResolveInfo ri,
            Set<ComponentName> excluded) {
        ActivityInfo info = ri.activityInfo;
        if (!eligible(pm, caller, info)) return false;
        ComponentName component = new ComponentName(info.packageName, info.name);
        if (excluded.contains(component)) return false;
        // Host browser targets do not have an Android activity/result/grant owner. They
        // require a separate external-handler contract, never an invented activity launch.
        ApkPackage code = pm.get(info.packageName);
        if (code == null) return false;
        if (options.size() == MAX_OPTIONS) throw new IllegalArgumentException("Too many chooser options");
        ApplicationInfo callerInfo = pm.getApplicationInfo(caller == null ? "android" : caller, 0, 0);
        if (callerInfo == null) throw new SecurityException("Chooser caller package was removed");
        Intent launch = prepare(base, chooser, callerInfo);
        Bundle replacements = chooser.getBundleExtra(Intent.EXTRA_REPLACEMENT_EXTRAS);
        if (replacements != null) {
            Bundle extras = replacements.getBundle(info.packageName);
            if (extras != null) launch.putExtras(extras);
        }
        options.add(new Option(launch, ri, code, base instanceof LabeledIntent
                ? ((LabeledIntent)base).getNonLocalizedLabel() : null));
        return true;
    }

    static boolean eligible(PackageManagerService pm, String caller, ActivityInfo info) {
        if (info == null || !info.enabled || info.applicationInfo == null || !info.applicationInfo.enabled) return false;
        int uid = caller == null ? android.os.Process.SYSTEM_UID : pm.getPackageUid(caller, 0, 0);
        return pm.permissions().checkComponentPermission(info.permission, uid, info.applicationInfo.uid, info.exported)
                == PackageManager.PERMISSION_GRANTED;
    }

    private void query(PackageManagerService pm, Intent original,
            java.util.function.Consumer<List<ResolveInfo>> resolved) {
        Intent effective = original.getComponent() == null && original.getSelector() != null
                ? original.getSelector() : original;
        String type = effective.getType();
        if (type == null && effective.getData() != null && "content".equals(effective.getScheme()) && resolver != null) {
            // Android's privileged chooser can discover protected MIME metadata.
            // Never guess a type or rewrite the outer payload. Only this lookup
            // uses server authority, outside the package/launch monitors; the
            // selected activity's permission and URI grants still use its creator.
            long identity = android.os.Binder.clearCallingIdentity();
            try {
                type = effective.resolveTypeIfNeeded(resolver);
            } finally { android.os.Binder.restoreCallingIdentity(identity); }
        }
        synchronized (pm) {
            // Keep each candidate's resolved metadata and captured package generation
            // together, while allowing provider code to reenter before this point.
            resolved.accept(pm.queryIntentActivities(effective, type,
                    PackageManager.MATCH_DEFAULT_ONLY | PackageManager.GET_META_DATA, 0).getList());
        }
    }

    static Intent prepare(Intent target, Intent chooser, ApplicationInfo callerInfo) {
        Intent launch = snapshot(target);
        // Only the URI-grant envelope belongs to the nested target. In particular the
        // wrapper's MIME/data, CLEAR_TASK, and FORWARD_RESULT cannot rewrite that target.
        int grants = chooser.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        launch.addFlags(grants | Intent.FLAG_ACTIVITY_FORWARD_RESULT | Intent.FLAG_ACTIVITY_PREVIOUS_IS_TOP);
        if (launch.getClipData() == null && chooser.getClipData() != null) launch.setClipData(chooser.getClipData());
        // Use the framework's own SEND/SEND_MULTIPLE migration. It keeps each URI,
        // text/HTML item and declared MIME type, including mismatched-list behavior.
        // The framework also attributes implicit-grant policy/metrics to the real
        // sender UID. This metadata-only context never participates in camera or
        // content-provider conversion: only the two SEND actions enter here.
        if (Intent.ACTION_SEND.equals(launch.getAction()) || Intent.ACTION_SEND_MULTIPLE.equals(launch.getAction())) {
            // A Binder-delivered target may still hold a parcelled Bundle. The
            // framework migration deliberately refuses that state. Materialize
            // its outer map before migration; unrelated Parcelable values remain
            // lazy and the framework still validates the typed SEND payload.
            Bundle extras = launch.getExtras();
            if (extras != null) {
                extras.size();
                launch.replaceExtras(extras);
            }
            ApplicationInfo origin = new ApplicationInfo(callerInfo);
            launch.migrateExtraStreamToClipData(new android.content.ContextWrapper(null) {
                @Override public ApplicationInfo getApplicationInfo() { return origin; }
                @Override public String getPackageName() { return origin.packageName; }
            });
        }
        return launch;
    }

    private static boolean requested(Intent intent, String key) {
        Object value = intent.getExtra(key);
        if (value instanceof Object[]) return ((Object[])value).length != 0;
        if (value instanceof java.util.Collection<?>) return !((java.util.Collection<?>)value).isEmpty();
        return value != null;
    }

    private static Parcelable[] array(Intent intent, String key) {
        Object value = intent.getExtra(key);
        if (value == null) return new Parcelable[0];
        if (!(value instanceof Parcelable[])) throw new IllegalArgumentException("Invalid chooser array: " + key);
        return (Parcelable[]) value;
    }
    private static void rejectRecursive(Intent intent) {
        if (handles(intent) || intent.getSelector() != null && handles(intent.getSelector())) {
            throw new IllegalArgumentException("A chooser cannot contain another chooser");
        }
    }
    static Intent snapshot(Intent intent) {
        return snapshot(intent, new java.util.IdentityHashMap<>());
    }
    private static Intent snapshot(Intent intent, java.util.IdentityHashMap<Intent, Intent> copies) {
        Intent existing = copies.get(intent);
        if (existing != null) return existing;
        if (copies.size() >= 64) throw new IllegalArgumentException("Chooser intent graph is too large");
        Intent copy = new Intent(intent);
        copies.put(intent, copy);
        Bundle extras = intent.getExtras();
        if (extras != null) copy.replaceExtras(extras.deepCopy());
        if (intent.getSelector() != null) copy.setSelector(snapshot(intent.getSelector(), copies));
        ClipData original = intent.getClipData();
        if (original != null) {
            ClipData clip = new ClipData(original);
            for (int i = 0; i < clip.getItemCount(); ++i) {
                ClipData.Item item = original.getItemAt(i);
                ClipData.Item cloned = item.getIntent() == null ? new ClipData.Item(item)
                        : new ClipData.Item(item.getText(), item.getHtmlText(), snapshot(item.getIntent(), copies), item.getUri());
                clip.setItemAt(i, cloned);
            }
            copy.setClipData(clip);
        }
        return copy;
    }
    private static String text(CharSequence input, String fallback, int limit) {
        String value = input == null || input.length() == 0 ? fallback : input.toString();
        value = value.replace('\0', ' ').replace('\r', ' ').replace('\n', ' ');
        return value.substring(0, Math.min(limit, value.length()));
    }
}
