/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.ProxyInfo;
import android.net.Uri;
import android.os.Parcel;

import java.net.InetSocketAddress;
import java.net.Proxy;
import java.util.List;

/** Nullable proxy snapshots and PAC setup before the local proxy has a port. */
public final class ProxyInfoContractTest {
    private static int checks;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static void rejectsNull(Runnable action) {
        try {
            action.run();
        } catch (NullPointerException expected) {
            checks++;
            return;
        }
        throw new AssertionError("null PAC URI must be rejected");
    }

    private static ProxyInfo roundTrip(ProxyInfo value) {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            ProxyInfo copy = ProxyInfo.CREATOR.createFromParcel(parcel);
            check(parcel.dataAvail() == 0, "complete proxy parcel consumed");
            check(copy.equals(value), "proxy parcel equality");
            check(copy.hashCode() == value.hashCode(), "proxy parcel hash");
            return copy;
        } finally {
            parcel.recycle();
        }
    }

    public static void main(String[] args) {
        // The value-only slice also runs on a host JVM with real framework
        // compile classes. The normal Windows track exercises native Parcel.
        boolean host = args.length == 1 && "--host".equals(args[0]);
        if (!host) System.loadLibrary("android_runtime");

        ProxyInfo empty = new ProxyInfo((ProxyInfo) null);
        check(empty.getHost() == null && empty.getPort() == 0, "nullable copy is no proxy");
        check(Uri.EMPTY.equals(empty.getPacFileUrl()), "nullable copy has no PAC URI");
        check(empty.isValid(), "empty proxy snapshot is valid");
        check(empty.makeProxy() == Proxy.NO_PROXY, "empty proxy converts without a socket");
        check(empty.getSocketAddress() == null, "empty proxy has no socket address");
        check(empty.equals(new ProxyInfo(empty)), "empty proxy can be copied again");
        check(empty.hashCode() == new ProxyInfo(empty).hashCode(), "empty copy hash");
        check(empty.toString() != null, "empty snapshot can be logged");

        Uri uri = Uri.parse("https://example.invalid/proxy.pac");
        ProxyInfo pending = ProxyInfo.buildPacProxy(uri);
        check(pending.isValid() && pending.getPort() == -1, "PAC may await a local port");
        check(pending.makeProxy() == Proxy.NO_PROXY, "pending PAC does not throw");
        check(pending.getSocketAddress() == null, "pending PAC has no socket address");
        check(new ProxyInfo(pending).makeProxy() == Proxy.NO_PROXY, "pending PAC copy");
        check(ProxyInfo.buildPacProxy(uri, 65536).makeProxy() == Proxy.NO_PROXY,
                "unusable PAC port is a nonthrowing conversion");
        rejectsNull(() -> ProxyInfo.buildPacProxy(null));
        rejectsNull(() -> ProxyInfo.buildPacProxy(null, 8080));

        ProxyInfo ready = ProxyInfo.buildPacProxy(uri, 8080);
        Proxy readyProxy = ready.makeProxy();
        check(readyProxy.type() == Proxy.Type.HTTP, "ready PAC uses HTTP proxy");
        check(((InetSocketAddress) readyProxy.address()).getPort() == 8080, "ready PAC port");
        ProxyInfo direct = ProxyInfo.buildDirectProxy("127.0.0.1", 8888, List.of("localhost"));
        check(direct.isValid() && direct.makeProxy().type() == Proxy.Type.HTTP, "direct proxy");
        ProxyInfo copy = new ProxyInfo(direct);
        String[] excluded = copy.getExclusionList();
        excluded[0] = "changed.invalid";
        check("localhost".equals(direct.getExclusionList()[0]), "copy retains source exclusions");
        check(copy.equals(direct) && copy.hashCode() == direct.hashCode(), "direct copy value");

        if (!host) {
            check(roundTrip(empty).makeProxy() == Proxy.NO_PROXY, "empty proxy parcel conversion");
            check(roundTrip(pending).makeProxy() == Proxy.NO_PROXY, "pending PAC parcel conversion");
            check(roundTrip(ready).makeProxy().type() == Proxy.Type.HTTP, "ready PAC parcel conversion");
            check(roundTrip(direct).makeProxy().type() == Proxy.Type.HTTP, "direct proxy parcel conversion");
        }
        System.out.println("ProxyInfoContractTest all passed: " + checks);
    }
}
