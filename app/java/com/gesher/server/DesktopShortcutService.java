/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.Intent;
import android.content.IntentFilter;
import android.content.IntentSender;
import android.content.pm.IShortcutService;
import android.content.pm.ParceledListSlice;
import android.content.pm.ShortcutInfo;
import com.gesher.app.shell.AppShell;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/** Package-authenticated dynamic shortcuts, persisted with their publisher identity. */
class DesktopShortcutService extends IShortcutService.Default {
    private static final int MAX_SHORTCUTS = 15;
    private final PackageManagerService mPackages;
    private final CallerIdentity mCallers;

    DesktopShortcutService(PackageManagerService packages) { this(packages, new CallerIdentity(packages, null)); }
    DesktopShortcutService(PackageManagerService packages, CallerIdentity callers) {
        mPackages = packages; mCallers = callers;
        packages.onPackageRemoval(this::retire);
    }
    private synchronized void retire(String pkg) { AppShell.removeAllDynamicShortcuts(pkg); }
    private ApkPackage query(String pkg, int userId) {
        if (userId != 0) throw new IllegalArgumentException("Unknown shortcut user");
        Objects.requireNonNull(pkg, "packageName");
        mCallers.requirePackage(pkg);
        ApkPackage installed = mPackages.get(pkg);
        if (installed == null) throw new IllegalArgumentException("Unknown shortcut package");
        if (mPackages.isRemovalPending(pkg)) throw new SecurityException("Shortcut publisher is being removed");
        return installed;
    }
    private LinkedHashMap<String,ShortcutInfo> load(ApkPackage pkg) {
        LinkedHashMap<String,ShortcutInfo> result = new LinkedHashMap<>();
        for (ShortcutInfo info : AppShell.loadDynamicShortcuts(pkg.packageName, pkg.codeIdentity())) result.put(info.getId(), info);
        return result;
    }
    private ShortcutInfo validated(String pkg, ShortcutInfo source) {
        return validated(pkg, source, false);
    }
    private ShortcutInfo validated(String pkg, ShortcutInfo source, boolean forUpdate) {
        Objects.requireNonNull(source, "shortcut");
        if (!pkg.equals(source.getPackage()) || source.getUserId() != 0) throw new SecurityException("Shortcut publisher mismatch");
        if (source.getId() == null || source.getId().isEmpty() || source.getId().length() > 1024) throw new IllegalArgumentException("Shortcut requires an ID");
        if ((!forUpdate && source.getShortLabel() == null)
                || (source.getShortLabel() != null && source.getShortLabel().length() == 0)) throw new IllegalArgumentException("Shortcut requires a label");
        if (source.getActivity() != null && !pkg.equals(source.getActivity().getPackageName())) throw new SecurityException("Shortcut activity belongs to another package");
        Intent[] intents = source.getIntents();
        if ((!forUpdate && intents == null) || (intents != null && intents.length == 0)) throw new IllegalArgumentException("Shortcut requires an intent");
        if (intents != null) for (int i = 0; i < intents.length; i++) intents[i] = AppShell.dynamicIntent(pkg, intents[i]);
        ShortcutInfo copy = source.clone(ShortcutInfo.CLONE_REMOVE_FOR_CREATOR);
        copy.replaceFlags(ShortcutInfo.FLAG_DYNAMIC);
        if (intents != null) copy.setIntents(intents);
        return copy;
    }
    private List<ShortcutInfo> validated(String pkg, ParceledListSlice values) {
        return validated(pkg, values, false);
    }
    private List<ShortcutInfo> validated(String pkg, ParceledListSlice values, boolean forUpdate) {
        Objects.requireNonNull(values, "shortcuts");
        List<?> list = Objects.requireNonNull(values.getList(), "shortcut list");
        if (list.size() > MAX_SHORTCUTS) throw new IllegalArgumentException("Too many dynamic shortcuts");
        List<ShortcutInfo> result = new ArrayList<>();
        for (Object value : list) {
            if (!(value instanceof ShortcutInfo)) throw new IllegalArgumentException("Invalid shortcut entry");
            result.add(validated(pkg, (ShortcutInfo)value, forUpdate));
        }
        return result;
    }
    private boolean publish(ApkPackage owner, Map<String,ShortcutInfo> snapshot) {
        if (snapshot.size() > MAX_SHORTCUTS) throw new IllegalArgumentException("Too many dynamic shortcuts");
        if (mPackages.get(owner.packageName) != owner || mPackages.isRemovalPending(owner.packageName)) throw new SecurityException("Shortcut publisher changed");
        boolean stored = AppShell.syncDynamicShortcuts(owner.packageName, new ArrayList<>(snapshot.values()), owner.codeIdentity());
        if (mPackages.get(owner.packageName) != owner || mPackages.isRemovalPending(owner.packageName)) {
            AppShell.removeAllDynamicShortcuts(owner.packageName);
            throw new SecurityException("Shortcut publisher changed while publishing");
        }
        return stored;
    }
    @Override public boolean isRequestPinItemSupported(int userId, int type) { return false; }
    @Override public boolean requestPinShortcut(String pkg, ShortcutInfo info, IntentSender result, int userId) {
        query(pkg,userId); Objects.requireNonNull(info,"shortcut"); return false;
    }
    @Override public Intent createShortcutResultIntent(String pkg, ShortcutInfo info, int userId) {
        query(pkg,userId); Objects.requireNonNull(info,"shortcut"); return null;
    }
    @Override public synchronized ParceledListSlice getShortcuts(String pkg, int flags, int userId) {
        ApkPackage owner = query(pkg,userId);
        return new ParceledListSlice<>((flags & 1) == 0 ? new ArrayList<ShortcutInfo>() : new ArrayList<>(load(owner).values()));
    }
    @Override public ParceledListSlice getShareTargets(String pkg, IntentFilter filter, int userId) {
        query(pkg,userId); Objects.requireNonNull(filter,"filter"); return ParceledListSlice.emptyList();
    }
    @Override public boolean hasShareTargets(String caller,String pkg,int userId) { query(caller,userId); return false; }
    @Override public int getMaxShortcutCountPerActivity(String pkg,int userId) { query(pkg,userId); return MAX_SHORTCUTS; }
    @Override public int getRemainingCallCount(String pkg,int userId) { query(pkg,userId); return Integer.MAX_VALUE; }
    @Override public long getRateLimitResetTime(String pkg,int userId) { query(pkg,userId); return 0; }
    @Override public int getIconMaxDimensions(String pkg,int userId) { query(pkg,userId); return 256; }
    @Override public synchronized boolean setDynamicShortcuts(String pkg,ParceledListSlice values,int userId) {
        ApkPackage owner=query(pkg,userId); LinkedHashMap<String,ShortcutInfo> next=new LinkedHashMap<>();
        for (ShortcutInfo info:validated(pkg,values)) next.put(info.getId(),info);
        return publish(owner,next);
    }
    @Override public synchronized boolean addDynamicShortcuts(String pkg,ParceledListSlice values,int userId) {
        ApkPackage owner=query(pkg,userId); List<ShortcutInfo> incoming=validated(pkg,values);
        LinkedHashMap<String,ShortcutInfo> next=load(owner);
        for (ShortcutInfo info:incoming) next.put(info.getId(),info);
        return publish(owner,next);
    }
    @Override public synchronized boolean updateShortcuts(String pkg,ParceledListSlice values,int userId) {
        ApkPackage owner=query(pkg,userId); List<ShortcutInfo> incoming=validated(pkg,values,true);
        LinkedHashMap<String,ShortcutInfo> next=load(owner);
        for (ShortcutInfo info:incoming) {
            ShortcutInfo target=next.get(info.getId());
            if(target!=null) {
                // The loaded snapshot is detached from stored state. Android's
                // merge preserves omitted fields, unset ranks and stored flags.
                target.copyNonNullFieldsFrom(info);
                target.setTimestamp(System.currentTimeMillis());
            }
        }
        return publish(owner,next);
    }
    @Override public synchronized void pushDynamicShortcut(String pkg,ShortcutInfo info,int userId) {
        ApkPackage owner=query(pkg,userId); ShortcutInfo copy=validated(pkg,info);
        LinkedHashMap<String,ShortcutInfo> next=load(owner); next.put(copy.getId(),copy);
        if(next.size()>MAX_SHORTCUTS) {
            String victim=null; int rank=Integer.MIN_VALUE;
            for(ShortcutInfo candidate:next.values()) if(!candidate.getId().equals(copy.getId()) && candidate.getRank()>rank) { victim=candidate.getId();rank=candidate.getRank(); }
            next.remove(victim);
        }
        if(!publish(owner,next)) throw new IllegalStateException("Dynamic shortcut publication failed");
    }
    @Override public synchronized void removeDynamicShortcuts(String pkg,List<String> ids,int userId) {
        ApkPackage owner=query(pkg,userId); Objects.requireNonNull(ids,"shortcutIds");
        LinkedHashMap<String,ShortcutInfo> next=load(owner);
        for(String id:ids) next.remove(Objects.requireNonNull(id,"shortcutId"));
        if(!publish(owner,next)) throw new IllegalStateException("Dynamic shortcut removal failed");
    }
    @Override public synchronized void removeAllDynamicShortcuts(String pkg,int userId) {
        query(pkg,userId); AppShell.removeAllDynamicShortcuts(pkg);
    }
    @Override public void removeLongLivedShortcuts(String pkg,List<String> ids,int userId) { removeDynamicShortcuts(pkg,ids,userId); }
    private synchronized void enabled(String pkg,List<String> ids,int userId,boolean value) {
        ApkPackage owner=query(pkg,userId); Objects.requireNonNull(ids,"shortcutIds");
        LinkedHashMap<String,ShortcutInfo> next=load(owner);
        for(String id:ids) { ShortcutInfo info=next.get(Objects.requireNonNull(id,"shortcutId")); if(info!=null) {
            if(value) info.clearFlags(ShortcutInfo.FLAG_DISABLED); else info.addFlags(ShortcutInfo.FLAG_DISABLED);
        }}
        if(!publish(owner,next)) throw new IllegalStateException("Dynamic shortcut state update failed");
    }
    @Override public void disableShortcuts(String pkg,List<String> ids,CharSequence message,int messageId,int userId) { enabled(pkg,ids,userId,false); }
    @Override public void enableShortcuts(String pkg,List<String> ids,int userId) { enabled(pkg,ids,userId,true); }
    @Override public void reportShortcutUsed(String pkg,String id,int userId) { query(pkg,userId);Objects.requireNonNull(id,"shortcutId"); }
    private static SecurityException administrationDenied() { return new SecurityException("Shortcut administration requires host authority"); }
    @Override public void resetThrottling() { throw administrationDenied(); }
    @Override public void onApplicationActive(String pkg,int userId) { throw administrationDenied(); }
    @Override public byte[] getBackupPayload(int userId) { throw administrationDenied(); }
    @Override public void applyRestore(byte[] payload,int userId) { throw administrationDenied(); }
}
