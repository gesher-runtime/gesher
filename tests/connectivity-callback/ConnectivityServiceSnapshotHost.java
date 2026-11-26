/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import android.net.ConnectivityManager;
import android.net.Network;
import com.gesher.net.WindowsNetworks;
import com.gesher.server.ConnectivityService;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.Map;

/** Production query service with only Windows adapter snapshots controlled at JNI. */
public final class ConnectivityServiceSnapshotHost {
    private static final String EMPTY = "internet\t0\n";
    private static final String ONLINE = "internet\t1\nnet\t12\tethernet\t1500\t1000000\t1000000\t0\t0\t0\t1\t1\n";
    private static String snapshot;
    private static int reads, checks;
    public static String readSnapshot() { reads++; return snapshot; }
    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name); field.setAccessible(true); return field;
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    public static void contracts() throws Exception {
        Field loaded = field(WindowsNetworks.class, "sLoaded");
        Field system = field(ConnectivityService.class, "sInSystemServer");
        Field instance = field(ConnectivityService.class, "sInstance");
        Field clientService = field(ConnectivityManager.class, "sService");
        Field clientSnapshot = field(ConnectivityManager.class, "sCurrent");
        Object oldLoaded = loaded.get(null), oldSystem = system.get(null), oldInstance = instance.get(null);
        Object oldService = clientService.get(null), oldSnapshot = clientSnapshot.get(null);
        check(((Map<?, ?>)field(ConnectivityManager.class, "sCallbacks").get(null)).isEmpty(),
                "freshness fixture owns no callback subscriptions");
        try {
            loaded.set(null, true); system.set(null, true);
            Constructor<?> constructor = ConnectivityService.class.getDeclaredConstructor(
                    Class.forName("com.gesher.server.PackageManagerService"),
                    Class.forName("com.gesher.server.ActivityTaskManagerService"));
            constructor.setAccessible(true);
            ConnectivityService service = (ConnectivityService)constructor.newInstance(null, null);
            clientService.set(null, service); clientSnapshot.set(null, null);
            ConnectivityManager manager = new ConnectivityManager(null);
            reads = 0; snapshot = EMPTY;
            check(manager.getActiveNetwork() == null, "initial query sees offline adapter snapshot");
            snapshot = ONLINE;
            check(new Network(112).equals(manager.getActiveNetwork()),
                    "query-only client sees reconnect without registering a callback");
            snapshot = ONLINE.replace("net\t12\t", "net\t15\t");
            check(new Network(115).equals(manager.getActiveNetwork()),
                    "query-only client sees the replacement default adapter");
            snapshot = EMPTY;
            check(manager.getAllNetworks().length == 0 && manager.getActiveNetwork() == null,
                    "query-only client sees later disconnect");
            check(reads == 5, "unwatched queries obtain current adapter snapshots");
            Field watcher = field(ConnectivityService.class, "mWatcher");
            check(watcher.get(service) == null, "queries do not start an unused watcher thread");

            // While watching, JNI changes belong to watch()'s event publication.
            // The sentinel models ownership only; no host event thread is claimed.
            watcher.set(service, new Thread()); snapshot = ONLINE;
            Field watching = null;
            try { watching = field(ConnectivityService.class, "mWatching"); watching.set(service, true); }
            catch (NoSuchFieldException baseline) { /* Older source used thread presence as ownership. */ }
            check(new Network(112).equals(manager.getActiveNetwork()), "watched cache is initialized");
            int before = reads;
            field(ConnectivityService.class, "mSnapshot").set(service, EMPTY);
            check(manager.getActiveNetwork() == null && reads == before,
                    "watched queries use the event-published cache without repeated enumeration");
            watcher.set(service, null);
            if (watching != null) watching.set(service, false);
            System.out.println("ConnectivityServiceSnapshotHost all passed: " + checks);
        } finally {
            clientService.set(null, oldService); clientSnapshot.set(null, oldSnapshot);
            loaded.set(null, oldLoaded); system.set(null, oldSystem); instance.set(null, oldInstance);
        }
    }
}
