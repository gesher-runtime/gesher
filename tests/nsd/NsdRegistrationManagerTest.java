/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;
import android.content.Context;
import android.net.Network;
import android.net.nsd.*;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

/** Public manager contracts; explicit backend callbacks and handler/executor boundaries. */
public final class NsdRegistrationManagerTest {
    static int checks;
    static void check(boolean value,String reason){if(!value)throw new AssertionError(reason);checks++;}
    interface Action{void run()throws Exception;}
    static void rejects(Class<? extends Throwable> type,Action action,String reason)throws Exception{
        try{action.run();}catch(Throwable failure){check(type.isInstance(failure),reason+": "+failure);return;}
        throw new AssertionError(reason+": no error");
    }
    static final class Queue implements Executor {
        final ArrayDeque<Runnable> work=new ArrayDeque<>();
        public void execute(Runnable r){work.add(r);}
        void drain(){while(!work.isEmpty())work.remove().run();}
    }
    static final class Listener implements NsdManager.RegistrationListener,NsdManager.DiscoveryListener,NsdManager.ResolveListener{
        final List<String> events=new ArrayList<>();NsdServiceInfo last;
        public void onRegistrationFailed(NsdServiceInfo info,int error){last=info;events.add("failed:"+error);}
        public void onUnregistrationFailed(NsdServiceInfo info,int error){last=info;events.add("unregisterFailed:"+error);}
        public void onServiceRegistered(NsdServiceInfo info){last=info;events.add("registered");}
        public void onServiceUnregistered(NsdServiceInfo info){last=info;events.add("unregistered");}
        public void onStartDiscoveryFailed(String type,int error){events.add("discoverFailed:"+error);}
        public void onStopDiscoveryFailed(String type,int error){}public void onDiscoveryStarted(String type){}
        public void onDiscoveryStopped(String type){}public void onServiceFound(NsdServiceInfo info){}public void onServiceLost(NsdServiceInfo info){}
        public void onResolveFailed(NsdServiceInfo info,int error){}public void onServiceResolved(NsdServiceInfo info){}
    }
    static final class Factory implements NsdRegistrationClient.Factory {
        final List<Node> nodes=new ArrayList<>();int retained;Object managerLock;
        final class Node implements NsdRegistrationClient.Session{
            AdvertisingRequest request;final NsdRegistrationClient.Callback callback;int updates,stops;
            Node(AdvertisingRequest request,NsdRegistrationClient.Callback callback){this.request=request;this.callback=callback;}
            public void update(AdvertisingRequest request,int other){check(!Thread.holdsLock(managerLock),"update outside map lock");this.request=request;updates++;}
            public void stop(){check(!Thread.holdsLock(managerLock),"stop outside map lock");stops++;callback.unregistered();}
        }
        public NsdRegistrationClient.Session start(AdvertisingRequest request,int other,NsdRegistrationClient.Callback cb){
            check(!Thread.holdsLock(managerLock),"start outside map lock");Node n=new Node(request,cb);nodes.add(n);return n;
        }
        public int retainedCount(){return retained;}
    }
    static NsdManager manager(Queue handler,Factory factory)throws Exception{
        Constructor<NsdManager> ctor=NsdManager.class.getDeclaredConstructor(Context.class,Executor.class,
                WindowsNsd.Factory.class,WindowsNsdResolver.Factory.class);ctor.setAccessible(true);
        NsdManager manager=ctor.newInstance(null,handler,new WindowsNsd.Factory(){
            public WindowsNsd.Session start(String type,String subtype,WindowsNsd.Scope scope,WindowsNsd.Callback callback){throw new AssertionError("Unexpected browse transport");}
            public int[] interfaces(){throw new AssertionError("Unexpected interface selection");}
        },(WindowsNsdResolver.Factory)(request,callback)->{
            throw new AssertionError("Unexpected resolve transport");});
        Field lock=NsdManager.class.getDeclaredField("mDiscoveries");lock.setAccessible(true);factory.managerLock=lock.get(manager);
        try{Field f=NsdManager.class.getDeclaredField("mRegistrationBackend");f.setAccessible(true);f.set(manager,factory);}
        catch(NoSuchFieldException oldSource){/* Negative control runs the old public failure implementation. */}
        return manager;
    }
    static NsdServiceInfo info(String name,String type){NsdServiceInfo info=new NsdServiceInfo();info.setServiceName(name);info.setServiceType(type);info.setPort(34321);info.setAttribute("binary",new byte[]{0,(byte)255});return info;}
    static void positiveProbe()throws Exception{
        Queue handler=new Queue();Factory factory=new Factory();NsdManager manager=manager(handler,factory);Listener listener=new Listener();
        manager.registerService(info("OwnedProbe","_own._tcp"),1,Runnable::run,listener);
        if(!factory.nodes.isEmpty())factory.nodes.get(0).callback.registered(new NsdServiceInfo("OwnedProbe",null));
        handler.drain();check(listener.events.equals(List.of("registered")),"public registration succeeds through selected backend, observed="+listener.events);
        System.out.println("PUBLIC_REGISTRATION_POSITIVE_OK");
    }
    static void contracts()throws Exception{
        Queue handler=new Queue(),app=new Queue();Factory factory=new Factory();NsdManager manager=manager(handler,factory);Listener listener=new Listener();
        NsdServiceInfo original=info("OwnedIdentity","_own._tcp,_a");
        manager.registerService(original,1,app,listener);Factory.Node node=factory.nodes.get(0);
        original.getAttributes().get("binary")[0]=55;
        check(node.request.getServiceInfo().getAttributes().get("binary")[0]==0,"manager boundary owns raw TXT");
        rejects(IllegalArgumentException.class,()->manager.registerService(info("Else","_own._tcp"),1,listener),"duplicate registration identity");
        rejects(IllegalArgumentException.class,()->manager.discoverServices("_own._tcp",1,listener),"cross-kind discovery identity");
        rejects(IllegalArgumentException.class,()->manager.resolveService(info("Else","_own._tcp"),listener),"cross-kind resolve identity");
        NsdServiceInfo result=new NsdServiceInfo("Chosen",null);node.callback.registered(result);
        check(listener.events.isEmpty(),"handler then application executor separation");handler.drain();check(listener.events.isEmpty(),"application executor owns delivery");app.drain();
        check(listener.last==result && listener.events.equals(List.of("registered")),"backend result preserved");
        NsdServiceInfo updated=info("OwnedIdentity","_own._tcp,_b");manager.registerService(updated,1,Runnable::run,listener);
        check(node.updates==1 && (node.request.getFlags()&AdvertisingRequest.NSD_ADVERTISING_UPDATE_ONLY)!=0,"ordinary implicit subtype update");
        manager.unregisterService(listener);handler.drain();check(listener.last==updated && listener.events.get(1).equals("unregistered"),"latest original request and executor for terminal callback");
        rejects(IllegalArgumentException.class,()->manager.unregisterService(listener),"unknown terminal listener");
        AdvertisingRequest request=new AdvertisingRequest.Builder(info("Flags","_flags._tcp"),1)
                .setFlags(AdvertisingRequest.FLAG_SKIP_PROBING|AdvertisingRequest.FLAG_SKIP_SUBTYPE_ANNOUNCEMENTS)
                .setTtl(Duration.ofSeconds(60)).build();
        manager.registerService(request,Runnable::run,listener);node=factory.nodes.get(1);
        check(node.request.getFlags()==request.getFlags()&&node.request.getTtl().equals(request.getTtl()),"hidden options never stripped");
        node.callback.failed(6,true);factory.retained=200;handler.drain();
        Listener discovery=new Listener();manager.discoverServices("_own._tcp",1,discovery);handler.drain();
        check(discovery.events.equals(List.of("discoverFailed:4")),"discovery counts retained server transactions");
        factory.retained=0;
        Listener rejected=new Listener();manager.registerService(info("Reject","_own._tcp"),1,command->{throw new RejectedExecutionException("fixture");},rejected);
        Factory.Node rejectedNode=factory.nodes.get(2);rejectedNode.callback.registered(result);
        rejects(RejectedExecutionException.class,handler::drain,"rejected application executor");
        check(rejectedNode.stops==1,"executor rejection withdraws owner");handler.drain();
        manager.registerService(info("Reused","_own._tcp"),1,Runnable::run,rejected);
        check(factory.nodes.size()==4,"identity released before rejected delivery");
    }
    public static void main(String[] args)throws Exception{
        System.load(args[0]);try{positiveProbe();if(args.length<2||!args[1].equals("probe"))contracts();
            System.out.println("NSD_REGISTRATION_MANAGER_OK checks="+checks);
        }catch(Throwable failure){failure.printStackTrace(System.err);throw failure;}
    }
}
