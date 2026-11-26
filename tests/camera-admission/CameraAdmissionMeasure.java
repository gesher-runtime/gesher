/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;
import android.hardware.camera2.*;
import android.hardware.camera2.impl.*;
import java.util.concurrent.*;
import java.lang.reflect.*;

/** Fixed blocked-consumer workload over either revision's complete production owners. */
public final class CameraAdmissionMeasure {
    public static void main(String[] args)throws Exception {
        CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);
        java.util.concurrent.atomic.AtomicInteger terminal=new java.util.concurrent.atomic.AtomicInteger();
        ICameraDeviceCallbacks target=new ICameraDeviceCallbacks.Default(){
            @Override public void onResultReceived(CameraMetadataInfo m,CaptureResultExtras e,PhysicalCaptureResultInfo[] p){terminal.incrementAndGet();}
            @Override public void onDeviceError(int error,CaptureResultExtras e){if(error!=ERROR_CAMERA_BUFFER)terminal.incrementAndGet();}
            @Override public void onCaptureStarted(CaptureResultExtras extras,long timestamp){
                entered.countDown();
                try{if(!release.await(15,TimeUnit.SECONDS))throw new AssertionError("fixture release timeout");}
                catch(InterruptedException e){throw new AssertionError(e);}
            }
        };
        CameraDeviceUser c=new CameraDeviceUser(new CameraService(),"measure",new WindowsCamera.Device(),target);
        CaptureRequest request=new CaptureRequest();request.bytes=128;
        int id=c.submitRequest(request,true).getRequestId();
        if(!entered.await(5,TimeUnit.SECONDS))throw new AssertionError("no blocked callback");
        Field worker=CameraDeviceUser.class.getDeclaredField("mThread");worker.setAccessible(true);
        Thread producer=(Thread)worker.get(c);
        long stop=System.nanoTime()+TimeUnit.SECONDS.toNanos(10);
        int observed;
        while(true){
            synchronized(WindowsCamera.class){observed=WindowsCamera.reads;}
            if(observed>=10000||producer.getState()==Thread.State.WAITING)break;
            if(System.nanoTime()>stop)throw new AssertionError("producer neither bounded nor reached workload limit");
            Thread.yield();
        }
        c.cancelRequest(id);c.disconnect();
        synchronized(WindowsCamera.class){observed=WindowsCamera.reads;}
        System.out.println("{\"blocked_callback\":true,\"native_reads_while_blocked\":"+observed+
                ",\"capture_thread_stopped\":"+!producer.isAlive()+",\"observation_limit\":10000}");
        release.countDown();
        stop=System.nanoTime()+TimeUnit.SECONDS.toNanos(10);
        while(terminal.get()!=observed){
            if(System.nanoTime()>stop)throw new AssertionError("missing terminal callbacks: "+terminal.get()+"/"+observed);
            Thread.yield();
        }
        System.out.println("{\"terminal_callbacks_after_unblock\":"+terminal.get()+"}");
    }
}
