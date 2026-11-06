/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.*;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.content.res.CompatibilityInfo;
import android.os.*;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Synthetic controller scheduling with genuine SDK PendingResult acknowledgement semantics. */
public final class OrderedBroadcastTest {
    private static int checks;
    private static final String ACTION="gesher.test.ORDERED";
    private static void require(boolean v,String why){if(!v)throw new AssertionError(why);checks++;}
    private static Field field(Class<?> c,String n)throws Exception{Field f=c.getDeclaredField(n);f.setAccessible(true);return f;}
    private static Object get(Object o,String n)throws Exception{return field(o.getClass(),n).get(o);}
    private static void set(Object o,String n,Object v)throws Exception{field(o.getClass(),n).set(o,v);}
    private static Method method(Class<?> c,String n,Class<?>...p)throws Exception{Method m=c.getDeclaredMethod(n,p);m.setAccessible(true);return m;}
    private static Object make(Class<?> c,Class<?>[] p,Object...a)throws Exception{Constructor<?> k=c.getDeclaredConstructor(p);k.setAccessible(true);return k.newInstance(a);}
    private interface Work{void run()throws Exception;}
    private static void rejected(Work w,Class<?> expected)throws Exception{
        try{w.run();}catch(InvocationTargetException e){if(expected.isInstance(e.getCause())){checks++;return;}throw e;}
        catch(RuntimeException e){if(expected.isInstance(e)){checks++;return;}throw e;}
        throw new AssertionError("Expected rejection: "+expected.getSimpleName());
    }
    private static final class Receiver extends IIntentReceiver.Stub{
        int calls,code;String data;Bundle extras;boolean ordered,assumed;Runnable action;
        final CountDownLatch received=new CountDownLatch(1);
        @Override public void performReceive(Intent i,int c,String d,Bundle b,boolean o,boolean s,int user){
            calls++;code=c;data=d;extras=b;ordered=o;received.countDown();if(action!=null)action.run();
        }
    }
    private static final class ThreadStub extends IApplicationThread.Default{
        final Binder binder=new Binder();volatile int manifests,exits;Runnable manifest;
        @Override public IBinder asBinder(){return binder;}
        @Override public void scheduleRegisteredReceiver(IIntentReceiver r,Intent i,int c,String d,Bundle b,
                boolean ordered,boolean sticky,boolean assume,int user,int state,int uid,String pkg)throws RemoteException{
            if(r instanceof Receiver)((Receiver)r).assumed=assume;
            r.performReceive(i,c,d,b,ordered,sticky,user);
        }
        @Override public void scheduleReceiver(Intent i,ActivityInfo info,CompatibilityInfo compat,
                int c,String d,Bundle b,boolean ordered,boolean assume,int user,int state,int uid,String pkg){
            manifests++;if(manifest!=null)manifest.run();
        }
        @Override public void scheduleExit(){exits++;}
    }
    private static final class Harness{
        final Object tasks,packages,controller,app,apk;
        final IActivityManager am;final ThreadStub thread=new ThreadStub();
        final String pkg;final Class<?> atm,process,bc,apkType;
        final List<Object> processes;
        @SuppressWarnings("unchecked") Harness(Object code,long timeout)throws Exception{
            apk=code;apkType=code.getClass();pkg=(String)apkType.getField("packageName").get(code);
            Class<?> pm=Class.forName("com.gesher.server.PackageManagerService"),amType=Class.forName("com.gesher.server.ActivityManagerService");
            atm=Class.forName("com.gesher.server.ActivityTaskManagerService");bc=Class.forName("com.gesher.server.BroadcastController");
            Class<?> notifications=Class.forName("com.gesher.server.NotificationManagerService");
            packages=pm.getConstructor(Collection.class).newInstance(Collections.singletonList(code));
            tasks=make(atm,new Class<?>[]{pm},packages);
            am=(IActivityManager)make(amType,new Class<?>[]{atm,pm,notifications},tasks,packages,make(notifications,new Class<?>[0]));
            controller=make(bc,new Class<?>[]{atm,pm,long.class},tasks,packages,timeout);set(am,"mBroadcasts",controller);
            process=Class.forName(atm.getName()+"$ProcessRecord");processes=(List<Object>)get(tasks,"mProcesses");
            app=addProcess(Binder.getCallingPid(),thread,pkg);
            // A live source activity prevents automatic idle exit while assertions run.
            ActivityInfo info=(ActivityInfo)apkType.getMethod("launcherActivity",String.class).invoke(code,(Object)null);
            Class<?> record=Class.forName(atm.getName()+"$ActivityRecord");
            Object activity=make(record,new Class<?>[]{atm,ActivityInfo.class,Intent.class,int.class,int.class,String.class,record,String.class,int.class},
                    tasks,info,new Intent(),1,Binder.getCallingPid(),pkg,null,null,-1);
            set(activity,"app",app);((List<Object>)get(app,"activities")).add(activity);((List<Object>)get(tasks,"mActivities")).add(activity);
        }
        Object addProcess(int pid,ThreadStub t,String processName)throws Exception{
            Object a=make(process,new Class<?>[]{atm,int.class,IApplicationThread.class},tasks,pid,t);
            set(a,"packageName",pkg);set(a,"processName",processName);set(a,"ready",true);processes.add(a);return a;
        }
        Receiver receiver(int priority,Runnable action)throws Exception{return receiver(priority,action,app,thread);}
        Receiver receiver(int priority,Runnable action,Object owner,ThreadStub t)throws Exception{
            Receiver r=new Receiver();r.action=action;IntentFilter f=new IntentFilter(ACTION);f.setPriority(priority);
            method(bc,"registerReceiver",IApplicationThread.class,String.class,IIntentReceiver.class,IntentFilter.class,int.class,String.class,int.class)
                    .invoke(controller,t,pkg,r,f,(Integer)get(owner,"pid"),null,Context.RECEIVER_EXPORTED);return r;
        }
        void send(Receiver result)throws Exception{send(new Intent(ACTION).setPackage(pkg),result,7,"initial",bundle(1),null,null,null);}
        void send(Intent i,Receiver result,int code,String data,Bundle extras,String[] required,String[] excluded,String[] excludedPkgs)throws Exception{
            am.broadcastIntentWithFeature(thread,null,i,null,result,code,data,extras,required,excluded,excludedPkgs,
                    android.app.AppOpsManager.OP_NONE,null,true,false,0);
        }
        void finish(int code,String data,Bundle extras,boolean abort)throws Exception{am.finishReceiver(thread.binder,code,data,extras,abort,0);}
        void finish()throws Exception{finish(0,null,null,false);}
        int queued()throws Exception{synchronized(controller){return ((List<?>)get(controller,"mOrdered")).size();}}
        int active()throws Exception{synchronized(controller){return ((Map<?,?>)get(controller,"mOrderedActive")).size();}}
        void quiet()throws Exception{require(queued()==0 && active()==0,"Ordered state leaked after completion");
            require(((ScheduledThreadPoolExecutor)get(controller,"mTimeouts")).getQueue().isEmpty(),"Canceled receiver/deadline timers leaked");}
    }
    private static Bundle bundle(int n){Bundle b=new Bundle();b.putInt("value",n);return b;}
    private static Bundle oversized(){Bundle b=new Bundle();b.putByteArray("oversize",new byte[262145]);return b;}
    private static void finish(Harness h,int c,String d,Bundle b,boolean abort){try{h.finish(c,d,b,abort);}catch(Exception e){throw new AssertionError(e);}}

    public static void main(String[] args)throws Exception{
        require(args.length==1,"Pass synthetic devsvc APK");System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,System.getenv("ANDROID_ROOT")+"/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA",Files.createTempDirectory("ordered-policy-").toString(),true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF","1",true);
        Object apk=Class.forName("com.gesher.server.ApkPackage").getMethod("parse",String.class).invoke(null,args[0]);
        Harness h=new Harness(apk,60000);List<String> sequence=new ArrayList<>();
        Receiver last=new Receiver();last.action=()->sequence.add("final");
        Receiver low=h.receiver(10,()->{sequence.add("low");finish(h,303,"last",bundle(3),false);});
        Receiver high=h.receiver(100,null);high.action=()->{
            sequence.add("high");require(high.ordered && !high.assumed,"Ordered receiver was assumed delivered");
            require(high.code==7 && "initial".equals(high.data) && high.extras.getInt("value")==1,"Initial result was discarded");
        };
        h.send(last);require(sequence.equals(Arrays.asList("high")) && last.calls==0,"goAsync slot advanced before finish");
        require((Boolean)method(h.bc,"hasReceivers",h.process).invoke(h.controller,h.app),"Async receiver lost process keepalive");
        rejected(()->h.am.finishReceiver(new Binder(),2,null,null,false,0),SecurityException.class);
        rejected(()->h.finish(2,"x".repeat(65537),null,false),IllegalArgumentException.class);
        rejected(()->h.finish(2,null,oversized(),false),IllegalArgumentException.class);
        // Real PendingResult carries the shared application-thread token and explicit ACK.
        android.content.BroadcastReceiver.PendingResult pending=new android.content.BroadcastReceiver.PendingResult(
                high.code,high.data,high.extras,android.content.BroadcastReceiver.PendingResult.TYPE_REGISTERED,
                true,false,false,h.thread.binder,0,0,10000,h.pkg);
        pending.setResult(202,"middle",bundle(2));pending.sendFinished(h.am);
        require(low.code==202 && "middle".equals(low.data) && low.extras.getInt("value")==2,"Intermediate results were not propagated");
        require(sequence.equals(Arrays.asList("high","low","final")),"Priority/completion order changed");
        require(last.calls==1 && !last.ordered && last.assumed && last.code==303 && "last".equals(last.data)
                && last.extras.getInt("value")==3,"Final callback contract changed");h.quiet();
        h.finish();require(last.calls==1,"Duplicate finish replayed final callback");
        Harness abort=new Harness(apk,60000);AtomicInteger skipped=new AtomicInteger();Receiver end=new Receiver();
        abort.receiver(10,skipped::incrementAndGet);abort.receiver(100,()->finish(abort,9,"abort",bundle(9),true));
        abort.send(end);require(skipped.get()==0 && end.calls==1 && end.code==9,"Abort omitted final callback or reached later receiver");abort.quiet();
        Harness noAbort=new Harness(apk,60000);AtomicInteger visited=new AtomicInteger();
        noAbort.receiver(100,()->finish(noAbort,9,null,null,true));noAbort.receiver(10,()->{visited.incrementAndGet();finish(noAbort,10,null,null,false);});
        Receiver noAbortEnd=new Receiver();noAbort.send(new Intent(ACTION).setPackage(noAbort.pkg).addFlags(Intent.FLAG_RECEIVER_NO_ABORT),noAbortEnd,0,null,null,null,null,null);
        require(visited.get()==1 && noAbortEnd.code==10,"NO_ABORT did not preserve later delivery");noAbort.quiet();
        Harness empty=new Harness(apk,60000);Receiver emptyEnd=new Receiver();empty.send(emptyEnd);
        require(emptyEnd.calls==1 && emptyEnd.code==7 && emptyEnd.extras.getInt("value")==1,"Empty broadcast never finalized initial result");empty.quiet();
        Harness filtered=new Harness(apk,60000);Receiver denied=filtered.receiver(0,()->{throw new AssertionError("Excluded receiver ran");});
        Receiver filteredEnd=new Receiver();filtered.send(new Intent(ACTION).setPackage(filtered.pkg),filteredEnd,11,null,null,
                new String[]{"android.permission.MANAGE_USERS"},null,null);
        require(denied.calls==0 && filteredEnd.calls==1 && filteredEnd.code==11,"Receiver permission filtering lost final result");
        rejected(()->filtered.send(new Intent("com.google.android.c2dm.intent.RECEIVE").setPackage("other.package"),new Receiver(),0,null,null,null,null,null),SecurityException.class);
        rejected(()->filtered.am.broadcastIntentWithFeature(new ThreadStub(),null,new Intent(ACTION),null,new Receiver(),0,null,null,null,null,null,-1,null,true,false,0),SecurityException.class);
        filtered.quiet();
        Harness unregister=new Harness(apk,60000);Receiver active=unregister.receiver(0,null),unregisterEnd=new Receiver();
        unregister.send(unregisterEnd);unregister.am.unregisterReceiver(active);
        Receiver other=new Receiver();unregister.send(new Intent("gesher.test.EMPTY").setPackage(unregister.pkg),other,0,null,null,null,null,null);
        require(unregisterEnd.calls==0 && unregister.active()==1 && other.calls==1,"Unregister prematurely consumed an active async completion slot");
        unregister.finish();require(unregisterEnd.calls==1,"Unregistered active receiver could not finish");unregister.quiet();
        Harness removed=new Harness(apk,60000);removed.receiver(0,null);Receiver parentEnd=new Receiver();removed.send(parentEnd);
        Receiver child=new Receiver();String childAction=ACTION+".CHILD";
        removed.am.registerReceiverWithFeature(removed.thread,removed.pkg,null,null,child,new IntentFilter(childAction),null,0,Context.RECEIVER_EXPORTED);
        Receiver childEnd=new Receiver();removed.send(new Intent(childAction).setPackage(removed.pkg),childEnd,17,null,null,null,null,null);
        require(child.calls==0 && childEnd.calls==0,"Queued child overlapped the active parent ACK slot");
        removed.am.unregisterReceiver(child);
        require(childEnd.calls==1 && childEnd.code==17 && parentEnd.calls==0 && removed.active()==1,
                "Removing queued receiver blocked its final callback or consumed parent's async slot");
        removed.finish();require(parentEnd.calls==1,"Parent completion was lost after queued unregister");removed.quiet();
        Harness reentry=new Harness(apk,60000);AtomicInteger nested=new AtomicInteger();Receiver recursiveEnd=new Receiver();
        recursiveEnd.action=()->{if(nested.incrementAndGet()<1000)try{reentry.send(recursiveEnd);}catch(Exception e){throw new AssertionError(e);}};
        reentry.send(recursiveEnd);require(nested.get()==1000 && recursiveEnd.calls==1000,"Reentrant final callbacks lost/duplicated work");reentry.quiet();
        Harness quota=new Harness(apk,60000);quota.receiver(0,null);List<Receiver> finals=new ArrayList<>();
        for(int i=0;i<4;i++){Receiver r=new Receiver();finals.add(r);quota.send(r);}
        rejected(()->quota.send(new Receiver()),IllegalStateException.class);
        for(int i=0;i<4;i++)quota.finish();require(finals.stream().allMatch(r->r.calls==1),"Quota/drain lost final callbacks");quota.quiet();
        Harness mixed=new Harness(apk,60000);Receiver wait=mixed.receiver(0,null),mixedEnd=new Receiver();mixed.send(mixedEnd);
        method(mixed.bc,"broadcastIntent",Intent.class,String.class,String.class,int.class).invoke(mixed.controller,
                new Intent(ACTION).setComponent(new ComponentName(mixed.pkg,mixed.pkg+".AlarmReceiver")),null,null,Binder.getCallingPid());
        require(mixed.thread.manifests==0,"Unordered manifest overlapped ordered app-thread finish slot");
        mixed.finish();require(mixed.thread.manifests==1 && mixedEnd.calls==1,"Ordered finish did not release queued manifest");mixed.finish();mixed.quiet();
        Harness timeout=new Harness(apk,150);ThreadStub peer=new ThreadStub();Object peerApp=timeout.addProcess(Binder.getCallingPid()+100000,peer,timeout.pkg+":peer");
        timeout.receiver(0,null,peerApp,peer);Receiver timeoutEnd=new Receiver();timeout.send(timeoutEnd);
        require(timeoutEnd.received.await(5,TimeUnit.SECONDS),"Timed-out receiver prevented final callback");
        // processDied can pump the final callback before receiverTimedOut's finally
        // schedules the process exit. Await both postconditions, with real visibility.
        Method processForPid=method(timeout.atm,"callerProcess",int.class);
        int peerPid=Binder.getCallingPid()+100000;
        long drained=System.currentTimeMillis()+2000;
        while((timeout.queued()!=0 || peer.exits!=1 || processForPid.invoke(timeout.tasks,peerPid)!=null)
                && System.currentTimeMillis()<drained)Thread.sleep(5);
        require(timeoutEnd.code==7 && peer.exits==1 && processForPid.invoke(timeout.tasks,peerPid)==null
                && processForPid.invoke(timeout.tasks,Binder.getCallingPid())==timeout.app,
                "Timeout did not retire only the receiver generation");timeout.quiet();
        Harness dead=new Harness(apk,60000);ThreadStub deadPeer=new ThreadStub();Object deadApp=dead.addProcess(Binder.getCallingPid()+200000,deadPeer,dead.pkg+":peer");
        dead.receiver(0,null,deadApp,deadPeer);Receiver deadEnd=new Receiver();dead.send(deadEnd);
        method(dead.atm,"processDied",dead.process).invoke(dead.tasks,deadApp);
        require(deadEnd.calls==1 && deadEnd.code==7,"Receiver death omitted final callback");dead.quiet();
        System.out.println("OrderedBroadcastTest all passed: "+checks+" result, ACK, policy, reentry and lifecycle checks");
    }
}
