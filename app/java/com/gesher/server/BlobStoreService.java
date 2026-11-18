/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.blob.BlobHandle;
import android.app.blob.BlobInfo;
import android.app.blob.BlobStoreManager;
import android.app.blob.IBlobCommitCallback;
import android.app.blob.IBlobStoreManager;
import android.app.blob.IBlobStoreSession;
import android.app.blob.LeaseInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.ResourceId;
import android.content.res.Resources;
import android.os.Bundle;
import android.os.LimitExceededException;
import android.os.ParcelFileDescriptor;
import android.os.ParcelableException;
import android.os.Process;
import android.os.RemoteCallback;
import android.os.RemoteException;
import android.os.UserHandle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.DisplayMetrics;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** Managed-runtime BlobStore with one atomic index for sessions, contributions and leases. */
public final class BlobStoreService extends IBlobStoreManager.Stub {
    private static final long QUOTA_FLOOR = 300L * 1024 * 1024;
    private static final long SESSION_LIFETIME = TimeUnit.DAYS.toMillis(7);
    private static final long LEASE_WAIT = TimeUnit.HOURS.toMillis(6);
    private static final long COMMIT_COOL_OFF = TimeUnit.HOURS.toMillis(48);
    private static final int MAX_SESSIONS = 250;
    private static final int MAX_COMMITTED = 1_000;
    private static final int MAX_LEASED = 500;
    private static final int MAX_GRANTS = 300;
    private static final int MAX_INDEX_ENTRIES = 100_000;
    private static final int INDEX_VERSION = 1;
    private static final String CONSUMED_SUFFIX = ".consumed";
    private static final Access PRIVATE = new Access(false, false, List.of());

    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final File mRoot;
    private final ScheduledThreadPoolExecutor mWorker = new ScheduledThreadPoolExecutor(1, task -> {
        Thread thread = new Thread(task, "blob-store");
        thread.setDaemon(true);
        return thread;
    });
    private final SecureRandom mRandom = new SecureRandom();
    private final Map<Long, Session> mOpen = new LinkedHashMap<>();
    // Committed sessions whose index retirement still needs to be persisted.
    private final Set<Long> mConsumed = new HashSet<>();
    private Index mIndex;

    BlobStoreService(PackageManagerService pm, CallerIdentity callers, File root) {
        mPm = pm;
        mCallers = callers;
        mRoot = root;
    }

    // Signed updates are already checked for continuity by Installer. The persisted
    // first-install time differentiates ordinary reinstallations, including across a
    // restart before maintenance. Legacy unverified installs are tied to their code path.
    private record Owner(String name, long installedAt, String unverifiedCode) { }
    private record Grant(String name, byte[] sha256) { }
    private record Access(boolean publiclyReadable, boolean sameSignature, List<Grant> grants) {
        boolean includes(String name, byte[] certificate) {
            for (Grant grant : grants) {
                if (grant.name.equals(name) && MessageDigest.isEqual(grant.sha256, certificate)) {
                    return true;
                }
            }
            return false;
        }
    }
    private record Contribution(Owner owner, long committedAt, Access access) { }
    private record Lease(Owner owner, long expiry, String resourceName, String description) {
        boolean active(long now) { return expiry == 0 || expiry >= now; }
    }
    private record Stage(long id, Owner owner, BlobHandle handle, String fileName, Access access) { }
    private record Blob(long id, BlobHandle handle, String fileName, long size,
            long unleasedSince, Map<String, Contribution> contributions, Map<String, Lease> leases) { }

    private static final class Index {
        final Map<Long, Stage> sessions;
        final Map<BlobHandle, Blob> blobs;
        Index() {
            sessions = new LinkedHashMap<>();
            blobs = new LinkedHashMap<>();
        }
        Index(Index previous) {
            sessions = new LinkedHashMap<>(previous.sessions);
            blobs = new LinkedHashMap<>(previous.blobs);
        }
    }

    private Owner installedOwner(String name) {
        if ("android".equals(name)) return new Owner("android", 0, "");
        ApkPackage pkg = mPm.get(name);
        if (pkg == null) return null;
        return new Owner(name, pkg.firstInstallTime(),
                pkg.hasVerifiedSigningIdentity() ? "" : pkg.apkPath);
    }

    private Owner requireOwner(String name) {
        Objects.requireNonNull(name, "packageName");
        mCallers.requirePackage(name);
        Owner owner = installedOwner(name);
        if (owner == null || mPm.isRemovalPending(name)) {
            throw new SecurityException("BlobStore caller is not an installed package");
        }
        return owner;
    }

    private void requireSystem() {
        if (!mCallers.isSystem() || (mCallers.callerPid() != Process.myPid()
                && mCallers.callerUid() != Process.SYSTEM_UID)) {
            throw new SecurityException("Blob metadata administration requires the system UID");
        }
    }

    private static BlobHandle checkedHandle(BlobHandle handle) {
        Objects.requireNonNull(handle, "blobHandle").assertIsValid();
        return BlobHandle.createWithSha256(handle.getSha256Digest().clone(),
                handle.getLabel().toString(), handle.getExpiryTimeMillis(), handle.getTag());
    }

    private static ParcelableException failure(IOException error) {
        return new ParcelableException(error);
    }

    private static ParcelableException limit(String message) {
        return new ParcelableException(new LimitExceededException(message));
    }

    private File dataFile(String name) { return new File(mRoot, name); }

    private Index index() {
        if (mRoot == null) throw new SecurityException("Shared blobs require the managed runtime");
        if (mIndex != null) return mIndex;
        try {
            Files.createDirectories(mRoot.toPath());
            if (!Files.isDirectory(mRoot.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("BlobStore is not a directory");
            }
            mIndex = readIndex();
            for (Stage stage : mIndex.sessions.values()) {
                if (consumedMarker(stage).exists()) mConsumed.add(stage.id);
            }
            mWorker.scheduleWithFixedDelay(this::maintain, 0, 1, TimeUnit.HOURS);
            return mIndex;
        } catch (IOException error) {
            throw failure(error);
        }
    }

    private void update(Index next) {
        File temporary = null;
        try {
            temporary = Files.createTempFile(mRoot.toPath(), "index-", ".tmp").toFile();
            try (FileOutputStream raw = new FileOutputStream(temporary);
                    DataOutputStream output = new DataOutputStream(new BufferedOutputStream(raw))) {
                writeIndex(output, next);
                output.flush();
                raw.getFD().sync();
            }
            Files.move(temporary.toPath(), dataFile("index").toPath(),
                    StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            mIndex = next;
        } catch (IOException error) {
            throw failure(error);
        } finally {
            BlobStoreFiles.discard(temporary);
        }
    }

    private long nextId(Index state) {
        for (;;) {
            long id = mRandom.nextLong() & Long.MAX_VALUE;
            if (id == 0 || state.sessions.containsKey(id)) continue;
            if (state.blobs.values().stream().noneMatch(blob -> blob.id == id)) return id;
        }
    }

    @Override public synchronized long createSession(BlobHandle handle, String packageName) {
        BlobHandle key = checkedHandle(handle);
        Owner owner = requireOwner(packageName);
        Index state = index();
        long active = state.sessions.values().stream()
                .filter(stage -> stage.owner.equals(owner) && !mConsumed.contains(stage.id)).count();
        if (active >= MAX_SESSIONS) throw limit("Too many active blob sessions");
        File file = null;
        try {
            file = Files.createTempFile(mRoot.toPath(), "data-", ".bin").toFile();
            long id = nextId(state);
            Index next = new Index(state);
            next.sessions.put(id, new Stage(id, owner, key, file.getName(), PRIVATE));
            update(next);
            return id;
        } catch (IOException error) {
            BlobStoreFiles.discard(file);
            throw failure(error);
        } catch (RuntimeException error) {
            BlobStoreFiles.discard(file);
            throw error;
        }
    }

    private Stage ownedStage(long id, Owner owner) {
        if (id <= 0) throw new IllegalArgumentException("Session ID must be positive");
        Stage stage = index().sessions.get(id);
        Session open = mOpen.get(id);
        if (stage == null || !stage.owner.equals(owner) || mConsumed.contains(id)
                || (open != null && open.committing)
                || !validFile(stage.fileName)) {
            throw new SecurityException("No accessible blob session");
        }
        return stage;
    }

    @Override public synchronized IBlobStoreSession openSession(long id, String packageName) {
        Stage stage = ownedStage(id, requireOwner(packageName));
        return mOpen.computeIfAbsent(id, key -> new Session(key, stage.owner));
    }

    @Override public synchronized void abandonSession(long id, String packageName) {
        abandon(ownedStage(id, requireOwner(packageName)));
    }

    private void abandon(Stage stage) {
        Index next = new Index(mIndex);
        next.sessions.remove(stage.id);
        update(next);
        mOpen.remove(stage.id);
        BlobStoreFiles.discard(dataFile(stage.fileName));
    }

    private boolean validFile(String name) {
        return Files.isRegularFile(dataFile(name).toPath(), LinkOption.NOFOLLOW_LINKS);
    }

    private boolean currentlyInstalled(Owner owner) {
        return owner.equals(installedOwner(owner.name));
    }

    private boolean access(Blob blob, Owner caller, long now) {
        if (blob.handle.isExpired() || !validFile(blob.fileName)) return false;
        Lease lease = blob.leases.get(caller.name);
        if (lease != null && lease.owner.equals(caller) && lease.active(now)) return true;
        ApkPackage recipient = mPm.get(caller.name);
        for (Contribution contribution : blob.contributions.values()) {
            Owner owner = contribution.owner;
            if (!currentlyInstalled(owner) || mPm.isRemovalPending(owner.name)) continue;
            Access allowed = contribution.access;
            if (owner.equals(caller) || allowed.publiclyReadable) return true;
            if (recipient == null) continue;
            if (allowed.sameSignature && recipient.signaturesMatch(mPm.get(owner.name))) return true;
            for (Grant grant : allowed.grants) {
                if (grant.name.equals(caller.name) && recipient.hasVerifiedSigningCertificate(grant.sha256)) {
                    return true;
                }
            }
        }
        return false;
    }

    private Blob accessible(BlobHandle handle, Owner owner, long now) {
        Blob blob = index().blobs.get(checkedHandle(handle));
        if (blob == null || !access(blob, owner, now)) {
            throw new SecurityException("No accessible shared blob");
        }
        return blob;
    }

    @Override public synchronized ParcelFileDescriptor openBlob(BlobHandle handle, String packageName) {
        Blob blob = accessible(handle, requireOwner(packageName), System.currentTimeMillis());
        return openReadOnly(blob.fileName);
    }

    private ParcelFileDescriptor openReadOnly(String name) {
        try {
            return ParcelFileDescriptor.open(dataFile(name), ParcelFileDescriptor.MODE_READ_ONLY);
        } catch (IOException error) {
            throw failure(error);
        }
    }

    private void allocateWriteLength(ParcelFileDescriptor descriptor, long length) throws IOException {
        var fd = descriptor.getFileDescriptor();
        try {
            long blocks = Math.max(0, Os.fstat(fd).st_blocks);
            long allocated = Math.min(blocks, Long.MAX_VALUE / 512) * 512;
            if (length > allocated
                    && length - allocated > Services.StorageService.allocatableBytes(mRoot, 0)) {
                throw new IOException("Insufficient allocatable space for blob write");
            }
            // length describes the whole file, independently of the requested seek offset.
            Os.posix_fallocate(fd, 0, length);
        } catch (ErrnoException error) {
            throw error.rethrowAsIOException();
        }
    }

    private final class Session extends IBlobStoreSession.Stub {
        private final long id;
        private final Owner owner;
        private boolean issuedWriter;
        private boolean committing;

        Session(long id, Owner owner) {
            this.id = id;
            this.owner = owner;
        }

        private void authenticate() {
            if (!owner.equals(requireOwner(owner.name))) {
                throw new SecurityException("Blob session owner changed");
            }
        }

        private Stage opened() {
            authenticate();
            Stage stage = mIndex.sessions.get(id);
            if (stage == null || mOpen.get(id) != this || committing) {
                throw new IllegalStateException("Blob session is closed or finalized");
            }
            return stage;
        }

        @Override public ParcelFileDescriptor openWrite(long offset, long length) {
            if (offset < 0 || length < -1) {
                throw new IllegalArgumentException("Invalid blob write offset or length");
            }
            synchronized (BlobStoreService.this) {
                File file = dataFile(opened().fileName);
                ParcelFileDescriptor descriptor = null;
                try {
                    descriptor = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_WRITE);
                    descriptor.seekTo(offset);
                    if (length > 0) allocateWriteLength(descriptor, length);
                    issuedWriter = true;
                    return descriptor;
                } catch (IOException | RuntimeException error) {
                    if (descriptor != null) {
                        try { descriptor.close(); } catch (IOException close) { error.addSuppressed(close); }
                    }
                    if (error instanceof IOException io) throw failure(io);
                    throw (RuntimeException) error;
                }
            }
        }

        @Override public ParcelFileDescriptor openRead() {
            synchronized (BlobStoreService.this) { return openReadOnly(opened().fileName); }
        }

        @Override public long getSize() {
            synchronized (BlobStoreService.this) { return dataFile(opened().fileName).length(); }
        }

        @Override public void allowPackageAccess(String name, byte[] certificate) {
            Objects.requireNonNull(name, "packageName");
            Objects.requireNonNull(certificate, "certificate");
            if (name.isEmpty() || name.length() > 255 || certificate.length != 32) {
                throw new IllegalArgumentException("Expected a package name and SHA-256 certificate digest");
            }
            synchronized (BlobStoreService.this) {
                Stage stage = opened();
                Access mode = stage.access;
                if (mode.includes(name, certificate)) return;
                if (mode.grants.size() >= MAX_GRANTS) throw limit("Too many permitted blob packages");
                List<Grant> grants = new ArrayList<>(mode.grants);
                grants.add(new Grant(name, certificate.clone()));
                saveAccess(stage, new Access(mode.publiclyReadable, mode.sameSignature, grants));
            }
        }

        @Override public void allowSameSignatureAccess() {
            synchronized (BlobStoreService.this) {
                Stage stage = opened();
                Access mode = stage.access;
                if (!mode.sameSignature) saveAccess(stage, new Access(mode.publiclyReadable, true, mode.grants));
            }
        }

        @Override public void allowPublicAccess() {
            synchronized (BlobStoreService.this) {
                Stage stage = opened();
                Access mode = stage.access;
                if (!mode.publiclyReadable) saveAccess(stage, new Access(true, mode.sameSignature, mode.grants));
            }
        }

        @Override public boolean isPackageAccessAllowed(String name, byte[] certificate) {
            Objects.requireNonNull(name, "packageName");
            Objects.requireNonNull(certificate, "certificate");
            if (certificate.length == 0) throw new IllegalArgumentException("Empty certificate");
            synchronized (BlobStoreService.this) { return opened().access.includes(name, certificate); }
        }

        @Override public boolean isSameSignatureAccessAllowed() {
            synchronized (BlobStoreService.this) { return opened().access.sameSignature; }
        }

        @Override public boolean isPublicAccessAllowed() {
            synchronized (BlobStoreService.this) { return opened().access.publiclyReadable; }
        }

        @Override public void close() {
            synchronized (BlobStoreService.this) {
                authenticate();
                if (mOpen.get(id) != this || committing) return;
                Stage stage = opened();
                if (issuedWriter) {
                    // A Windows Binder FD is a duplicated HANDLE. A stale guest write
                    // handle may still exist after close, so reopen a detached copy.
                    File replacement = null;
                    try {
                        replacement = BlobStoreFiles.snapshot(dataFile(stage.fileName), mRoot, null);
                        Index next = new Index(mIndex);
                        next.sessions.put(id, new Stage(id, owner, stage.handle,
                                replacement.getName(), stage.access));
                        update(next);
                    } catch (IOException error) {
                        throw failure(error);
                    } catch (RuntimeException error) {
                        BlobStoreFiles.discard(replacement);
                        throw error;
                    }
                    BlobStoreFiles.discard(dataFile(stage.fileName));
                }
                mOpen.remove(id);
            }
        }

        @Override public void abandon() {
            synchronized (BlobStoreService.this) { BlobStoreService.this.abandon(opened()); }
        }

        @Override public void commit(IBlobCommitCallback callback) {
            Objects.requireNonNull(callback, "callback");
            synchronized (BlobStoreService.this) {
                Stage stage = opened();
                committing = true;
                mWorker.execute(() -> finishCommit(stage, callback));
            }
        }
    }

    private void saveAccess(Stage stage, Access access) {
        Index next = new Index(mIndex);
        next.sessions.put(stage.id, new Stage(stage.id, stage.owner, stage.handle, stage.fileName, access));
        update(next);
    }

    private void finishCommit(Stage stage, IBlobCommitCallback callback) {
        File candidate = null;
        int result = BlobStoreManager.COMMIT_RESULT_ERROR;
        try {
            // Hash exactly the bytes written to the new file. Checking the stage and
            // then renaming it would let an outstanding guest FD mutate a published blob.
            if (!stage.handle.isExpired()) {
                try {
                    candidate = BlobStoreFiles.snapshot(dataFile(stage.fileName), mRoot,
                            stage.handle.getSha256Digest());
                } catch (IOException ignored) {
                    // Digest mismatch or copy failure consumes the session.
                }
            }
            synchronized (this) {
                if (mIndex.sessions.get(stage.id) == stage) {
                    if (mPm.isRemovalPending(stage.owner.name) && currentlyInstalled(stage.owner)) {
                        // An uninstall may still roll back. Keep the saved stage
                        // available for the recovered owner to reopen.
                        mOpen.remove(stage.id);
                    } else {
                        // Persist consumption first. A failed blob publication must not
                        // roll back session retirement or an earlier valid contribution.
                        retireCommit(stage);
                        if (candidate != null && publish(stage, candidate)) {
                            result = BlobStoreManager.COMMIT_RESULT_SUCCESS;
                            candidate = null;
                        }
                    }
                }
            }
        } catch (RuntimeException error) {
            synchronized (this) {
                if (mIndex.sessions.get(stage.id) == stage && !mConsumed.contains(stage.id)) {
                    try { retireCommit(stage); }
                    catch (RuntimeException retirement) { error.addSuppressed(retirement); }
                }
            }
            SystemServer.log("BlobStore commit unavailable: " + error.getClass().getSimpleName());
        } finally {
            BlobStoreFiles.discard(candidate);
        }
        try { callback.onResult(result); }
        catch (RemoteException | RuntimeException ignored) {
            // A dead client does not undo an already published blob.
        }
    }

    private File consumedMarker(Stage stage) {
        return dataFile(stage.fileName + CONSUMED_SUFFIX);
    }

    private void retireCommit(Stage stage) {
        mOpen.remove(stage.id);
        mConsumed.add(stage.id);
        try {
            Index next = new Index(mIndex);
            next.sessions.remove(stage.id);
            update(next);
            mConsumed.remove(stage.id);
        } catch (RuntimeException error) {
            // A failed atomic replacement can leave the old index readable, and
            // an outstanding Windows descriptor can prevent deleting its data file.
            // Record retirement separately so neither can resurrect the session.
            try (FileChannel marker = FileChannel.open(consumedMarker(stage).toPath(),
                    StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
                marker.force(true);
            } catch (java.nio.file.FileAlreadyExistsException alreadyConsumed) {
                // An earlier retirement attempt has already recorded the marker.
            } catch (IOException markerError) {
                error.addSuppressed(markerError);
            }
            throw error;
        } finally {
            BlobStoreFiles.discard(dataFile(stage.fileName));
        }
    }

    private boolean publish(Stage stage, File candidate) {
        long now = System.currentTimeMillis();
        if (!currentlyInstalled(stage.owner) || stage.handle.isExpired()) return false;
        Blob existing = mIndex.blobs.get(stage.handle);
        if (existing != null && !validFile(existing.fileName)) existing = null;
        Contribution previous = existing == null ? null : existing.contributions.get(stage.owner.name);
        long committed = mIndex.blobs.values().stream().filter(blob -> {
            Contribution contribution = blob.contributions.get(stage.owner.name);
            return contribution != null && contribution.owner.equals(stage.owner);
        }).count();
        // Replacing this owner's existing contribution does not consume a new
        // committed-blob slot. A changed install generation is a new owner and
        // must pass the admission limit independently.
        boolean newContribution = previous == null || !previous.owner.equals(stage.owner);
        if (newContribution && committed >= MAX_COMMITTED) return false;
        Map<String, Contribution> contributions = existing == null
                ? new LinkedHashMap<>() : new LinkedHashMap<>(existing.contributions);
        long committedAt = previous != null && previous.owner.equals(stage.owner)
                && now - previous.committedAt < COMMIT_COOL_OFF ? previous.committedAt : now;
        contributions.put(stage.owner.name, new Contribution(stage.owner, committedAt, stage.access));
        Blob blob = existing == null
                ? new Blob(stage.id, stage.handle, candidate.getName(), candidate.length(), now,
                        contributions, Map.of())
                : new Blob(existing.id, existing.handle, existing.fileName, existing.size,
                        existing.unleasedSince, contributions, existing.leases);
        Index next = new Index(mIndex);
        next.blobs.put(stage.handle, blob);
        update(next);
        if (existing != null) BlobStoreFiles.discard(candidate);
        return true;
    }

    private boolean liveLease(Blob blob, Owner owner, long now) {
        Lease lease = blob.leases.get(owner.name);
        return lease != null && lease.owner.equals(owner) && lease.active(now);
    }

    private long quota(Owner owner, long now) {
        long remaining = Math.max(QUOTA_FLOOR, mRoot.getTotalSpace() / 100);
        for (Blob blob : mIndex.blobs.values()) {
            if (liveLease(blob, owner, now)) remaining -= Math.min(remaining, blob.size);
        }
        return remaining;
    }

    @Override public synchronized long getRemainingLeaseQuotaBytes(String packageName) {
        Owner owner = requireOwner(packageName);
        if (mRoot == null) return 0;
        index();
        return quota(owner, System.currentTimeMillis());
    }

    @Override public synchronized List<BlobHandle> getLeasedBlobs(String packageName) {
        Owner owner = requireOwner(packageName);
        if (mRoot == null) return List.of();
        long now = System.currentTimeMillis();
        List<BlobHandle> handles = new ArrayList<>();
        for (Blob blob : index().blobs.values()) {
            if (liveLease(blob, owner, now)) handles.add(checkedHandle(blob.handle));
        }
        return handles;
    }

    @Override public synchronized LeaseInfo getLeaseInfo(BlobHandle handle, String packageName) {
        Owner owner = requireOwner(packageName);
        long now = System.currentTimeMillis();
        Blob blob = accessible(handle, owner, now);
        if (!liveLease(blob, owner, now)) return null;
        Lease lease = blob.leases.get(owner.name);
        return leaseInfo(lease, lease.expiry);
    }

    @Override public synchronized void acquireLease(BlobHandle handle, int descriptionResId,
            CharSequence description, long expiry, String packageName) {
        Owner owner = requireOwner(packageName);
        BlobHandle key = checkedHandle(handle);
        boolean resource = ResourceId.isValid(descriptionResId);
        if (expiry < 0 || (!resource && description == null)
                || (key.getExpiryTimeMillis() > 0 && expiry > key.getExpiryTimeMillis())) {
            throw new IllegalArgumentException("Invalid blob lease expiry or description");
        }
        long now = System.currentTimeMillis();
        Blob blob = accessible(key, owner, now);
        long live = mIndex.blobs.values().stream().filter(item -> liveLease(item, owner, now)).count();
        Lease previousLease = blob.leases.get(owner.name);
        boolean renewing = previousLease != null && previousLease.owner.equals(owner)
                && previousLease.active(now);
        // Extending an active lease changes neither the leased-blob count nor
        // its byte charge. Only a genuinely new lease consumes quota.
        if (!renewing && (live >= MAX_LEASED || blob.size > quota(owner, now))) {
            throw limit("Blob lease quota exceeded");
        }
        String text = description == null ? null
                : description.subSequence(0, Math.min(description.length(), 300)).toString();
        Lease lease = resource ? leaseResource(owner.name, resources -> new Lease(owner, expiry,
                resources.getResourceName(descriptionResId),
                text == null ? resources.getString(descriptionResId) : text))
                : new Lease(owner, expiry, null, text);
        Map<String, Lease> leases = new LinkedHashMap<>(blob.leases);
        leases.put(owner.name, lease);
        Index next = new Index(mIndex);
        next.blobs.put(blob.handle, withLeases(blob, leases, now));
        update(next);
    }

    private LeaseInfo leaseInfo(Lease lease, long expiry) {
        int resourceId = 0;
        if (lease.resourceName != null) {
            try {
                resourceId = leaseResource(lease.owner.name,
                        resources -> resources.getIdentifier(lease.resourceName, null, null));
            } catch (IllegalArgumentException unavailable) {
                // An app update can remove the resource. The lease still exists.
            }
        }
        return new LeaseInfo(lease.owner.name, expiry, resourceId, lease.description);
    }

    private <T> T leaseResource(String name, java.util.function.Function<Resources, T> operation) {
        try {
            if ("android".equals(name)) return operation.apply(Resources.getSystem());
            ApkPackage pkg = mPm.get(name);
            if (pkg == null) throw new IllegalArgumentException("Unknown lease resource package");
            ApplicationInfo info = pkg.applicationInfo(0);
            try (AssetManager assets = new AssetManager()) {
                if (assets.addAssetPath(info.sourceDir) == 0) {
                    throw new IllegalArgumentException("Lease resource APK is unavailable");
                }
                if (info.splitSourceDirs != null) {
                    for (String split : info.splitSourceDirs) assets.addAssetPath(split);
                }
                return operation.apply(new Resources(assets, new DisplayMetrics(), new Configuration()));
            }
        } catch (Resources.NotFoundException invalid) {
            throw new IllegalArgumentException("Unknown lease description resource", invalid);
        }
    }

    private static Blob withLeases(Blob blob, Map<String, Lease> leases, long now) {
        boolean live = leases.values().stream().anyMatch(lease -> lease.active(now));
        return new Blob(blob.id, blob.handle, blob.fileName, blob.size, live ? 0 : now,
                blob.contributions, leases);
    }

    @Override public synchronized void releaseLease(BlobHandle handle, String packageName) {
        Owner owner = requireOwner(packageName);
        if (mRoot == null) return;
        long now = System.currentTimeMillis();
        Blob blob = accessible(handle, owner, now);
        Index next = new Index(mIndex);
        if (release(next, blob, owner, now)) update(next);
    }

    @Override public synchronized void releaseAllLeases(String packageName) {
        Owner owner = requireOwner(packageName);
        if (mRoot == null) return;
        long now = System.currentTimeMillis();
        Index state = index();
        Index next = new Index(state);
        boolean changed = false;
        for (Blob blob : state.blobs.values()) changed |= release(next, blob, owner, now);
        if (changed) update(next);
    }

    private static boolean release(Index next, Blob blob, Owner owner, long now) {
        Lease lease = blob.leases.get(owner.name);
        if (lease == null || !lease.owner.equals(owner)) return false;
        Map<String, Lease> leases = new LinkedHashMap<>(blob.leases);
        leases.remove(owner.name);
        next.blobs.put(blob.handle, withLeases(blob, leases, now));
        return true;
    }

    @Override public void waitForIdle(RemoteCallback callback) {
        Objects.requireNonNull(callback, "callback");
        if (!mCallers.isSystem() && mPm.permissions().checkPermission(mCallers.callerPackage(),
                android.Manifest.permission.DUMP) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Waiting for BlobStore requires DUMP");
        }
        mWorker.execute(() -> {
            maintain();
            callback.sendResult(new Bundle());
        });
    }

    @Override public synchronized List<BlobInfo> queryBlobsForUser(int userId) {
        requireSystem();
        if (userId < 0 && userId != UserHandle.USER_CURRENT) {
            throw new IllegalArgumentException("Invalid blob user");
        }
        if (mRoot == null || (userId != 0 && userId != UserHandle.USER_CURRENT)) return List.of();
        long now = System.currentTimeMillis();
        List<BlobInfo> result = new ArrayList<>();
        for (Blob blob : index().blobs.values()) {
            if (blob.handle.isExpired() || !validFile(blob.fileName)) continue;
            List<LeaseInfo> leases = new ArrayList<>();
            for (Lease lease : blob.leases.values()) {
                if (lease.active(now) && currentlyInstalled(lease.owner)) {
                    long expiry = lease.expiry == 0 ? blob.handle.getExpiryTimeMillis() : lease.expiry;
                    leases.add(leaseInfo(lease, expiry));
                }
            }
            if (leases.isEmpty() && blob.contributions.values().stream()
                    .noneMatch(contribution -> currentlyInstalled(contribution.owner))) continue;
            result.add(new BlobInfo(blob.id, blob.handle.getExpiryTimeMillis(),
                    blob.handle.getLabel(), blob.size, leases));
        }
        return result;
    }

    @Override public synchronized void deleteBlob(long id) {
        requireSystem();
        for (Blob blob : index().blobs.values()) {
            if (blob.id != id) continue;
            Index next = new Index(mIndex);
            next.blobs.remove(blob.handle);
            update(next);
            BlobStoreFiles.discard(dataFile(blob.fileName));
            return;
        }
    }

    private synchronized void maintain() {
        if (mIndex == null) return;
        try {
            prune();
        } catch (IOException | RuntimeException error) {
            SystemServer.log("BlobStore maintenance unavailable: " + error.getClass().getSimpleName());
        }
    }

    private void prune() throws IOException {
        long now = System.currentTimeMillis();
        Index next = new Index(mIndex);
        List<Stage> expiredStages = new ArrayList<>();
        for (Stage stage : mIndex.sessions.values()) {
            // No committing operation shares the worker with maintenance.
            // Removal-pending packages still exist until Installer commits removal.
            if (mConsumed.contains(stage.id) || !validFile(stage.fileName) || !currentlyInstalled(stage.owner)
                    || now - dataFile(stage.fileName).lastModified() >= SESSION_LIFETIME
                    || stage.handle.isExpired()) {
                next.sessions.remove(stage.id);
                expiredStages.add(stage);
            }
        }
        for (Blob blob : mIndex.blobs.values()) {
            if (blob.handle.isExpired() || !validFile(blob.fileName)) {
                next.blobs.remove(blob.handle);
                continue;
            }
            Map<String, Contribution> contributions = new LinkedHashMap<>(blob.contributions);
            contributions.values().removeIf(item -> !currentlyInstalled(item.owner));
            Map<String, Lease> leases = new LinkedHashMap<>(blob.leases);
            leases.values().removeIf(lease -> !lease.active(now) || !currentlyInstalled(lease.owner));
            long unleased = leases.isEmpty()
                    ? (blob.unleasedSince == 0 ? now : blob.unleasedSince) : 0;
            boolean leaseGraceEnded = now - unleased >= LEASE_WAIT
                    && contributions.values().stream().noneMatch(
                            item -> now - item.committedAt < LEASE_WAIT);
            if (leases.isEmpty() && (contributions.isEmpty() || leaseGraceEnded)) {
                next.blobs.remove(blob.handle);
            } else if (!contributions.equals(blob.contributions) || !leases.equals(blob.leases)) {
                next.blobs.put(blob.handle, new Blob(blob.id, blob.handle, blob.fileName, blob.size,
                        unleased, contributions, leases));
            }
        }
        if (!next.sessions.equals(mIndex.sessions) || !next.blobs.equals(mIndex.blobs)) {
            update(next);
            for (Stage stage : expiredStages) {
                mOpen.remove(stage.id);
                mConsumed.remove(stage.id);
            }
        }
        var retained = new java.util.HashSet<String>();
        for (Stage stage : mIndex.sessions.values()) {
            retained.add(stage.fileName);
            if (mConsumed.contains(stage.id)) retained.add(consumedMarker(stage).getName());
        }
        for (Blob blob : mIndex.blobs.values()) retained.add(blob.fileName);
        try (var files = Files.newDirectoryStream(mRoot.toPath())) {
            for (var file : files) {
                String name = file.getFileName().toString();
                boolean marker = name.endsWith(CONSUMED_SUFFIX)
                        && isDataName(name.substring(0, name.length() - CONSUMED_SUFFIX.length()));
                if ((isDataName(name) || marker || (name.startsWith("index-") && name.endsWith(".tmp")))
                        && !retained.contains(name)) BlobStoreFiles.discard(file.toFile());
            }
        }
    }

    private static boolean isDataName(String name) {
        return name.startsWith("data-") && name.endsWith(".bin")
                && name.indexOf('/') < 0 && name.indexOf('\\') < 0 && name.indexOf(':') < 0;
    }

    private static String readName(DataInputStream input) throws IOException {
        String name = input.readUTF();
        if (!isDataName(name)) throw new IOException("Invalid BlobStore filename");
        return name;
    }

    private static int readCount(DataInputStream input, int maximum) throws IOException {
        int count = input.readInt();
        if (count < 0 || count > maximum) throw new IOException("Invalid BlobStore entry count");
        return count;
    }

    private static void writeCount(DataOutputStream output, int count, int maximum) throws IOException {
        if (count > maximum) throw new IOException("BlobStore index entry limit");
        output.writeInt(count);
    }

    private Index readIndex() throws IOException {
        Index result = new Index();
        File file = dataFile("index");
        if (Files.notExists(file.toPath(), LinkOption.NOFOLLOW_LINKS)) return result;
        if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("Invalid BlobStore index file");
        }
        try (var input = new DataInputStream(new BufferedInputStream(new FileInputStream(file)))) {
            if (input.readInt() != INDEX_VERSION) throw new IOException("Unknown BlobStore index version");
            for (int count = readCount(input, MAX_INDEX_ENTRIES); count > 0; count--) {
                long id = input.readLong();
                Stage stage = new Stage(id, readOwner(input), readHandle(input),
                        readName(input), readAccess(input));
                if (id <= 0 || result.sessions.put(id, stage) != null) {
                    throw new IOException("Duplicate or invalid BlobStore session");
                }
            }
            for (int count = readCount(input, MAX_INDEX_ENTRIES); count > 0; count--) {
                long id = input.readLong();
                BlobHandle handle = readHandle(input);
                String name = readName(input);
                long size = input.readLong(), unleased = input.readLong();
                Map<String, Contribution> contributions = new LinkedHashMap<>();
                Map<String, Lease> leases = new LinkedHashMap<>();
                for (int entries = readCount(input, MAX_INDEX_ENTRIES); entries > 0; entries--) {
                    Owner owner = readOwner(input);
                    Contribution item = new Contribution(owner, input.readLong(), readAccess(input));
                    if (contributions.put(owner.name, item) != null) {
                        throw new IOException("Duplicate BlobStore contribution");
                    }
                }
                for (int entries = readCount(input, MAX_INDEX_ENTRIES); entries > 0; entries--) {
                    Owner owner = readOwner(input);
                    long expiry = input.readLong();
                    String resource = input.readBoolean() ? input.readUTF() : null;
                    String description = input.readBoolean() ? input.readUTF() : null;
                    if (leases.put(owner.name, new Lease(owner, expiry, resource, description)) != null) {
                        throw new IOException("Duplicate BlobStore lease");
                    }
                }
                Blob blob = new Blob(id, handle, name, size, unleased, contributions, leases);
                if (id <= 0 || size < 0 || result.blobs.put(handle, blob) != null) {
                    throw new IOException("Duplicate or invalid shared blob");
                }
            }
            if (input.read() != -1) throw new IOException("Trailing data in BlobStore index");
        } catch (IllegalArgumentException | NullPointerException corrupt) {
            throw new IOException("Invalid BlobStore metadata", corrupt);
        }
        return result;
    }

    private static void writeIndex(DataOutputStream output, Index index) throws IOException {
        output.writeInt(INDEX_VERSION);
        writeCount(output, index.sessions.size(), MAX_INDEX_ENTRIES);
        for (Stage stage : index.sessions.values()) {
            output.writeLong(stage.id);
            writeOwner(output, stage.owner);
            writeHandle(output, stage.handle);
            output.writeUTF(stage.fileName);
            writeAccess(output, stage.access);
        }
        writeCount(output, index.blobs.size(), MAX_INDEX_ENTRIES);
        for (Blob blob : index.blobs.values()) {
            output.writeLong(blob.id);
            writeHandle(output, blob.handle);
            output.writeUTF(blob.fileName);
            output.writeLong(blob.size);
            output.writeLong(blob.unleasedSince);
            writeCount(output, blob.contributions.size(), MAX_INDEX_ENTRIES);
            for (Contribution item : blob.contributions.values()) {
                writeOwner(output, item.owner);
                output.writeLong(item.committedAt);
                writeAccess(output, item.access);
            }
            writeCount(output, blob.leases.size(), MAX_INDEX_ENTRIES);
            for (Lease lease : blob.leases.values()) {
                writeOwner(output, lease.owner);
                output.writeLong(lease.expiry);
                output.writeBoolean(lease.resourceName != null);
                if (lease.resourceName != null) output.writeUTF(lease.resourceName);
                output.writeBoolean(lease.description != null);
                if (lease.description != null) output.writeUTF(lease.description);
            }
        }
    }

    private static Owner readOwner(DataInputStream input) throws IOException {
        return new Owner(input.readUTF(), input.readLong(), input.readUTF());
    }

    private static void writeOwner(DataOutputStream output, Owner owner) throws IOException {
        output.writeUTF(owner.name);
        output.writeLong(owner.installedAt);
        output.writeUTF(owner.unverifiedCode);
    }

    private static BlobHandle readHandle(DataInputStream input) throws IOException {
        byte[] digest = new byte[readCount(input, 1024)];
        input.readFully(digest);
        return BlobHandle.createWithSha256(digest, input.readUTF(), input.readLong(), input.readUTF());
    }

    private static void writeHandle(DataOutputStream output, BlobHandle handle) throws IOException {
        byte[] digest = handle.getSha256Digest();
        writeCount(output, digest.length, 1024);
        output.write(digest);
        output.writeUTF(handle.getLabel().toString());
        output.writeLong(handle.getExpiryTimeMillis());
        output.writeUTF(handle.getTag());
    }

    private static Access readAccess(DataInputStream input) throws IOException {
        boolean readable = input.readBoolean(), sameSignature = input.readBoolean();
        List<Grant> grants = new ArrayList<>();
        for (int count = readCount(input, MAX_GRANTS); count > 0; count--) {
            String name = input.readUTF();
            byte[] certificate = new byte[32];
            input.readFully(certificate);
            grants.add(new Grant(name, certificate));
        }
        return new Access(readable, sameSignature, grants);
    }

    private static void writeAccess(DataOutputStream output, Access access) throws IOException {
        output.writeBoolean(access.publiclyReadable);
        output.writeBoolean(access.sameSignature);
        output.writeInt(access.grants.size());
        for (Grant grant : access.grants) {
            output.writeUTF(grant.name);
            output.write(grant.sha256);
        }
    }
}
