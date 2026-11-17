/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.app.blob.BlobHandle;
import android.app.blob.BlobInfo;
import android.app.blob.BlobStoreManager;
import android.app.blob.IBlobStoreManager;
import android.app.blob.IBlobStoreSession;
import android.content.Context;
import android.content.res.AssetManager;
import android.os.IBinder;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.RemoteCallback;
import android.os.ServiceManager;
import android.os.UserHandle;

import com.gesher.server.Services;

import java.io.IOException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

/** Exercises public Android BlobStore contracts with the managed runtime's real disk backend. */
public final class BlobStoreContractTest {
    private static final long CALLBACK_SECONDS = 20;
    private static int checks;

    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    private interface Operation { void run() throws Exception; }

    private static void expect(Class<? extends Throwable> type, Operation operation, String reason)
            throws Exception {
        try { operation.run(); }
        catch (Throwable failure) {
            if (!type.isInstance(failure)) throw new AssertionError(reason, failure);
            checks++;
            return;
        }
        throw new AssertionError(reason);
    }

    private static byte[] read(ParcelFileDescriptor descriptor) throws IOException {
        try (var stream = new ParcelFileDescriptor.AutoCloseInputStream(descriptor)) {
            return stream.readAllBytes();
        }
    }

    private static void write(ParcelFileDescriptor descriptor, byte[] bytes) throws IOException {
        try (var stream = new ParcelFileDescriptor.AutoCloseOutputStream(descriptor)) {
            stream.write(bytes);
        }
    }

    private static int commit(BlobStoreManager.Session session) throws Exception {
        CompletableFuture<Integer> result = new CompletableFuture<>();
        session.commit(Runnable::run, result::complete);
        return result.get(CALLBACK_SECONDS, TimeUnit.SECONDS);
    }

    private static BlobHandle handle(byte[] bytes, String tag, long expiry) throws Exception {
        return BlobHandle.createWithSha256(MessageDigest.getInstance("SHA-256").digest(bytes),
                "Gesher BlobStore fixture", expiry, tag);
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Map<String, IBinder> created = Services.create(Collections.emptyList());
        IBinder binder = created.get(Context.BLOB_STORE_SERVICE);
        check(binder != null, "blob_store service is registered");
        Field cacheField = ServiceManager.class.getDeclaredField("sCache");
        cacheField.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<String, IBinder> cache = (Map<String, IBinder>) cacheField.get(null);
        cache.putAll(created);
        Constructor<ActivityThread> constructor = ActivityThread.class.getDeclaredConstructor();
        constructor.setAccessible(true);
        Context context = constructor.newInstance().getSystemContext();
        Object lookup = context.getSystemService(Context.BLOB_STORE_SERVICE);
        check(lookup instanceof BlobStoreManager, "real ContextImpl constructs BlobStoreManager");
        BlobStoreManager manager = (BlobStoreManager) lookup;
        check(manager == context.getSystemService(BlobStoreManager.class), "typed and named lookup agree");
        check(manager == context.getSystemService(Context.BLOB_STORE_SERVICE), "manager is cached");
        check(manager.getClass() == BlobStoreManager.class, "unmodified framework client implementation");
        check(manager.getClass().getClassLoader() == Object.class.getClassLoader(), "manager is on boot classpath");

        byte[] bytes = "Android blob: immutable after commit".getBytes(StandardCharsets.UTF_8);
        byte[] corrupt = "invalid payload".getBytes(StandardCharsets.UTF_8);
        String tag = UUID.randomUUID().toString();
        long expiry = System.currentTimeMillis() + TimeUnit.HOURS.toMillis(1);
        BlobHandle handle = handle(bytes, tag, expiry);
        IBlobStoreManager service = IBlobStoreManager.Stub.asInterface(binder);
        long quota = manager.getRemainingLeaseQuotaBytes();
        check(quota >= 300L * 1024 * 1024, "quota reflects managed storage");
        check(!manager.getLeasedBlobs().contains(handle), "new handle is unleased");
        expect(SecurityException.class, () -> service.getLeasedBlobs("com.gesher.uninstalled"),
                "unknown package cannot request lease metadata");
        expect(SecurityException.class, () -> manager.openBlob(handle), "uncommitted blob is inaccessible");
        expect(SecurityException.class, () -> manager.getLeaseInfo(handle), "inaccessible lease is rejected");
        expect(IllegalArgumentException.class, () -> manager.openSession(0), "invalid session ID is rejected");
        expect(SecurityException.class, () -> manager.openSession(Long.MAX_VALUE),
                "unknown session ID is rejected");

        long id = manager.createSession(handle);
        check(id > 0, "session ID is positive");
        int prefix = 7;
        BlobStoreManager.Session initial = manager.openSession(id);
        check(!initial.isPublicAccessAllowed(), "sessions begin private");
        byte[] certificate = new byte[32];
        byte[] other = new byte[32];
        other[0] = 1;
        initial.allowPackageAccess("com.gesher.recipient", certificate);
        check(initial.isPackageAccessAllowed("com.gesher.recipient", certificate), "package grant retained");
        check(!initial.isPackageAccessAllowed("com.gesher.recipient", other), "certificate must match");
        initial.allowSameSignatureAccess();
        check(initial.isSameSignatureAccessAllowed(), "same-signer access mode retained");
        initial.allowPublicAccess();
        check(initial.isPublicAccessAllowed(), "public access mode retained");
        write(initial.openWrite(0, -1), Arrays.copyOf(bytes, prefix));
        check(initial.getSize() == prefix, "partial length is reported");
        check(Arrays.equals(Arrays.copyOf(bytes, prefix), read(initial.openRead())),
                "session read sees the current bytes");
        initial.close();
        expect(IllegalStateException.class, initial::openRead, "closed Binder cannot reopen a descriptor");

        try (BlobStoreManager.Session resumed = manager.openSession(id)) {
            check(resumed.getSize() == prefix && resumed.isPublicAccessAllowed(),
                    "reopen preserves both bytes and access modes");
            write(resumed.openWrite(prefix, bytes.length), Arrays.copyOfRange(bytes, prefix, bytes.length));
            check(Arrays.equals(bytes, read(resumed.openRead())), "offset write preserves the prefix");
            try (var stale = new ParcelFileDescriptor.AutoCloseOutputStream(resumed.openWrite(0, -1))) {
                check(commit(resumed) == BlobStoreManager.COMMIT_RESULT_SUCCESS, "verified commit succeeds");
                try { stale.write(corrupt); } catch (IOException revoked) { }
                check(Arrays.equals(bytes, read(manager.openBlob(handle))), "committed data is immutable");
            }
        }
        expect(SecurityException.class, () -> manager.openSession(id), "committed session cannot reopen");
        check(manager.getLeaseInfo(handle) == null, "available blob without caller lease returns null");

        BlobHandle mismatch = handle(bytes, tag + "-mismatch", expiry);
        long mismatchId = manager.createSession(mismatch);
        try (BlobStoreManager.Session session = manager.openSession(mismatchId)) {
            write(session.openWrite(0, -1), corrupt);
            check(commit(session) == BlobStoreManager.COMMIT_RESULT_ERROR, "digest mismatch fails asynchronously");
        }
        expect(SecurityException.class, () -> manager.openBlob(mismatch), "bad digest publishes no blob");
        expect(SecurityException.class, () -> manager.openSession(mismatchId), "failed commit consumes session");
        long abandoned = manager.createSession(mismatch);
        manager.abandonSession(abandoned);
        expect(SecurityException.class, () -> manager.openSession(abandoned), "abandon deletes the session");

        expect(IllegalArgumentException.class, () -> manager.acquireLease(handle, "too long", expiry + 1),
                "lease cannot outlive the blob");
        manager.acquireLease(handle, "expires immediately", System.currentTimeMillis() - 1000);
        check(!manager.getLeasedBlobs().contains(handle), "expired lease is omitted");
        manager.acquireLease(handle, android.R.string.ok);
        check(manager.getLeaseInfo(handle).getDescriptionResId() == android.R.string.ok,
                "resource-based description retains its resource identity");
        manager.acquireLease(handle, "persistent consumer");
        check(manager.getLeasedBlobs().contains(handle), "lease listing includes the blob");
        check("persistent consumer".contentEquals(manager.getLeaseInfo(handle).getDescription()),
                "lease metadata is returned");
        check(manager.getRemainingLeaseQuotaBytes() == quota - bytes.length,
                "replacement lease charges actual bytes once");

        BlobInfo info = manager.queryBlobsForUser(UserHandle.of(0)).stream()
                .filter(blob -> blob.getLabel().equals(handle.getLabel()) && blob.getExpiryTimeMs() == expiry)
                .findFirst().orElseThrow();
        check(info.getSizeBytes() == bytes.length && info.getLeases().size() == 1,
                "administrative metadata includes size and active lease");
        check(service.queryBlobsForUser(10).isEmpty(), "unknown user does not expose managed blobs");
        CompletableFuture<Boolean> idle = new CompletableFuture<>();
        service.waitForIdle(new RemoteCallback(bundle -> idle.complete(bundle != null && bundle.isEmpty())));
        check(idle.get(CALLBACK_SECONDS, TimeUnit.SECONDS), "idle callback follows pending writes");
        expect(NullPointerException.class, () -> service.waitForIdle(null), "idle callback is required");

        BlobHandle pending = handle(bytes, tag + "-pending", expiry);
        long pendingId = manager.createSession(pending);
        try (BlobStoreManager.Session session = manager.openSession(pendingId)) {
            write(session.openWrite(0, -1), Arrays.copyOf(bytes, prefix));
        }
        IBinder registryAfterRestart = Services.create(Collections.emptyList()).get(Context.BLOB_STORE_SERVICE);
        IBlobStoreManager restored = IBlobStoreManager.Stub.asInterface(registryAfterRestart);
        check(restored.getLeasedBlobs("android").contains(handle), "new service reloads durable lease");
        check(Arrays.equals(bytes, read(restored.openBlob(handle, "android"))), "new service reloads committed bytes");
        IBlobStoreSession sessionAfterRestart = restored.openSession(pendingId, "android");
        check(sessionAfterRestart.getSize() == prefix, "new service reloads partial session");
        sessionAfterRestart.abandon();
        restored.releaseAllLeases("android");
        check(!restored.getLeasedBlobs("android").contains(handle), "release-all clears ownership leases");
        check(restored.getRemainingLeaseQuotaBytes("android") == quota, "released bytes restore quota");
        restored.deleteBlob(info.getId());
        expect(SecurityException.class, () -> restored.openBlob(handle, "android"),
                "system deletion removes published bytes from lookups");
        List<BlobInfo> remaining = restored.queryBlobsForUser(0);
        check(remaining.stream().noneMatch(blob -> blob.getId() == info.getId()),
                "deleted blob metadata stays absent");

        System.out.println("BlobStoreContractTest all passed: " + checks);
        System.exit(0);
    }
}
