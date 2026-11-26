// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import java.lang.reflect.InvocationTargetException;
import java.net.URL;
import java.net.URLClassLoader;

/** Reproduce ART's separate runtime-package access boundary on a host JVM. */
public final class HttpEngineBrotliLoaderHost {
    public static void main(String[] args) throws Exception {
        String fixture="android.net.http.HttpEngineBrotliTest";
        URL classes=HttpEngineBrotliLoaderHost.class.getProtectionDomain().getCodeSource().getLocation();
        ClassLoader parent=HttpEngineBrotliLoaderHost.class.getClassLoader();
        try(URLClassLoader child=new URLClassLoader(new URL[]{classes},parent) {
            @Override protected Class<?> loadClass(String name,boolean resolve) throws ClassNotFoundException {
                synchronized(getClassLoadingLock(name)) {
                    if(!name.equals(fixture) && !name.startsWith(fixture+"$")) return super.loadClass(name,resolve);
                    Class<?> loaded=findLoadedClass(name);
                    if(loaded==null) loaded=findClass(name);
                    if(resolve) resolveClass(loaded);
                    return loaded;
                }
            }
        }) {
            Class<?> test=Class.forName(fixture,true,child);
            Class<?> decoder=Class.forName("android.net.http.BrotliInputStream",true,parent);
            if(test.getClassLoader()!=child || decoder.getClassLoader()!=parent)
                throw new AssertionError("Fixture and production did not retain distinct defining loaders");
            try { test.getMethod("main",String[].class).invoke(null,(Object)new String[]{"--host"}); }
            catch(InvocationTargetException failure) {
                Throwable cause=failure.getCause();
                if(cause instanceof Error) throw (Error)cause;
                if(cause instanceof Exception) throw (Exception)cause;
                throw new AssertionError(cause);
            }
            System.out.println("BROTLI_SPLIT_LOADER_PASS: unchanged production, complete native/HTTP fixture");
        }
    }
}
