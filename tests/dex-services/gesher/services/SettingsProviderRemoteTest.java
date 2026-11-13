/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ContentProviderHolder;
import android.app.IActivityManager;
import android.content.AttributionSource;
import android.content.ContentProviderNative;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.IContentProvider;
import android.content.IContentService;
import android.database.Cursor;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.ServiceManager;
import com.gesher.app.Main;
import com.gesher.server.Services;
import com.gesher.server.StandIn;
import com.gesher.server.SystemServer;
import java.io.File;
import java.nio.file.Files;
import java.util.Collections;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Ordinary-UID ContentProvider.Transport calls over two real Windows ART processes. */
public final class SettingsProviderRemoteTest {
    private static final String NAME = "gesher.settings.contract";
    private static int checks;
    private static synchronized void check(boolean result, String why) { if (!result) throw new AssertionError(why); checks++; }
    private static void server(String readyFile) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        Main.bootstrap("settings-contract", root + "/build.prop", root + "/framework/framework-res.apk");
        check(Process.myUid() == Process.SYSTEM_UID, "Server must run as the system UID");
        Map<String, IBinder> services = Services.create(Collections.emptyList());
        // ContentProvider.getType asks this same policy-backed service in production.
        services.put("permission_checker", StandIn.of(
                android.permission.IPermissionChecker.class,
                new Services.PermissionChecker()).asBinder());
        for (Map.Entry<String, IBinder> service : services.entrySet()) ServiceManager.addService(service.getKey(), service.getValue());
        IActivityManager am = IActivityManager.Stub.asInterface(services.get("activity"));
        ContentProviderHolder holder = am.getContentProviderExternal("settings", 0, null, "settings-contract");
        check(holder != null && holder.provider != null, "Real settings provider holder");
        ServiceManager.addService(NAME, holder.provider.asBinder());
        SystemServer.startBinderThreads("settings-contract", 4);
        Files.write(new File(readyFile).toPath(), new byte[]{1});
        System.out.println("SettingsProviderRemoteTest server ready"); System.out.flush();
        Binder.joinThreadPool();
    }
    private static void client() throws Exception {
        System.loadLibrary("android_runtime");
        check(Process.myUid() == 10042, "Client must use an ordinary distinct UID");
        IBinder binder = ServiceManager.getService(NAME);
        check(binder != null && !(binder instanceof Binder), "Provider is a remote BinderProxy");
        IContentProvider provider = ContentProviderNative.asInterface(binder);
        AttributionSource caller = new AttributionSource.Builder(Process.myUid()).setPackageName("gesher.settings.contract").build();
        Uri table = Uri.parse("content://settings/system"), item = Uri.withAppendedPath(table, "remote-contract");
        IContentService content = IContentService.Stub.asInterface(ServiceManager.getService("content"));
        check(content != null && !(content.asBinder() instanceof Binder), "Content service is remote");
        CountDownLatch observed = new CountDownLatch(3);
        AtomicReference<Throwable> callbackFailure = new AtomicReference<>();
        IContentObserver observer = new IContentObserver.Stub() {
            private void changed(Uri uri) {
                try {
                    check(item.equals(uri), "Exact observer URI");
                    check(Binder.getCallingUid() == Process.SYSTEM_UID, "Observer delivered as system identity");
                    provider.call(caller, "settings", "GET_system", "remote-contract", null);
                } catch (Throwable error) { callbackFailure.compareAndSet(null, error); }
                finally { observed.countDown(); }
            }
            @Override public void onChange(boolean self, Uri uri, int user) { changed(uri); }
            @Override public void onChangeEtc(boolean self, Uri[] uris, int flags, int user) {
                for (Uri uri : uris) changed(uri);
            }
        };
        SystemServer.startBinderThreads("settings-client", 2);
        content.registerContentObserver(item, false, observer, 0, 36);
        try {
            check("vnd.android.cursor.dir/system".equals(provider.getType(caller, table)), "Remote directory MIME");
            ContentValues values = new ContentValues(); values.put("name", "remote-contract"); values.put("value", "first");
            check(item.equals(provider.insert(caller, table, values, null)), "Remote insert URI");
            check("first".equals(provider.call(caller, "settings", "GET_system", "remote-contract", null).getString("value")), "Remote GET matches insert");
            try (Cursor cursor = provider.query(caller, item, new String[]{"value", "_id", "name"}, null, null)) {
                check(cursor != null && cursor.moveToFirst() && "first".equals(cursor.getString(0))
                        && "remote-contract".equals(cursor.getString(2)), "Remote query and cursor transfer");
            }
            values.putNull("value");
            check(provider.update(caller, item, values, null) == 1, "Remote nullable update");
            try (Cursor cursor = provider.query(caller, item, new String[]{"value"}, null, null)) {
                check(cursor != null && cursor.moveToFirst() && cursor.isNull(0), "Remote null row retained");
            }
            check(provider.delete(caller, item, null) == 1, "Remote delete count");
            check(observed.await(10, TimeUnit.SECONDS), "Remote observer callbacks timed out");
            if (callbackFailure.get() != null) throw new AssertionError("Remote callback failed", callbackFailure.get());
        } finally { content.unregisterContentObserver(observer); }
        System.out.println("SettingsProviderRemoteTest all passed: " + checks
                + " checks; ordinary UID, BinderProxy, CRUD, nullable CursorWindow and reentrant observers");
    }
    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("server")) {
            // Main.bootstrap owns the server JNI library in the framework class loader.
            server(args[1]);
        } else client();
    }
}
