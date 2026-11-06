/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.IPackageInstaller;
import android.content.pm.IPackageInstallerCallback;
import android.content.pm.IPackageInstallerSession;
import android.content.pm.PackageInstaller;
import android.content.pm.ParceledListSlice;
import android.content.pm.Signature;
import android.content.pm.parsing.ApkLite;
import android.content.pm.parsing.ApkLiteParseUtils;
import android.content.pm.parsing.result.ParseResult;
import android.content.pm.parsing.result.ParseTypeImpl;
import android.graphics.Bitmap;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.IBinder;
import android.os.ServiceSpecificException;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.FileVisitResult;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.*;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

/** Full APK sessions with fresh host consent for every commit. No store receives silent-install authority. */
public final class PackageInstallerService extends IPackageInstaller.Default {
    static final long MAX_FILE_BYTES = 1024L * 1024 * 1024;
    static final long MAX_SESSION_BYTES = 2 * MAX_FILE_BYTES;
    private static final int MAX_SESSIONS = 16, MAX_OWNER_SESSIONS = 4, MAX_FILES = 64;
    interface Consent { boolean allow(String installer, String target, long version); }

    private final PackageManagerService mPm;
    private final Executor mExecutor;
    private final Consent mConsent;
    private final Supplier<String> mTestCaller;
    private final InstallFileBridge.Transport mTransport;
    private final AtomicInteger mIds = new AtomicInteger(1);
    private final Map<Integer, Session> mSessions = new LinkedHashMap<>();
    private final Map<IBinder, CallbackOwner> mCallbacks = new HashMap<>();
    private final Executor mCallbackExecutor = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "package-install-callbacks"); t.setDaemon(true); return t;
    });
    private volatile CallerIdentity mCallers;
    private volatile Installer mInstaller;

    PackageInstallerService(PackageManagerService pm) {
        this(pm, Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, "package-install-consent"); t.setDaemon(true); return t;
        }), (installer, target, version) -> PermissionRequests.confirm(installer,
                "Install or update Android application " + target + " (version " + version
                + ")?\nThis approval applies to this installation only."), null, InstallPipeTransport.TRANSPORT);
    }
    /** Server-local test injection; never exposed through Binder, intent extras, or environment. */
    PackageInstallerService(PackageManagerService pm, Executor executor, Consent consent,
            Supplier<String> caller, InstallFileBridge.Transport transport) {
        mPm = pm; mExecutor = executor; mConsent = consent; mTestCaller = caller; mTransport = transport;
    }
    void attach(ActivityTaskManagerService atm, Installer installer) {
        if (mInstaller != null) throw new IllegalStateException("Package installer already attached");
        // Session metadata is not persisted. Only this service's private staging
        // directories survive a crashed server, never installed code or app data.
        File[] stale = new File(Device.dataDir(), "system/install-sessions").listFiles();
        if (stale != null) for (File f : stale) {
            if (f.getName().startsWith("session-") || f.getName().startsWith("commit-")) clean(f);
        }
        mCallers = new CallerIdentity(mPm, atm); mInstaller = installer;
    }
    private String caller() {
        if (mTestCaller != null) return mTestCaller.get();
        if (mCallers == null) throw new UnsupportedOperationException("No persistent package installer");
        return mCallers.callerPackage();
    }
    private String owner(String claimed, int userId) {
        String actual = caller();
        if (userId != 0) throw new SecurityException("Only user 0 is supported");
        if (claimed == null || (!"android".equals(actual) && !actual.equals(claimed))
                || (!"android".equals(claimed) && mPm.get(claimed) == null)) {
            throw new SecurityException("Unknown or foreign installer");
        }
        return claimed;
    }
    private void reapStaleSessions() {
        List<Session> snapshot;
        synchronized (mSessions) { snapshot = new ArrayList<>(mSessions.values()); }
        // Never acquire a session lock while holding the registry lock. A commit
        // already owns a private snapshot; destruction marks it ineligible for
        // consent/publication without deleting that worker's snapshot underneath it.
        for (Session s : snapshot) {
            synchronized (s) {
                if (mPm.get(s.owner) != s.ownerCode) s.destroy();
            }
        }
    }
    @Override public int createSession(PackageInstaller.SessionParams p, String pkg, String tag, int user) {
        String owner = owner(pkg, user);
        if (mInstaller == null) throw new UnsupportedOperationException("No persistent package installer");
        if (p == null || p.mode != PackageInstaller.SessionParams.MODE_FULL_INSTALL || p.isMultiPackage
                || p.isStaged || p.dataLoaderParams != null || p.abiOverride != null
                || p.volumeUuid != null || p.getEnableRollback()
                || (p.installFlags & 0x00020000) != 0) { // AOSP INSTALL_APEX, hidden in framework export.
            throw new UnsupportedOperationException("Only ordinary full APK installs are supported");
        }
        if (p.sizeBytes < -1 || p.sizeBytes > MAX_SESSION_BYTES) {
            throw new IllegalArgumentException("Invalid installation size");
        }
        reapStaleSessions();
        ApkPackage ownerCode = mPm.get(owner);
        if (ownerCode == null && !"android".equals(owner)) throw new SecurityException("Installer disappeared");
        // Client installFlags, permission grants, originating UID and USER_ACTION_NOT_REQUIRED
        // never select privileged behavior. Every caller, including Play, needs fresh consent.
        Session created;
        synchronized (mSessions) {
            int count = 0;
            for (Session s : mSessions.values()) if (owner.equals(s.owner)) count++;
            if (mSessions.size() >= MAX_SESSIONS || count >= MAX_OWNER_SESSIONS) {
                throw new IllegalStateException("Too many installation sessions");
            }
            try {
                File root = new File(Device.dataDir(), "system/install-sessions");
                Files.createDirectories(root.toPath());
                File stage = Files.createTempDirectory(root.toPath(), "session-").toFile();
                int id = mIds.getAndIncrement();
                created = new Session(id, owner, ownerCode, p.appPackageName, p.sizeBytes, stage);
                mSessions.put(id, created);
            } catch (IOException e) { throw io(); }
        }
        notify(created, c -> c.onSessionCreated(created.id));
        return created.id;
    }
    private Session owned(int id) {
        Session s; synchronized (mSessions) { s = mSessions.get(id); }
        if (s == null) throw new IllegalArgumentException("Unknown installation session");
        s.checkOwner(); return s;
    }
    @Override public IPackageInstallerSession openSession(int id) { return owned(id).binder; }
    @Override public void abandonSession(int id) { owned(id).abandon(); }
    @Override public void updateSessionAppIcon(int id, Bitmap icon) { owned(id); }
    @Override public void updateSessionAppLabel(int id, String label) { owned(id); }
    @Override public PackageInstaller.SessionInfo getSessionInfo(int id) {
        Session s; synchronized (mSessions) { s = mSessions.get(id); }
        if (s == null) return null; s.checkOwner(); return s.info();
    }
    @Override public ParceledListSlice<PackageInstaller.SessionInfo> getAllSessions(int user) {
        if (user != 0) throw new SecurityException("Only user 0 is supported");
        String owner = caller();
        List<PackageInstaller.SessionInfo> list = new ArrayList<>();
        List<Session> snapshot;
        synchronized (mSessions) { snapshot = new ArrayList<>(mSessions.values()); }
        for (Session s : snapshot) {
            if (s.ownerCurrent() && ("android".equals(owner) || owner.equals(s.owner))) list.add(s.info());
        }
        return new ParceledListSlice<>(list);
    }
    @Override public ParceledListSlice<PackageInstaller.SessionInfo> getMySessions(String pkg, int user) {
        String owner = owner(pkg, user);
        List<PackageInstaller.SessionInfo> list = new ArrayList<>();
        List<Session> snapshot;
        synchronized (mSessions) { snapshot = new ArrayList<>(mSessions.values()); }
        for (Session s : snapshot) {
            if (owner.equals(s.owner) && s.ownerCurrent()) list.add(s.info());
        }
        return new ParceledListSlice<>(list);
    }
    @Override public ParceledListSlice<PackageInstaller.SessionInfo> getStagedSessions() {
        caller(); return new ParceledListSlice<>(Collections.emptyList());
    }
    @Override public void registerCallback(IPackageInstallerCallback c, int user) {
        if (user != 0) throw new SecurityException("Only user 0 is supported");
        String pkg = caller();
        if (c == null) throw new IllegalArgumentException("Missing callback");
        IBinder key = c.asBinder();
        if (key == null) throw new IllegalArgumentException("Missing callback Binder");
        synchronized (mCallbacks) {
            for (CallbackOwner old : new ArrayList<>(mCallbacks.values())) {
                if (mPm.get(old.pkg) != old.code) removeCallback(old);
            }
            CallbackOwner old = mCallbacks.get(key);
            if (old != null) {
                if (!old.pkg.equals(pkg)) throw new SecurityException("Foreign callback");
                return; // A new Java AIDL Proxy for the same Binder is the same registration.
            }
            if (mCallbacks.size() >= 64) throw new IllegalStateException("Too many callbacks");
            CallbackOwner entry = new CallbackOwner(pkg, mPm.get(pkg), c);
            try { key.linkToDeath(entry.death, 0); }
            catch (RemoteException e) { return; }
            mCallbacks.put(key, entry);
            if (!key.isBinderAlive()) removeCallback(entry);
        }
    }
    @Override public void unregisterCallback(IPackageInstallerCallback c) {
        String pkg = caller();
        synchronized (mCallbacks) {
            CallbackOwner owner = c == null ? null : mCallbacks.get(c.asBinder());
            if (owner != null && !"android".equals(pkg) && !pkg.equals(owner.pkg)) {
                throw new SecurityException("Foreign callback");
            }
            if (owner != null) removeCallback(owner);
        }
    }
    @Override public void setPermissionsResult(int id, boolean accepted) {
        throw new SecurityException("Only the host installation dialog can approve a session");
    }
    @Override public void installExistingPackage(String pkg, int flags, int reason, IntentSender receiver,
            int user, List<String> permissions) {
        caller();
        result(receiver, -1, pkg, PackageInstaller.STATUS_FAILURE_INCOMPATIBLE, "Additional users unsupported");
    }
    @Override public void uninstall(android.content.pm.VersionedPackage pkg, String claimed, int flags,
            IntentSender receiver, int user) {
        owner(claimed, user);
        result(receiver, -1, pkg == null ? null : pkg.getPackageName(),
                PackageInstaller.STATUS_FAILURE_BLOCKED, "Uninstall through this API is unsupported");
    }
    private final class CallbackOwner {
        final String pkg;
        final ApkPackage code;
        final IPackageInstallerCallback callback;
        final IBinder binder;
        final IBinder.DeathRecipient death = () -> removeCallback(this);
        CallbackOwner(String pkg, ApkPackage code, IPackageInstallerCallback callback) {
            this.pkg = pkg; this.code = code; this.callback = callback; binder = callback.asBinder();
        }
    }
    private void removeCallback(CallbackOwner entry) {
        synchronized (mCallbacks) {
            if (mCallbacks.get(entry.binder) != entry) return;
            mCallbacks.remove(entry.binder);
            try { entry.binder.unlinkToDeath(entry.death, 0); }
            catch (RuntimeException ignored) { } // A dead Binder already discarded its recipients.
        }
    }
    private interface Callback { void send(IPackageInstallerCallback callback) throws RemoteException; }
    private void notify(Session s, Callback action) {
        Map<IBinder, CallbackOwner> list;
        synchronized (mCallbacks) { list = new HashMap<>(mCallbacks); }
        // Never invoke an app's Binder while holding a session or registry lock.
        mCallbackExecutor.execute(() -> {
            for (Map.Entry<IBinder, CallbackOwner> e : list.entrySet()) {
                CallbackOwner owner = e.getValue();
                if (mPm.get(owner.pkg) != owner.code) {
                    removeCallback(owner);
                    continue;
                }
                if (!s.owner.equals(owner.pkg) && !"android".equals(owner.pkg)) continue;
                try { action.send(owner.callback); }
                catch (RemoteException | RuntimeException ignored) {
                    removeCallback(owner);
                }
            }
        });
    }
    private static void result(IntentSender receiver, int id, String pkg, int status, String message) {
        if (receiver == null) return;
        Intent intent = new Intent().putExtra(PackageInstaller.EXTRA_SESSION_ID, id)
                .putExtra(PackageInstaller.EXTRA_STATUS, status)
                .putExtra(PackageInstaller.EXTRA_STATUS_MESSAGE, message);
        if (pkg != null) intent.putExtra(PackageInstaller.EXTRA_PACKAGE_NAME, pkg);
        try { receiver.getTarget().send(0, intent, null, null, null, null, null); }
        catch (RemoteException | RuntimeException e) {
            SystemServer.log("package install result unavailable: " + e.getClass().getSimpleName());
        }
    }
    private static ServiceSpecificException io() {
        return new ServiceSpecificException(android.system.OsConstants.EIO, "Installation I/O failure");
    }
    private static void clean(File directory) {
        // walkFileTree does not follow symbolic links, unlike File.isDirectory.
        // Every argument is a service-generated direct child of install-sessions.
        Path root = new File(Device.dataDir(), "system/install-sessions").toPath().toAbsolutePath().normalize();
        Path path = directory.toPath().toAbsolutePath().normalize();
        if (!root.equals(path.getParent())) throw new SecurityException("Invalid session cleanup path");
        try {
            Files.walkFileTree(path, new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                    Files.delete(file); return FileVisitResult.CONTINUE;
                }
                @Override public FileVisitResult postVisitDirectory(Path dir, IOException failure) throws IOException {
                    if (failure != null) throw failure;
                    Files.delete(dir); return FileVisitResult.CONTINUE;
                }
            });
        } catch (IOException e) {
            SystemServer.log("package session cleanup deferred: " + e.getClass().getSimpleName());
        }
    }

    public final class Session extends IPackageInstallerSession.Default {
        final int id;
        final String owner, expectedPackage;
        final ApkPackage ownerCode;
        final long expectedSize, created = System.currentTimeMillis();
        final File stage;
        final IPackageInstallerSession binder;
        final Map<String, InstallFileBridge> writes = new HashMap<>();
        boolean sealed, committing, destroyed, writeFailed;
        float progress;
        long bytes;
        Session(int id, String owner, ApkPackage code, String pkg, long size, File stage) {
            this.id = id; this.owner = owner; ownerCode = code; expectedPackage = pkg;
            expectedSize = size; this.stage = stage;
            binder = StandIn.of(IPackageInstallerSession.class, this).asInterface();
        }
        void checkOwner() {
            String actual = caller();
            if (!"android".equals(actual) && !owner.equals(actual)) throw new SecurityException("Foreign session");
            if (mPm.get(owner) != ownerCode) throw new SecurityException("Installer code changed");
            if (destroyed) throw new IllegalStateException("Installation session closed");
        }
        void writable() { checkOwner(); if (sealed) throw new IllegalStateException("Session sealed"); }
        File file(String name) {
            if (name == null || !name.matches("[A-Za-z0-9_-][A-Za-z0-9_.-]{0,123}\\.apk")) {
                throw new IllegalArgumentException("Invalid APK filename");
            }
            return new File(stage, name);
        }
        synchronized PackageInstaller.SessionInfo info() {
            PackageInstaller.SessionInfo i = new PackageInstaller.SessionInfo();
            i.sessionId = id; i.userId = 0; i.installerPackageName = owner;
            i.installerUid = ownerCode == null ? android.os.Process.SYSTEM_UID : ownerCode.uid();
            i.mode = PackageInstaller.SessionParams.MODE_FULL_INSTALL; i.progress = progress;
            i.sealed = sealed; i.active = !writes.isEmpty() || committing;
            i.sizeBytes = expectedSize; i.appPackageName = expectedPackage;
            i.requireUserAction = PackageInstaller.SessionParams.USER_ACTION_REQUIRED;
            i.createdMillis = created; i.updatedMillis = created; return i;
        }
        @Override public synchronized void setClientProgress(float value) {
            writable(); if (!Float.isFinite(value)) throw new IllegalArgumentException("Invalid progress");
            progress = Math.max(0, Math.min(1, value));
            PackageInstallerService.this.notify(this, c -> c.onSessionProgressChanged(id, progress));
        }
        @Override public synchronized void addClientProgress(float value) { setClientProgress(progress + value); }
        @Override public synchronized String[] getNames() {
            checkOwner(); String[] names = stage.list((dir, name) -> name.endsWith(".apk"));
            return names == null ? new String[0] : names;
        }
        @Override public synchronized ParcelFileDescriptor openWrite(String name, long offset, long length) {
            writable(); File file = file(name);
            if (offset < 0 || offset > file.length() || length < -1 || length > MAX_FILE_BYTES
                    || writes.containsKey(name) || (!file.exists() && getNames().length >= MAX_FILES)) {
                throw new IllegalArgumentException("Invalid write range or active writer");
            }
            if (PackageInstaller.ENABLE_REVOCABLE_FD) {
                throw new UnsupportedOperationException("Revocable installation descriptors unsupported");
            }
            try {
                InstallFileBridge bridge = new InstallFileBridge(file, offset, MAX_FILE_BYTES,
                        (growth) -> {
                            synchronized (Session.this) {
                                if (destroyed || growth > MAX_SESSION_BYTES - bytes) {
                                    throw new IOException("Session size or lifetime exceeded");
                                }
                                bytes += growth;
                            }
                        }, (complete) -> {
                            boolean retryCleanup;
                            synchronized (Session.this) {
                                writes.remove(name);
                                if (!complete && !destroyed) writeFailed = true;
                                retryCleanup = destroyed;
                            }
                            if (retryCleanup) clean(stage);
                        }, mTransport);
                writes.put(name, bridge); bridge.start(); return bridge.client();
            } catch (IOException e) { throw io(); }
        }
        @Override public synchronized ParcelFileDescriptor openRead(String name) {
            checkOwner(); if (writes.containsKey(name)) throw new IllegalStateException("APK write active");
            try { return ParcelFileDescriptor.open(file(name), ParcelFileDescriptor.MODE_READ_ONLY); }
            catch (IOException e) { throw io(); }
        }
        @Override public void write(String name, long offset, long length, ParcelFileDescriptor input) {
            throw new UnsupportedOperationException("Use bounded openWrite stream");
        }
        @Override public synchronized void close() { checkOwner(); }
        @Override public synchronized void seal() {
            writable();
            if (!writes.isEmpty() || writeFailed) throw new IllegalStateException("APK writes incomplete");
            sealed = true;
        }
        @Override public synchronized void commit(IntentSender receiver, boolean transferred) {
            checkOwner();
            if (receiver == null) throw new IllegalArgumentException("Missing status receiver");
            if (transferred || committing || !writes.isEmpty() || writeFailed) {
                throw new IllegalStateException("Session cannot be committed");
            }
            sealed = true; committing = true;
            try { mExecutor.execute(() -> install(receiver)); }
            catch (RuntimeException e) { committing = false; throw e; }
        }
        private void install(IntentSender receiver) {
            int status = PackageInstaller.STATUS_FAILURE_INVALID;
            String pkg = expectedPackage, message = "Invalid APK set";
            File frozen = null;
            try {
                frozen = Files.createTempDirectory(stage.toPath().getParent(), "commit-").toFile();
                File[] files = stage.listFiles((dir, name) -> name.endsWith(".apk"));
                if (files == null || files.length == 0) throw new IllegalArgumentException("Empty APK set");
                ApkLite first = null; Signature[] signers = null; boolean base = false;
                for (File source : files) {
                    File snapshot = new File(frozen, source.getName()); Files.copy(source.toPath(), snapshot.toPath());
                    ParseResult<ApkLite> parsed = ApkLiteParseUtils.parseApkLite(
                            ParseTypeImpl.forParsingWithoutPlatformCompat(), snapshot, 0);
                    if (parsed.isError()) throw new IllegalArgumentException("Invalid manifest");
                    ApkLite lite = parsed.getResult();
                    Signature[] current = ApkPackage.verifySignatures(snapshot.getPath());
                    if (current == null) throw new SecurityException("Store installs must be signed");
                    if (first == null) { first = lite; signers = current; }
                    else if (!first.getPackageName().equals(lite.getPackageName())
                            || first.getLongVersionCode() != lite.getLongVersionCode()
                            || !Arrays.equals(signers, current)) throw new SecurityException("Mismatched APK set");
                    if (lite.getSplitName() == null) { if (base) throw new IllegalArgumentException("Multiple bases"); base = true; }
                }
                if (!base) throw new IllegalArgumentException("Missing base APK");
                // A unique private canonical path bypasses the mutable-source cache. The
                // client has no writable FD to either snapshot or final installer input.
                File canonical = new File(frozen, "apks"); Files.createDirectory(canonical.toPath());
                for (File source : frozen.listFiles((dir, name) -> name.endsWith(".apk"))) {
                    ApkLite lite = ApkLiteParseUtils.parseApkLite(
                            ParseTypeImpl.forParsingWithoutPlatformCompat(), source, 0).getResult();
                    String split = lite.getSplitName();
                    if (split != null && !split.matches("[A-Za-z0-9_][A-Za-z0-9_.-]{0,120}")) {
                        throw new IllegalArgumentException("Invalid split name");
                    }
                    Files.move(source.toPath(), new File(canonical,
                            split == null ? "base.apk" : "split_" + split + ".apk").toPath());
                }
                pkg = first.getPackageName();
                if (expectedPackage != null && !expectedPackage.equals(pkg)) throw new SecurityException("Wrong package");
                ApkPackage targetCode = mPm.get(pkg);
                if (!ownerCurrent()) throw new SecurityException("Installer changed");
                status = PackageInstaller.STATUS_FAILURE_ABORTED; message = "Installation not approved";
                if (!mConsent.allow(owner, pkg, first.getLongVersionCode())) return;
                // The installer's own lock covers both stale-consent checks and publication.
                synchronized (mInstaller) {
                    if (!ownerCurrent() || mPm.get(pkg) != targetCode) throw new SecurityException("Package code changed");
                    String[] installed = mInstaller.install(new File(canonical, "base.apk").getPath());
                    if (!pkg.equals(installed[0]) || mPm.get(pkg) == null) throw new IllegalStateException("Not published");
                }
                status = PackageInstaller.STATUS_SUCCESS; message = "Installed";
            } catch (SecurityException e) {
                status = PackageInstaller.STATUS_FAILURE_BLOCKED; message = "Package identity or consent changed";
            } catch (IOException e) {
                status = PackageInstaller.STATUS_FAILURE_STORAGE; message = "Installation storage failure";
            } catch (Exception e) {
                status = PackageInstaller.STATUS_FAILURE_INVALID; message = "Installation failed: " + e.getClass().getSimpleName();
            } finally {
                boolean success = status == PackageInstaller.STATUS_SUCCESS;
                result(receiver, id, pkg, status, message);
                PackageInstallerService.this.notify(this, c -> c.onSessionFinished(id, success));
                if (frozen != null) clean(frozen);
                destroy();
            }
        }
        synchronized boolean ownerCurrent() { return !destroyed && mPm.get(owner) == ownerCode; }
        @Override public synchronized void abandon() {
            checkOwner(); if (committing) throw new IllegalStateException("Commit active");
            destroy(); PackageInstallerService.this.notify(this, c -> c.onSessionFinished(id, false));
        }
        synchronized void destroy() {
            destroyed = true;
            for (InstallFileBridge bridge : new ArrayList<>(writes.values())) bridge.forceClose();
            synchronized (mSessions) { mSessions.remove(id); }
            clean(stage);
        }
        @Override public synchronized boolean isMultiPackage() { checkOwner(); return false; }
        @Override public synchronized boolean isStaged() { checkOwner(); return false; }
        @Override public synchronized int getInstallFlags() { checkOwner(); return 0; }
        @Override public synchronized int[] getChildSessionIds() { checkOwner(); return new int[0]; }
        @Override public synchronized int getParentSessionId() { checkOwner(); return -1; }
        @Override public void transfer(String pkg) { throw new UnsupportedOperationException("Transfer unsupported"); }
        @Override public void stageViaHardLink(String path) { throw new SecurityException("Host paths cannot be staged"); }
        @Override public void removeSplit(String split) { throw new UnsupportedOperationException("Inheritance unsupported"); }
        @Override public void addChildSessionId(int child) { throw new UnsupportedOperationException("Multi-package unsupported"); }
        @Override public void removeChildSessionId(int child) { throw new UnsupportedOperationException("Multi-package unsupported"); }
        @Override public void addFile(int location, String name, long length, byte[] metadata, byte[] signature) {
            throw new UnsupportedOperationException("Data loaders unsupported");
        }
        @Override public void removeFile(int location, String name) {
            throw new UnsupportedOperationException("Data loaders unsupported");
        }
        @Override public void requestUserPreapproval(PackageInstaller.PreapprovalDetails details, IntentSender receiver) {
            checkOwner();
            result(receiver, id, expectedPackage, PackageInstaller.STATUS_FAILURE_BLOCKED,
                    "Preapproval unsupported; commit requires the host installation dialog");
        }
        @Override public void setChecksums(String name, android.content.pm.Checksum[] checksums, byte[] signature) {
            throw new UnsupportedOperationException("Caller checksum storage unsupported");
        }
        @Override public ParcelFileDescriptor openWriteAppMetadata() {
            throw new UnsupportedOperationException("App metadata writes unsupported");
        }
        @Override public List<String> fetchPackageNames() {
            checkOwner();
            throw new UnsupportedOperationException("Package names are resolved at commit");
        }
    }
}
