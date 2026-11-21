/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.nsd;

import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import com.gesher.net.WindowsNsd;

import java.lang.reflect.Constructor;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.concurrent.Executor;

/** Deliberate old-source negative control, never a current-runtime success gate. */
public final class NsdResolveOldSourceControl {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("host native library, isolated old class directory");
        System.load(args[0]);
        Path origin = Path.of(NsdManager.class.getProtectionDomain().getCodeSource().getLocation().toURI());
        require(origin.toRealPath().equals(Path.of(args[1]).toRealPath()), "old manager must own its class definition");
        ArrayDeque<Runnable> queue = new ArrayDeque<>();
        Constructor<NsdManager> ctor = NsdManager.class.getDeclaredConstructor(Executor.class, WindowsNsd.Factory.class);
        ctor.setAccessible(true);
        WindowsNsd.Factory unusedBrowse = new WindowsNsd.Factory() {
            public WindowsNsd.Session start(String type, String subtype, WindowsNsd.Scope scope,
                    WindowsNsd.Callback callback) {
                throw new AssertionError("negative resolution control cannot browse");
            }
            public int[] interfaces() { throw new AssertionError("negative control cannot enumerate interfaces"); }
        };
        NsdManager manager = ctor.newInstance((Executor) queue::add, unusedBrowse);
        NsdServiceInfo input = new NsdServiceInfo();
        input.setServiceName("Owned negative control"); input.setServiceType("_grnegative._tcp");
        NsdServiceInfo[] failed = {null}; int[] error = {-1};
        NsdManager.ResolveListener listener = new NsdManager.ResolveListener() {
            public void onServiceResolved(NsdServiceInfo info) { throw new AssertionError("base unexpectedly resolved"); }
            public void onResolveFailed(NsdServiceInfo info, int code) { failed[0] = info; error[0] = code; }
        };
        manager.resolveService(input, Runnable::run, listener);
        require(failed[0] == null && queue.size() == 1, "base queues its unconditional unsupported outcome");
        while (!queue.isEmpty()) queue.remove().run();
        require(error[0] == NsdManager.FAILURE_INTERNAL_ERROR, "base always reports internal failure");
        require(failed[0] != input && failed[0].getServiceName().equals(input.getServiceName()),
                "base failure returned a copy rather than the original request");
        manager.stopServiceResolution(listener);
        require(queue.isEmpty(), "base cancellation is a silent no-op");
        System.out.println("NsdResolveOldSourceControl observed: unsupported-failure=0; requestCopied=true; cancelNoOp=true; provider="
                + origin + "; windowsExecution=false");
    }
}
