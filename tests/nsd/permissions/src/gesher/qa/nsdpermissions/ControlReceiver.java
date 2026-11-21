package gesher.qa.nsdpermissions;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.net.Network;
import android.net.NetworkRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.os.Process;
import android.os.SystemClock;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

/** Original SDK-only observation fixture. It makes no assumption about permission outcomes. */
public final class ControlReceiver extends BroadcastReceiver {
    private static final String PREFIX = "NSD_APK_PERMISSION_REFERENCE:";

    private static void put(JSONObject object, String key, Object value) {
        try { object.put(key, value == null ? JSONObject.NULL : value); }
        catch (org.json.JSONException impossible) { throw new AssertionError(impossible); }
    }
    private static JSONObject exception(Throwable error) {
        JSONObject value = new JSONObject();
        put(value, "class", error.getClass().getName());
        put(value, "message", error.getMessage());
        Throwable cause = error.getCause();
        if (cause != null && cause != error) {
            put(value, "causeClass", cause.getClass().getName());
            put(value, "causeMessage", cause.getMessage());
        }
        return value;
    }

    @Override public void onReceive(Context context, Intent request) {
        PendingResult pending = goAsync();
        Context application = context.getApplicationContext();
        new Thread(() -> {
            JSONObject result = observe(application, request);
            String text = PREFIX + result;
            android.util.Log.i("NSD_APK_REFERENCE", text);
            pending.setResultCode(0);
            pending.setResultData(text);
            pending.finish();
        }, "nsd-apk-measurement").start();
    }

    private static final class Listener implements NsdManager.DiscoveryListener {
        final long began = SystemClock.elapsedRealtime();
        final List<JSONObject> events = new ArrayList<>();
        final CountDownLatch initial = new CountDownLatch(1);
        final CountDownLatch terminal = new CountDownLatch(1);
        volatile String firstOutcome;
        private synchronized void event(String name, String type, Integer error) {
            JSONObject value = new JSONObject();
            put(value, "event", name);
            put(value, "type", type);
            put(value, "error", error);
            put(value, "elapsedMillis", SystemClock.elapsedRealtime() - began);
            put(value, "thread", Thread.currentThread().getName());
            events.add(value);
        }
        synchronized JSONArray snapshot() {
            JSONArray values = new JSONArray();
            for (JSONObject event : events) values.put(event);
            return values;
        }
        public void onDiscoveryStarted(String type) {
            event("started", type, null); firstOutcome = "started"; initial.countDown();
        }
        public void onStartDiscoveryFailed(String type, int error) {
            event("start_failed", type, error); firstOutcome = "start_failed";
            initial.countDown(); terminal.countDown();
        }
        public void onDiscoveryStopped(String type) {
            event("stopped", type, null); terminal.countDown();
        }
        public void onStopDiscoveryFailed(String type, int error) {
            event("stop_failed", type, error); terminal.countDown();
        }
        public void onServiceFound(NsdServiceInfo value) {
            // Only the owned random service type is queried. No device names/addresses are logged.
            event("found", value.getServiceType(), null);
        }
        public void onServiceLost(NsdServiceInfo value) {
            event("lost", value.getServiceType(), null);
        }
    }

    private static JSONObject observe(Context context, Intent request) {
        long began = SystemClock.elapsedRealtime();
        JSONObject result = new JSONObject();
        ExecutorService executor = null;
        Listener listener = null;
        try {
            String mode = request.getStringExtra("mode");
            String type = request.getStringExtra("type");
            if (!List.of("acquire", "ordinary", "request").contains(mode)
                    || type == null || !type.matches("_gq[0-9a-f]{10}\\._tcp")) {
                throw new IllegalArgumentException("Owned reference mode/type required");
            }
            put(result, "mode", mode);
            put(result, "type", type);
            put(result, "package", context.getPackageName());
            put(result, "pid", Process.myPid());
            put(result, "uid", Process.myUid());
            put(result, "callingUidOnWorker", Binder.getCallingUid());
            put(result, "sdk", Build.VERSION.SDK_INT);
            put(result, "targetSdk", context.getApplicationInfo().targetSdkVersion);
            put(result, "internetSelfCheck", context.checkSelfPermission("android.permission.INTERNET"));
            put(result, "networkStateSelfCheck", context.checkSelfPermission("android.permission.ACCESS_NETWORK_STATE"));
            put(result, "discoverCall", mode.equals("acquire") ? "not_requested" : "not_attempted_manager_unavailable");
            NsdManager manager;
            try {
                manager = context.getSystemService(NsdManager.class);
                put(result, "managerAcquisition", manager == null ? "returned_null" : "returned_manager");
            } catch (Throwable failure) {
                put(result, "managerAcquisition", "threw");
                put(result, "managerException", exception(failure));
                put(result, "observationComplete", true);
                return result;
            }
            if (manager == null || mode.equals("acquire")) {
                put(result, "observationComplete", true);
                return result;
            }
            executor = Executors.newSingleThreadExecutor(action -> new Thread(action, "nsd-apk-callback"));
            listener = new Listener();
            boolean returned = false;
            try {
                if (mode.equals("request")) {
                    manager.discoverServices(type, NsdManager.PROTOCOL_DNS_SD,
                            new NetworkRequest.Builder().build(), executor, listener);
                } else {
                    manager.discoverServices(type, NsdManager.PROTOCOL_DNS_SD,
                            (Network) null, executor, listener);
                }
                returned = true;
                put(result, "discoverCall", "returned");
            } catch (Throwable failure) {
                put(result, "discoverCall", "threw");
                put(result, "discoverException", exception(failure));
            }
            if (returned) {
                put(result, "initialCallbackObserved", listener.initial.await(2500, TimeUnit.MILLISECONDS));
                put(result, "firstCallback", listener.firstOutcome);
                if (!"start_failed".equals(listener.firstOutcome)) {
                    try {
                        manager.stopServiceDiscovery(listener);
                        put(result, "stopCall", "returned");
                        put(result, "terminalCallbackObserved", listener.terminal.await(2500, TimeUnit.MILLISECONDS));
                    } catch (Throwable failure) {
                        put(result, "stopCall", "threw");
                        put(result, "stopException", exception(failure));
                    }
                }
            }
            put(result, "observationComplete", true);
        } catch (Throwable failure) {
            put(result, "fixtureError", exception(failure));
            put(result, "observationComplete", false);
        } finally {
            if (executor != null) {
                executor.shutdown();
                try { put(result, "executorTerminated", executor.awaitTermination(500, TimeUnit.MILLISECONDS)); }
                catch (InterruptedException failure) { Thread.currentThread().interrupt(); put(result, "executorTerminated", false); }
            }
            if (listener != null) put(result, "events", listener.snapshot());
            put(result, "elapsedMillis", SystemClock.elapsedRealtime() - began);
            put(result, "platform", "stock-android-sdk-apk");
            put(result, "candidateWindowsExecution", false);
        }
        return result;
    }
}
