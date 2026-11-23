/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.hardware.ICameraService;
import android.hardware.camera2.*;
import android.hardware.camera2.impl.*;
import android.hardware.camera2.utils.SubmitInfo;
import android.os.*;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.BooleanSupplier;

/** Native capture and metadata are explicit adapters; both full production owners run. */
public final class CameraAdmissionHost {
    static int checks;
    static synchronized void check(boolean b,String m){checks++;if(!b)throw new AssertionError(m);}
    static Field field(Class<?> c,String n)throws Exception{Field f=c.getDeclaredField(n);f.setAccessible(true);return f;}
    static Object get(Object o,String n)throws Exception{
        Object lock;
        try{lock=field(o.getClass(),"mLock").get(o);}catch(NoSuchFieldException missing){return field(o.getClass(),n).get(o);}
        synchronized(lock){return field(o.getClass(),n).get(o);}
    }
    static long number(Object o,String n)throws Exception{return ((Number)get(o,n)).longValue();}
    static void set(Object o,String n,Object v)throws Exception{field(o.getClass(),n).set(o,v);}
    static void until(BooleanSupplier s,String why)throws Exception{
        long stop=System.nanoTime()+TimeUnit.SECONDS.toNanos(10);
        while(!s.getAsBoolean()){if(System.nanoTime()>stop)throw new AssertionError(why);Thread.sleep(1);}
        check(true,why);
    }
    static long frames(CameraDeviceUser c){try{return number(get(c,"mCallbackQueue"),"mFrames");}catch(Exception e){throw new RuntimeException(e);}}
    static int queues(){synchronized(CameraCallbacks.class){try{return field(CameraCallbacks.class,"sQueues").getInt(null);}catch(Exception e){throw new RuntimeException(e);}}}
    static int tasks(CameraDeviceUser c)throws Exception{Object q=get(c,"mCallbackQueue");synchronized(get(q,"mLock")){return ((ArrayDeque<?>)get(q,"mQueue")).size();}}
    static final class Target extends ICameraDeviceCallbacks.Default {
        final CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);
        final List<String> events=Collections.synchronizedList(new ArrayList<>());
        final AtomicInteger terminals=new AtomicInteger(),repeats=new AtomicInteger();
        boolean block,fail;Runnable reenter;
        void callback(String event)throws RemoteException{
            events.add(event);
            if(entered.getCount()!=0){entered.countDown();
                if(block)try{check(release.await(10,TimeUnit.SECONDS),"target released");}catch(InterruptedException e){throw new RuntimeException(e);}
                if(reenter!=null)reenter.run();
                if(fail)throw new RemoteException();
            }
        }
        @Override public void onCaptureStarted(CaptureResultExtras e,long t)throws RemoteException{callback("start:"+e.frame+":"+e.subsequence);}
        @Override public void onResultReceived(CameraMetadataInfo m,CaptureResultExtras e,PhysicalCaptureResultInfo[] p)throws RemoteException{try{callback("result:"+e.frame);}finally{terminals.incrementAndGet();}}
        @Override public void onDeviceError(int error,CaptureResultExtras e)throws RemoteException{try{callback("error:"+e.frame+":"+error);}finally{if(e.frame>=0&&error!=ERROR_CAMERA_BUFFER)terminals.incrementAndGet();}}
        @Override public void onPrepared(int stream)throws RemoteException{callback("prepared:"+stream);}
        @Override public void onDeviceIdle()throws RemoteException{callback("idle");}
        @Override public void onRepeatingRequestError(long f,int id)throws RemoteException{repeats.incrementAndGet();callback("repeat-error:"+id);}
    }
    static CameraDeviceUser camera(Target t,boolean run)throws Exception{
        CameraDeviceUser c=new CameraDeviceUser(new CameraService(),"host",new WindowsCamera.Device(),t);
        if(!run)set(c,"mThread",new Thread()); // explicit producer scheduling seam; no native work
        return c;
    }
    static CaptureRequest request(long bytes){CaptureRequest r=new CaptureRequest();r.bytes=bytes;return r;}
    static void expectFull(Runnable r){try{r.run();throw new AssertionError("expected capacity rejection");}catch(ServiceSpecificException e){check(e.errorCode==ICameraService.ERROR_INVALID_OPERATION,"Android submission error");}}
    static void clean()throws Exception{until(()->queues()==0,"all callback owners retire");}

    static void admissionAndFlush()throws Exception{
        Target t=new Target();t.block=true;CameraDeviceUser c=camera(t,false);
        CaptureRequest[] burst=new CaptureRequest[CameraCallbacks.MAX_FRAMES];Arrays.fill(burst,request(128));
        SubmitInfo accepted=c.submitRequestList(burst,false);
        check(accepted.getLastFrameNumber()==63,"complete burst assigned exactly once");
        long next=number(c,"mNextFrameNumber"),id=number(c,"mNextRequestId");int copies=CameraMetadataNative.copies;
        for(int i=0;i<1024;i++)expectFull(()->c.submitRequest(request(128),false));
        check(CameraMetadataNative.copies==copies,"saturated request count rejects before metadata copies");
        check(number(c,"mNextFrameNumber")==next&&number(c,"mNextRequestId")==id,"rejected submissions consume no identities");
        c.flush();check(t.entered.await(5,TimeUnit.SECONDS),"flush reaches real callback worker");
        check(frames(c)==64,"blocked callback and queued terminal errors retain every credit");
        check(tasks(c)<=64,"bounded callback tasks");
        c.disconnect();check(queues()==1,"closed blocked consumer remains accounted");
        t.release.countDown();until(()->t.terminals.get()==64,"every admitted request receives terminal error");clean();
        for(int i=0;i<64;i++)check(t.events.get(i).startsWith("error:"+i+":"),"flush preserves queued frame order");
    }

    static void bytesAndAtomicBurst()throws Exception{
        Target t=new Target();CameraDeviceUser c=camera(t,false);int accepted=0;
        while(true){try{c.submitRequest(request(CameraCallbacks.MAX_METADATA_BYTES),false);accepted++;}catch(ServiceSpecificException full){break;}}
        check(accepted>0&&accepted<64,"metadata budget bounds frames before count ceiling");
        check(number(get(c,"mCallbackQueue"),"mBytes")<=CameraCallbacks.MAX_BYTES,"retained bytes bounded");
        long ids=number(c,"mNextRequestId"),frames=number(c,"mNextFrameNumber");
        expectFull(()->c.submitRequest(request(CameraCallbacks.MAX_METADATA_BYTES+1),false));
        check(number(c,"mNextRequestId")==ids&&number(c,"mNextFrameNumber")==frames,"oversized metadata not admitted");
        CaptureRequest invalid=request(128);invalid.streams=new int[65];expectFull(()->c.submitRequest(invalid,false));
        c.disconnect();final int expected=accepted;until(()->t.terminals.get()==expected,"byte-budget requests complete on close");clean();
        System.out.println("metadata admission: "+accepted+" maximum-size requests; budget="+CameraCallbacks.MAX_BYTES);
    }

    static void repeatingBackpressure()throws Exception{
        Target t=new Target();t.block=true;CameraDeviceUser c=camera(t,true);WindowsCamera.reads=0;
        SubmitInfo info=c.submitRequestList(new CaptureRequest[]{request(128),request(128)},true);
        check(t.entered.await(5,TimeUnit.SECONDS),"repeating callback blocked");
        until(()->frames(c)==64,"repeating producer reaches bounded in-flight capacity");
        Thread producer=(Thread)get(c,"mThread");until(()->producer.getState()==Thread.State.WAITING,"producer parks on notification");
        long number=number(c,"mNextFrameNumber");check(number==64,"repeat generation stops before next frame/metadata");
        check(WindowsCamera.reads==64,"one native capture per admitted repeat frame");
        check(c.cancelRequest(info.getRequestId())==63,"cancel reports last admitted frame");
        c.flush();c.disconnect();check(!producer.isAlive(),"disconnect wakes parked producer without callback join");
        t.release.countDown();until(()->t.terminals.get()==64,"repeat admitted results retained");clean();
        for(int i=0;i<64;i++)check(t.events.contains("start:"+i+":"+(i%2)),"repeating burst subsequences preserved");
        System.out.println("blocked repeat: "+number+" reads/frames; no additional capture while capacity is full");
    }

    static void controlsAndFailure()throws Exception{
        Target t=new Target();t.block=true;CameraDeviceUser c=camera(t,false);
        for(int i=0;i<CameraCallbacks.MAX_CONTROLS;i++)c.prepare(i);
        check(t.entered.await(5,TimeUnit.SECONDS),"control callback started");
        expectFull(()->c.prepare(999));c.evicted();check(queues()==1,"terminal device event fits saturated controls");
        t.release.countDown();clean();check(t.events.size()==17,"all accepted controls and disconnect delivered");
        t=new Target();t.fail=true;CameraDeviceUser d=camera(t,true);WindowsCamera.failRead=true;
        d.submitRequest(request(128),true);final Target failed=t;
        until(()->failed.terminals.get()==1&&failed.repeats.get()==1,"failed callback/read preserves terminal and repeating error");
        d.disconnect();WindowsCamera.failRead=false;clean();
    }

    static void reentrantAndReopen()throws Exception{
        Target t=new Target();CameraDeviceUser c=camera(t,true);Object lock=get(c,"mLock");AtomicReference<Throwable> error=new AtomicReference<>();
        t.reenter=()->{try{check(!Thread.holdsLock(lock),"application callback runs outside camera lock");c.submitRequest(request(128),false);c.flush();c.disconnect();}catch(Throwable e){error.set(e);}};
        c.submitRequest(request(128),false);until(()->t.entered.getCount()==0,"reentrant callback entered");clean();
        if(error.get()!=null)throw new AssertionError("reentrant lifecycle failed",error.get());
        List<Target> blocked=new ArrayList<>();
        for(int i=0;i<CameraCallbacks.MAX_QUEUES;i++){
            Target b=new Target();b.block=true;CameraDeviceUser old=camera(b,false);old.prepare(0);check(b.entered.await(5,TimeUnit.SECONDS),"old callback blocks");old.disconnect();blocked.add(b);
        }
        try{camera(new Target(),false);throw new AssertionError("reopen exceeded worker budget");}catch(ServiceSpecificException e){check(e.errorCode==ICameraService.ERROR_MAX_CAMERAS_IN_USE,"exhausted worker resources reported");}
        check(queues()==CameraCallbacks.MAX_QUEUES,"retired blocked workers remain bounded");
        for(Target b:blocked)b.release.countDown();clean();
        for(int i=0;i<1000;i++){CameraDeviceUser fresh=camera(new Target(),false);fresh.disconnect();}
        clean();System.out.println("reopen: "+CameraCallbacks.MAX_QUEUES+" blocked queues maximum; 1000 idle reopen/close cycles retire");
    }

    static void progressAndFailureRegressions()throws Exception{
        Target t=new Target();CameraDeviceUser c=camera(t,false);
        CaptureRequest[] oversized=new CaptureRequest[31];Arrays.fill(oversized,request(CameraCallbacks.MAX_METADATA_BYTES));
        expectFull(()->c.submitRequestList(oversized,true));
        check(number(c,"mNextRequestId")==0&&get(c,"mRepeating")==null,"impossible repeat reserves no identity/template");
        check(number(get(c,"mCallbackQueue"),"mBytes")==0,"impossible repeat retains no storage");
        SubmitInfo original=c.submitRequest(request(128),true);
        expectFull(()->c.submitRequestList(oversized,true));
        check(number(c,"mRepeatingId")==original.getRequestId(),"failed replacement preserves old repeat");
        c.disconnect();clean();

        Target progressing=new Target();CameraDeviceUser bounded=camera(progressing,true);
        CaptureRequest[] large=new CaptureRequest[28];Arrays.fill(large,request(CameraCallbacks.MAX_METADATA_BYTES));
        SubmitInfo accepted=bounded.submitRequestList(large,true);
        until(()->progressing.terminals.get()>=56,"each large repeating subsequence can progress for two rounds");
        bounded.cancelRequest(accepted.getRequestId());bounded.disconnect();clean();

        Target ordered=new Target();CameraDeviceUser flushing=camera(ordered,true);
        WindowsCamera.readEntered=new CountDownLatch(1);WindowsCamera.readRelease=new CountDownLatch(1);
        flushing.submitRequestList(new CaptureRequest[]{request(128),request(128),request(128)},false);
        check(WindowsCamera.readEntered.await(5,TimeUnit.SECONDS),"actual capture producer is held in native read seam");
        flushing.flush();until(()->ordered.terminals.get()==3,"flush delivers every active/queued terminal");
        for (int i = 0; i < 3; i++) {
            check(ordered.events.get(i).equals("error:" + i + ":" + ICameraDeviceCallbacks.ERROR_CAMERA_REQUEST),
                    "active flush before capture and queued frames report no image in order");
        }
        check(frames(flushing)==1,"flushed active native read remains charged until producer finishes");
        WindowsCamera.readRelease.countDown();WindowsCamera.readRelease=null;flushing.disconnect();clean();

        AtomicInteger afterError=new AtomicInteger();
        CameraCallbacks queue=new CameraCallbacks("assertion",new ICameraDeviceCallbacks.Default(){
            @Override public void onPrepared(int stream){if(stream==0)throw new AssertionError("application callback");afterError.incrementAndGet();}
        },()->{});
        queue.prepared(0);queue.prepared(1);queue.close();
        until(()->afterError.get()==1,"application Error does not strand subsequent callbacks");clean();
    }

    static void flushDuringImageDelivery() throws Exception {
        Target target = new Target();
        CameraDeviceUser camera = camera(target, true);
        CaptureRequest active = request(128);
        active.streams = new int[] {
                camera.createStream(new android.hardware.camera2.params.OutputConfiguration()) };
        CountDownLatch delivered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        CameraFrames.afterDelivery = () -> {
            delivered.countDown();
            try {
                check(release.await(5, TimeUnit.SECONDS), "finish output delivery after flush");
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new AssertionError(e);
            }
        };
        try {
            camera.submitRequestList(new CaptureRequest[] {active, request(128)}, false);
            check(delivered.await(5, TimeUnit.SECONDS), "frame is publishing buffer with metadata still pending");
            camera.flush();
            until(() -> target.terminals.get() == 2, "active and queued frames have terminal callbacks");
            check(target.events.contains("error:0:" + ICameraDeviceCallbacks.ERROR_CAMERA_RESULT),
                    "active frame with an image must report a result error");
            check(target.events.contains("error:1:" + ICameraDeviceCallbacks.ERROR_CAMERA_REQUEST),
                    "queued frame without an image must report a request error");
        } finally {
            release.countDown();
            CameraFrames.afterDelivery = null;
            camera.disconnect();
        }
        clean();
        check(target.terminals.get() == 2, "flush publishes no duplicate terminal result");
    }

    public static void main(String[] args)throws Exception{
        flushDuringImageDelivery();
        admissionAndFlush();bytesAndAtomicBurst();repeatingBackpressure();controlsAndFailure();reentrantAndReopen();progressAndFailureRegressions();
        System.out.println("Camera admission production checks passed: "+checks);
    }
}
