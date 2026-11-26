// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import java.io.*;
import java.lang.reflect.*;
import java.net.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** Isolated loader and explicit host policy boundaries; actual provider/HTTP
 * behavior is replayed completely after failed resolution becomes available. */
public final class Http2BindingsLoaderHost {
    private static Throwable cause(InvocationTargetException failure) { return failure.getCause(); }
    public static void main(String[] args) throws Exception {
        ClassLoader parent=Http2BindingsLoaderHost.class.getClassLoader();
        URL classes=Http2BindingsLoaderHost.class.getProtectionDomain().getCodeSource().getLocation();
        String missing="com.android.okhttp.okio.Source";
        AtomicBoolean unavailable=new AtomicBoolean(true);
        try(URLClassLoader child=new URLClassLoader(new URL[]{classes},parent) {
            @Override protected Class<?> loadClass(String name,boolean resolve) throws ClassNotFoundException {
                synchronized(getClassLoadingLock(name)) {
                    if(name.equals(missing) && unavailable.get()) throw new ClassNotFoundException(missing);
                    if(!name.startsWith("android.net.http.") && !name.equals("Http2BindingsTest")
                            && !name.startsWith("Http2BindingsTest$")) return super.loadClass(name,resolve);
                    Class<?> loaded=findLoadedClass(name);
                    if(loaded==null) loaded=findClass(name);
                    if(resolve) resolveClass(loaded);
                    return loaded;
                }
            }
        }) {
            Class<?> transport=Class.forName("android.net.http.Http2Transport",true,child);
            Constructor<?> create=transport.getDeclaredConstructor(); create.setAccessible(true);
            for(int i=0;i<2;i++) {
                try { create.newInstance(); throw new AssertionError("Missing provider was silently accepted"); }
                catch(InvocationTargetException failure) {
                    Throwable actual=cause(failure);
                    if(!(actual instanceof ClassNotFoundException) || !missing.equals(actual.getMessage()))
                        throw new AssertionError("Failed provider resolution poisoned later retries",actual);
                }
            }
            unavailable.set(false);
            try(Closeable recovered=(Closeable)create.newInstance()) {
                if(transport.getClassLoader()!=child) throw new AssertionError("Loader boundary not exercised");
            }
            // Metadata reuse must never freeze the current policy instance or
            // its answer. This setter exists only in the declared host stub.
            Class<?> policy=Class.forName("android.security.NetworkSecurityPolicy",true,parent);
            Method permitted=policy.getMethod("setCleartextTrafficPermitted",boolean.class);
            Class<?> route=Class.forName("android.net.http.Http2SessionPool$Route",true,child);
            Constructor<?> routeConstructor=route.getDeclaredConstructor(URL.class,Class.forName("android.net.Network"));
            routeConstructor.setAccessible(true);
            URL url=new URL("http://127.0.0.1:9/policy");
            try {
                for(int i=0;i<3;i++) {
                    permitted.invoke(null,false);
                    try { routeConstructor.newInstance(url,null); throw new AssertionError("Warm binding cached a cleartext allowance"); }
                    catch(InvocationTargetException failure) {
                        if(!(cause(failure) instanceof IOException)
                                || !cause(failure).getMessage().contains("Cleartext traffic is not permitted"))
                            throw new AssertionError("Policy denial changed",cause(failure));
                    }
                    permitted.invoke(null,true); routeConstructor.newInstance(url,null);
                }
            } finally { permitted.invoke(null,true); }
            Class<?> fixture=Class.forName("Http2BindingsTest",true,child);
            try { fixture.getMethod("main",String[].class).invoke(null,(Object)new String[0]); }
            catch(InvocationTargetException failure) {
                if(cause(failure) instanceof Error) throw (Error)cause(failure);
                if(cause(failure) instanceof Exception) throw (Exception)cause(failure);
                throw new AssertionError(cause(failure));
            }
            System.out.println("HTTP2_BINDINGS_LOADER_PASS: retries, live policy and complete isolated wire corpus");
        }
    }
}
