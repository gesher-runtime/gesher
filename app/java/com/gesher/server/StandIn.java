/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.HashSet;
import java.util.Set;

/**
 * A stand-in for a system service's AIDL interface, served in-process or across processes.
 *
 * <p>The implementation is a subclass of the interface's generated {@code IFoo.Default}
 * that overrides the methods the headless lifecycle calls, so javac checks each one
 * against the interface. A {@link Proxy} over the interface dispatches to it; a call to a
 * method the subclass does not override is logged once and gets {@code Default}'s answer
 * (null, 0, false), or throws {@link UnsupportedOperationException} in strict mode
 * ({@code --strict-services}). That log is the list of what to implement next.
 *
 * <p>The service's Binder is {@code com.gesher.server.stub.IFooStub}, a concrete
 * {@code IFoo.Stub} that app/gen/StubGen.java writes at build time, delegating every
 * method to the proxy: the AIDL-generated {@code onTransact} unmarshals calls from other
 * processes, and {@code IFoo.Stub.asInterface} in the same process returns the stub
 * itself, with no Parcel round trip. For an interface without a generated stub the
 * Binder is a plain {@link Binder} with the proxy attached as its local interface, which
 * serves the same process only.
 */
public final class StandIn<T extends IInterface> implements InvocationHandler {
    /** Package of the stubs app/gen/StubGen.java generates. */
    static final String STUB_PACKAGE = "com.gesher.server.stub.";

    private static volatile boolean sStrict;
    private static volatile String sTag = "gesher-app";

    private final Class<T> mInterface;
    private final Class<?> mDefault;
    private final Object mImpl;
    private final T mProxy;
    private final IBinder mBinder;
    private final Set<String> mReported = new HashSet<>();

    private StandIn(Class<T> iface, Object impl) {
        mInterface = iface;
        mImpl = impl;
        Class<?> d = impl.getClass();
        while (d != null && !d.getName().equals(iface.getName() + "$Default")) {
            d = d.getSuperclass();
        }
        mDefault = d;
        mProxy = iface.cast(Proxy.newProxyInstance(iface.getClassLoader(),
                new Class<?>[] {iface}, this));
        IBinder stub = stub(iface, mProxy);
        if (stub == null) {
            Binder b = new Binder();
            b.attachInterface(mProxy, descriptor(iface));
            stub = b;
        }
        mBinder = stub;
    }

    /** Wraps {@code impl}, an {@code iface.Default} subclass, as a service of type iface. */
    public static <T extends IInterface> StandIn<T> of(Class<T> iface, T impl) {
        return new StandIn<>(iface, impl);
    }

    /** Methods the implementation does not override throw instead of answering a default. */
    public static void setStrict(boolean strict) {
        sStrict = strict;
    }

    /** The prefix of the "not implemented" log lines: the process's name. */
    public static void setTag(String tag) {
        sTag = tag;
    }

    /** The generated {@code IFoo.Stub} delegating to {@code impl}, or null if there is none. */
    private static IBinder stub(Class<?> iface, Object impl) {
        Class<?> stub;
        try {
            stub = Class.forName(STUB_PACKAGE + iface.getSimpleName() + "Stub", true,
                    StandIn.class.getClassLoader());
        } catch (ClassNotFoundException e) {
            return null;
        }
        try {
            return (IBinder) stub.getConstructor(iface).newInstance(impl);
        } catch (InvocationTargetException e) {
            throw new IllegalStateException("creating " + stub.getName(), e.getCause());
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("creating " + stub.getName(), e);
        }
    }

    /** The interface's AIDL descriptor, {@code IFoo.Stub.DESCRIPTOR}. */
    static String descriptor(Class<?> iface) {
        try {
            Class<?> stub = Class.forName(iface.getName() + "$Stub", false,
                    iface.getClassLoader());
            return (String) stub.getField("DESCRIPTOR").get(null);
        } catch (ReflectiveOperationException e) {
            throw new IllegalArgumentException("no AIDL descriptor for " + iface.getName(), e);
        }
    }

    public T asInterface() {
        return mProxy;
    }

    /** The service's Binder, to register or to return from another service's method. */
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        String name = method.getName();
        if (method.getDeclaringClass() == Object.class) {
            switch (name) {
                case "equals": return proxy == args[0];
                case "hashCode": return System.identityHashCode(proxy);
                default: return "StandIn{" + mInterface.getSimpleName() + "}";
            }
        }
        if (name.equals("asBinder") && method.getParameterCount() == 0) {
            return mBinder;
        }
        Method m = mImpl.getClass().getMethod(name, method.getParameterTypes());
        if (m.getDeclaringClass() == mDefault || m.getDeclaringClass() == mInterface) {
            boolean first;
            synchronized (mReported) {
                first = mReported.add(name);
            }
            if (sStrict) {
                throw new UnsupportedOperationException("gesher stand-in "
                        + mInterface.getSimpleName() + "." + name + " is not implemented");
            }
            if (first) {
                System.err.println(sTag + ": stand-in " + mInterface.getSimpleName() + "."
                        + name + " not implemented; returning a default");
            }
        }
        try {
            return m.invoke(mImpl, args);
        } catch (InvocationTargetException e) {
            throw e.getCause();
        }
    }
}
