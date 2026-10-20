/*
 * Gesher: compile-time stub of Conscrypt's android.net.ssl.SSLSockets.
 *
 * libcore's core-all is compiled against Conscrypt's intra-core API stubs
 * (conscrypt.module.intra.core.api.stubs in libcore/JavaLibrary.bp); okhttp
 * needs only this class from them. The stub is never packaged: at run time
 * the class comes from Conscrypt's own boot jar.
 */
package android.net.ssl;

import javax.net.ssl.SSLSocket;

public final class SSLSockets {
    private SSLSockets() {}

    public static boolean isSupportedSocket(SSLSocket socket) {
        throw new UnsupportedOperationException("compile-time stub");
    }

    public static void setUseSessionTickets(SSLSocket socket, boolean useSessionTickets) {
        throw new UnsupportedOperationException("compile-time stub");
    }
}
