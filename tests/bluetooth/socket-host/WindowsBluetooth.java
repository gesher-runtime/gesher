/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Declared native boundary with a blocked first connect and an accept completion hook. */
public final class WindowsBluetooth {
    public static final AtomicInteger opened=new AtomicInteger();
    public static final CountDownLatch connecting=new CountDownLatch(1);
    public static final CountDownLatch closed=new CountDownLatch(1);
    public static volatile Runnable acceptHook;
    public static volatile Runnable closeHook;
    public static volatile int acceptedClosed;
    public static String hresult(int hr){return Integer.toHexString(hr);}
    public static String address(long address){return "00:11:22:33:44:55";}
    public static long nativeRfcommOpen(long address,String uuid,int channel,boolean secure,int[] hr){return opened.incrementAndGet();}
    public static int nativeRfcommConnect(long socket,int[] channel){
        if(socket!=1)return 0;
        connecting.countDown();
        try{if(!closed.await(5,TimeUnit.SECONDS))throw new AssertionError("connect not cancelled");}
        catch(InterruptedException e){throw new AssertionError(e);}
        return -1;
    }
    public static void nativeRfcommClose(long socket){
        if(socket==99)acceptedClosed++;
        closed.countDown();
        Runnable hook=closeHook;if(hook!=null)hook.run();
    }
    /** Scripted native read for connected sockets (default: a native error). */
    public interface Read{int read(long socket,byte[] b,int off,int len);}
    public static volatile Read readHook;
    public static final java.io.ByteArrayOutputStream written=new java.io.ByteArrayOutputStream();
    public static final java.util.Set<byte[]> writeBuffers=java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<>());
    public static int nativeRfcommRead(long socket,byte[] b,int off,int len){Read hook=readHook;return hook==null?-1:hook.read(socket,b,off,len);}
    public static int nativeRfcommWrite(long socket,byte[] b,int off,int len){synchronized(written){writeBuffers.add(b);written.write(b,off,len);}return len;}
    public static int nativeRfcommAvailable(long socket){return 0;}
    public static long nativeRfcommListen(String name,String uuid,boolean secure,int[] out){out[1]=3;return 7;}
    public static long nativeRfcommAccept(long server,int timeout,long[] out){
        acceptHook.run();out[1]=1;out[2]=3;return 99;
    }
    public static void nativeRfcommServerClose(long server){}
}
