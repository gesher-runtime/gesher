/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

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
 * An in-process stand-in for a system service's AIDL interface.
 *
 * <p>The implementation is a subclass of the interface's generated {@code IFoo.Default}
 * that overrides the methods the headless lifecycle calls, so javac checks each one
 * against the interface. A {@link Proxy} over the interface dispatches to it; a call to a
 * method the subclass does not override is logged once and gets {@code Default}'s answer
 * (null, 0, false), or throws {@link UnsupportedOperationException} in strict mode
 * ({@code --strict-services}). That log is the list of what to implement next.
 *
 * <p>The proxy is attached to a local {@link Binder} under the interface's descriptor, so
 * {@code IFoo.Stub.asInterface(ServiceManager.getService(name))} returns it directly, with
 * no Parcel round trip: Binder's in-process path, as on a device for a service in the
 * caller's own process.
 */
public final class StandIn<T extends IInterface> implements InvocationHandler {
    static volatile boolean sStrict;

    private final Class<T> mInterface;
    private final Class<?> mDefault;
    private final Object mImpl;
    private final T mProxy;
    private final Binder mBinder;
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
        mBinder = new Binder();
        mBinder.attachInterface(mProxy, descriptor(iface));
    }

    /** Wraps {@code impl}, an {@code iface.Default} subclass, as a service of type iface. */
    public static <T extends IInterface> StandIn<T> of(Class<T> iface, T impl) {
        return new StandIn<>(iface, impl);
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
                throw new UnsupportedOperationException("gesher-app stand-in "
                        + mInterface.getSimpleName() + "." + name + " is not implemented");
            }
            if (first) {
                System.err.println("gesher-app: stand-in " + mInterface.getSimpleName() + "."
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
