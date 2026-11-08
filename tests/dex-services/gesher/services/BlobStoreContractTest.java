/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.app.blob.BlobHandle;
import android.app.blob.BlobInfo;
import android.app.blob.BlobStoreManager;
import android.app.blob.IBlobStoreManager;
import android.content.Context;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteCallback;
import android.os.ServiceManager;
import android.os.UserHandle;

import com.gesher.server.Services;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

/** Real framework lookup and honest empty queries without a shared-blob backend. */
public final class BlobStoreContractTest {
    private static int checks;

    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    private interface Operation { void run() throws Exception; }

    private static void unsupported(Operation operation, String reason) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { checks++; return; }
        throw new AssertionError(reason);
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

        IBlobStoreManager service = IBlobStoreManager.Stub.asInterface(binder);
        List<BlobHandle> leased = manager.getLeasedBlobs();
        check(leased != null && leased.isEmpty(), "getLeasedBlobs is non-null and empty");
        BlobHandle handle = BlobHandle.createWithSha256(new byte[32], "synthetic blob", 0, "gesher.fixture");
        try { leased.add(handle); } catch (UnsupportedOperationException expected) { }
        check(manager.getLeasedBlobs().isEmpty(), "client mutation cannot create a lease");
        check(service.getLeasedBlobs("com.gesher.other").isEmpty(), "foreign package reveals no leases");
        check(manager.getRemainingLeaseQuotaBytes() == 0, "no storage quota is advertised");
        check(manager.queryBlobsForUser(UserHandle.of(0)).isEmpty(), "no blobs exist for user0");
        check(service.queryBlobsForUser(10).isEmpty(), "unknown user reveals no blobs");
        try { manager.getLeaseInfo(handle); throw new AssertionError("absent blob reported accessible"); }
        catch (SecurityException expected) { checks++; }

        manager.waitForIdle(1000);
        checks++;
        AtomicInteger callbacks = new AtomicInteger();
        service.waitForIdle(new RemoteCallback(result -> {
            if (result != null && result.isEmpty()) callbacks.incrementAndGet();
        }));
        check(callbacks.get() == 1, "empty backend reports idle exactly once");
        try { service.waitForIdle(null); throw new AssertionError("null callback accepted"); }
        catch (NullPointerException expected) { checks++; }

        unsupported(() -> manager.createSession(handle), "session creation fabricated an ID");
        unsupported(() -> manager.openSession(1), "session open fabricated a session");
        unsupported(() -> manager.openBlob(handle), "blob open fabricated a descriptor");
        manager.abandonSession(1); checks++;
        unsupported(() -> manager.acquireLease(handle, "synthetic lease"), "lease acquisition reported success");
        manager.releaseLease(handle); checks++;
        manager.releaseAllLeases(); checks++;
        BlobInfo info = new BlobInfo(1, 0, "synthetic blob", 0, Collections.emptyList());
        unsupported(() -> manager.deleteBlob(info), "unsupported blob deletion reported success");
        check(manager.getLeasedBlobs().isEmpty() && manager.getRemainingLeaseQuotaBytes() == 0,
                "rejected requests leave queries empty and quota unavailable");
        System.out.println("BlobStoreContractTest all passed: " + checks);
        System.exit(0);
    }
}
