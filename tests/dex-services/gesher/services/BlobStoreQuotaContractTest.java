/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.blob.BlobHandle;
import android.app.blob.BlobStoreManager;
import android.app.blob.IBlobCommitCallback;
import android.app.blob.IBlobStoreManager;
import android.app.blob.IBlobStoreSession;
import android.os.LimitExceededException;
import android.os.ParcelableException;

import com.gesher.server.CallerIdentity;
import com.gesher.server.PackageManagerService;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** Actual service admission and accounting, with bounded metadata seeded at quota boundaries.
 * Does not substitute for the descriptor/commit-sealing or remote Binder fixtures. */
public final class BlobStoreQuotaContractTest {
    private static int checks;
    private static final byte[] BYTES = {42};
    private static final String OWNER = "android";
    private interface Operation { void run() throws Exception; }

    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    private static Object field(Object object, String name) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(object);
    }

    private static Object record(String name, Class<?>[] types, Object... values) throws Exception {
        Class<?> type = Class.forName("com.gesher.server.BlobStoreService$" + name);
        Constructor<?> constructor = type.getDeclaredConstructor(types);
        constructor.setAccessible(true);
        return constructor.newInstance(values);
    }

    private static Object component(Object record, String name) throws Exception {
        Method method = record.getClass().getDeclaredMethod(name);
        method.setAccessible(true);
        return method.invoke(record);
    }

    private static void limited(Operation operation, String reason) throws Exception {
        try { operation.run(); }
        catch (ParcelableException failure) {
            check(failure.getCause() instanceof LimitExceededException, reason + ": wrong error");
            return;
        }
        throw new AssertionError(reason);
    }

    private static final class Fixture implements AutoCloseable {
        final Path root = Files.createTempDirectory("gesher-blob-quota-");
        final IBlobStoreManager service;
        final ScheduledThreadPoolExecutor worker;
        final List<BlobHandle> handles = new ArrayList<>();
        final Map<BlobHandle, Object> blobs;
        final long quota;

        @SuppressWarnings("unchecked")
        Fixture(int count, long size, boolean leased) throws Exception {
            Class<?> type = Class.forName("com.gesher.server.BlobStoreService");
            Constructor<?> constructor = type.getDeclaredConstructor(PackageManagerService.class,
                    CallerIdentity.class, java.io.File.class);
            constructor.setAccessible(true);
            PackageManagerService packages = new PackageManagerService(List.of());
            service = (IBlobStoreManager) constructor.newInstance(packages,
                    new CallerIdentity(packages, null), root.toFile());
            quota = service.getRemainingLeaseQuotaBytes(OWNER);
            worker = (ScheduledThreadPoolExecutor) field(service, "mWorker");
            worker.submit(() -> {}).get(10, TimeUnit.SECONDS); // Initial maintenance has finished.
            blobs = (Map<BlobHandle, Object>) field(field(service, "mIndex"), "blobs");
            Files.write(root.resolve("fixture-data.bin"), BYTES);
            Class<?> ownerType = Class.forName("com.gesher.server.BlobStoreService$Owner");
            Class<?> accessType = Class.forName("com.gesher.server.BlobStoreService$Access");
            Object owner = record("Owner", new Class<?>[]{String.class, long.class, String.class},
                    OWNER, 0L, "");
            Field privateAccess = type.getDeclaredField("PRIVATE");
            privateAccess.setAccessible(true);
            Object contribution = record("Contribution",
                    new Class<?>[]{ownerType, long.class, accessType},
                    owner, System.currentTimeMillis(), privateAccess.get(null));
            Object lease = record("Lease",
                    new Class<?>[]{ownerType, long.class, String.class, String.class},
                    owner, 0L, null, "original");
            // A negative size asks for a single blob exceeding its remaining quota.
            long bytes = size < 0 ? quota / 2 + 1 : size;
            synchronized (service) {
                for (int i = 0; i < count; i++) {
                    BlobHandle handle = BlobHandle.createWithSha256(
                            MessageDigest.getInstance("SHA-256").digest(BYTES),
                            "quota fixture", 0, "quota-" + i);
                    handles.add(handle);
                    blobs.put(handle, record("Blob", new Class<?>[]{long.class, BlobHandle.class,
                                    String.class, long.class, long.class, Map.class, Map.class},
                            (long) i + 1, handle, "fixture-data.bin", bytes,
                            System.currentTimeMillis(), Map.of(OWNER, contribution),
                            leased ? Map.of(OWNER, lease) : Map.of()));
                }
            }
        }

        int recommit(BlobHandle handle) throws Exception {
            long id = service.createSession(handle, OWNER);
            @SuppressWarnings("unchecked")
            Map<Long, Object> sessions = (Map<Long, Object>) field(field(service, "mIndex"), "sessions");
            Object stage = sessions.get(id);
            Files.write(root.resolve((String) component(stage, "fileName")), BYTES);
            IBlobStoreSession session = service.openSession(id, OWNER);
            session.allowPublicAccess();
            CompletableFuture<Integer> completion = new CompletableFuture<>();
            session.commit(new IBlobCommitCallback.Stub() {
                @Override public void onResult(int result) { completion.complete(result); }
            });
            int result = completion.get(10, TimeUnit.SECONDS);
            try { service.openSession(id, OWNER); }
            catch (SecurityException consumed) { checks++; return result; }
            throw new AssertionError("A completed commit must consume its session");
        }

        @Override public void close() throws Exception {
            worker.shutdownNow();
            check(worker.awaitTermination(10, TimeUnit.SECONDS), "BlobStore worker terminated");
            try (var paths = Files.walk(root)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) Files.delete(path);
            }
        }
    }

    public static int contracts() throws Exception {
        int before = checks;
        try (Fixture f = new Fixture(500, 1, true)) {
            BlobHandle handle = f.handles.get(0);
            limited(() -> f.service.acquireLease(handle, 0, "renewed", 0, OWNER),
                    "Renewal at the leased-blob count limit was accepted");
            check("original".contentEquals(f.service.getLeaseInfo(handle, OWNER).getDescription()),
                    "Rejected renewal changed the existing lease");
        }
        try (Fixture f = new Fixture(499, 1, true)) {
            long beforeQuota = f.service.getRemainingLeaseQuotaBytes(OWNER);
            f.service.acquireLease(f.handles.get(0), 0, "renewed", 0, OWNER);
            check(f.service.getLeasedBlobs(OWNER).size() == 499,
                    "Successful renewal consumed an additional lease slot");
            check(f.service.getRemainingLeaseQuotaBytes(OWNER) == beforeQuota,
                    "Successful renewal charged the blob bytes twice");
        }
        try (Fixture f = new Fixture(1, -1, true)) {
            BlobHandle handle = f.handles.get(0);
            limited(() -> f.service.acquireLease(handle, 0, "renewed", 0, OWNER),
                    "Renewal exceeding remaining byte quota was accepted");
            f.service.releaseLease(handle, OWNER);
            f.service.acquireLease(handle, 0, "reacquired", 0, OWNER);
            check("reacquired".contentEquals(f.service.getLeaseInfo(handle, OWNER).getDescription()),
                    "Available quota did not permit lease acquisition");
        }
        try (Fixture f = new Fixture(1000, 1, false)) {
            BlobHandle handle = f.handles.get(0);
            Object previous = f.blobs.get(handle);
            check(f.recommit(handle) == BlobStoreManager.COMMIT_RESULT_ERROR,
                    "Recommit at the committed-blob count limit was accepted");
            @SuppressWarnings("unchecked")
            Map<BlobHandle, Object> current = (Map<BlobHandle, Object>) field(field(f.service, "mIndex"), "blobs");
            check(current.get(handle) == previous && current.size() == 1000,
                    "Rejected recommit replaced the earlier contribution");
        }
        try (Fixture f = new Fixture(999, 1, false)) {
            BlobHandle handle = f.handles.get(0);
            check(f.recommit(handle) == BlobStoreManager.COMMIT_RESULT_SUCCESS,
                    "Recommit below the committed-blob limit was rejected");
            @SuppressWarnings("unchecked")
            Map<BlobHandle, Object> current = (Map<BlobHandle, Object>) field(field(f.service, "mIndex"), "blobs");
            check(current.size() == 999, "Successful recommit consumed another blob slot");
        }
        return checks - before;
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 0) System.load(args[0]); // Explicit Linux host JNI boundary.
        System.out.println("BlobStore quota contracts passed: " + contracts() + " checks");
    }
}
