// Copyright (C) 2026 The Gesher Authors; All rights reserved.
package com.gesher.app.webview;
import java.util.*;
import org.json.*;

/** Production coordinator; only CDP transport, scheduling and app prompts are boundaries. */
public final class WebViewAuthBridgeTest {
 static int checks;
 static void check(boolean value,String reason){checks++;if(!value)throw new AssertionError(reason);}
 static JSONObject object(Object... values)throws Exception {JSONObject x=new JSONObject();for(int i=0;i<values.length;i+=2)x.put((String)values[i],values[i+1]);return x;}
 static final class Queue implements WebViewAuthBridge.Scheduler {
  ArrayDeque<Runnable> work=new ArrayDeque<>(); ArrayList<Runnable> deadlines=new ArrayList<>();
  public void post(Runnable r){work.add(r);} public void later(Runnable r,long delay){deadlines.add(r);}
  void pump(){int limit=1000;while(!work.isEmpty()){check(--limit>0,"Coordinator scheduling loop");work.remove().run();}}
 }
 static final class Call {
  final Port port;final String session,method;final JSONObject params;final long token;boolean taken;
  Call(Port p,String s,String m,String j,long t){port=p;session=s;method=m;token=t;try{params=new JSONObject(j);}catch(Exception e){throw new AssertionError(e);}}
  JSONObject dispatch()throws Exception {String expression=params.optString("expression");if(!expression.startsWith("globalThis.gesherAuthDispatch("))return null;return new JSONObject(expression.substring(expression.indexOf('(')+1,expression.lastIndexOf(')')));}
 }
 static final class Port implements WebViewAuthBridge.Transport {
  final Fixture f;final List<Boolean> ready=new ArrayList<>();final Map<String,String> nativeSessions=new HashMap<>();WebViewAuthBridge.Endpoint endpoint;
  final List<WebViewAuthBridge.Reply> replies=new ArrayList<>();
  Port(Fixture f){this.f=f;endpoint=new WebViewAuthBridge.Endpoint(this,f.queue,(url,host,realm,reply)->replies.add(reply));}
  public void command(String session,String method,String params,long token){f.calls.add(new Call(this,session,method,params,token));}
  public void session(String session,String target){f.sessions.add(session+":"+target);if(target.isEmpty())nativeSessions.remove(session);else nativeSessions.put(session,target);}
  public void ready(boolean value){ready.add(value);}
 }
 static final class Fixture implements AutoCloseable {
  final Queue queue=new Queue();final List<Call> calls=new ArrayList<>();final List<String> sessions=new ArrayList<>();
  final Port a=new Port(this),b=new Port(this);String session="session-1",binding,epoch,channel;int context=77;
  Call take(String method)throws Exception {for(Call c:calls)if(!c.taken&&c.token>0&&c.method.equals(method)){c.taken=true;return c;}throw new AssertionError("Missing CDP command "+method);}
  void answer(Call c,JSONObject result){c.port.endpoint.event(c.token,0,c.method,result.toString(),c.session);queue.pump();}
  void event(Port sender,String method,JSONObject event,String session){sender.endpoint.event(0,0,method,event.toString(),session);queue.pump();}
  void start()throws Exception {
   a.endpoint.installed(0,WebViewAuthBridge.EXTENSION);queue.pump();
   answer(take("Target.getTargetInfo"),object("targetInfo",object("targetId","target-A")));
   handshake();
   b.endpoint.installed(0,WebViewAuthBridge.EXTENSION);queue.pump();
   answer(take("Target.getTargetInfo"),object("targetInfo",object("targetId","target-B")));
   acknowledgeEvaluations();message("registered","target-B",null);check(b.ready.contains(true),"Second native target not registered");
  }
  void handshake()throws Exception {
   answer(take("Target.setDiscoverTargets"),object());
   answer(take("Target.getTargets"),object("targetInfos",new JSONArray().put(object("targetId","worker","type","service_worker","url",WebViewAuthBridge.WORKER))));
   answer(take("Target.attachToTarget"),object("sessionId",session));
   event(a,"Runtime.executionContextCreated",object("context",object("id",context,"origin","chrome-extension://"+WebViewAuthBridge.EXTENSION,"auxData",object("isDefault",true))),session);
   answer(take("Runtime.enable"),object());
   Call add=take("Runtime.addBinding");binding=add.params.getString("name");answer(add,object());
   Call hello=take("Runtime.evaluate");JSONObject command=hello.dispatch();check(command.getString("op").equals("hello"),"Worker handshake missing hello");
   epoch=command.getString("epoch");channel=command.optString("channel");answer(hello,object("result",object("type","boolean","value",true)));
   message("ready",null,null);acknowledgeEvaluations();message("registered","target-A",null);check(a.ready.contains(true),"Native target released before registration");
  }
  void acknowledgeEvaluations()throws Exception {
   for(Call c:new ArrayList<>(calls))if(!c.taken&&c.token>0&&c.method.equals("Runtime.evaluate")){c.taken=true;answer(c,object("result",object("type","boolean","value",true)));}
  }
  JSONObject auth(String target,String ticket)throws Exception {return object("version",1,"channel",channel,"epoch",epoch,"event","auth","targetId",target,"generation","0","ticket",ticket,"requestId","request-"+ticket,"url","https://same.invalid/file","host","same.invalid","realm","same","scheme","basic");}
  void binding(Port sender,JSONObject payload,String eventSession,int eventContext,String bindingName)throws Exception {
   event(sender,"Runtime.bindingCalled",object("name",bindingName,"executionContextId",eventContext,"payload",payload.toString()),eventSession);
  }
  void message(String kind,String target,String ticket)throws Exception {
   JSONObject value=object("version",1,"channel",channel,"epoch",epoch,"event",kind);if(target!=null)value.put("targetId",target);if(ticket!=null)value.put("ticket",ticket);
   binding(a,value,session,context,binding);
  }
  long responses()throws Exception {long n=0;for(Call c:calls){JSONObject d=c.dispatch();if(d!=null&&d.optString("op").equals("respond"))n++;}return n;}
  public void close(){a.endpoint.close();b.endpoint.close();queue.pump();}
 }
 static void identity()throws Exception {
  try(Fixture f=new Fixture()){f.start();JSONObject auth=f.auth("target-A","one");
   f.binding(f.b,auth,f.session,f.context,f.binding);f.binding(f.a,auth,"page-session",f.context,f.binding);
   f.binding(f.a,auth,f.session,f.context+1,f.binding);f.binding(f.a,auth,f.session,f.context,"pageBinding");
   JSONObject stale=new JSONObject(auth.toString()).put("epoch","stale");f.binding(f.a,stale,f.session,f.context,f.binding);
   if(!f.channel.isEmpty())f.binding(f.a,new JSONObject(auth.toString()).put("channel","foreign"),f.session,f.context,f.binding);
   check(f.a.replies.isEmpty()&&f.b.replies.isEmpty(),"Untrusted session/context/epoch/channel prompted the app");
   f.binding(f.a,auth,f.session,f.context,f.binding);f.binding(f.a,f.auth("target-B","two"),f.session,f.context,f.binding);
   check(f.a.replies.size()==1&&f.b.replies.size()==1,"Identical URLs lost exact target ownership");
   f.a.replies.get(0).answer(true,"alice","secret-A");f.b.replies.get(0).answer(false,"","");f.queue.pump();check(f.responses()==2,"Independent consent replies missing");
   f.a.replies.get(0).answer(true,"duplicate","duplicate");f.queue.pump();check(f.responses()==2,"Duplicate reply reused the ticket");
   f.binding(f.a,auth,f.session,f.context,f.binding);check(f.a.replies.size()==1,"Duplicate ticket delivered after reply");
  }
 }
 static void retirement()throws Exception {
  try(Fixture f=new Fixture()){f.start();f.binding(f.a,f.auth("target-A","one"),f.session,f.context,f.binding);
   WebViewAuthBridge.Reply old=f.a.replies.get(0);old.answer(true,"queued","queued");f.a.endpoint.stop();f.queue.pump();check(f.responses()==0,"Stop failed to retire a queued credential reply");
   old.answer(true,"late","late");f.queue.pump();check(f.responses()==0,"Late Stop reply resumed authentication");
   f.binding(f.a,f.auth("target-B","two"),f.session,f.context,f.binding);WebViewAuthBridge.Reply destroyed=f.b.replies.get(0);
   f.b.endpoint.close();destroyed.answer(true,"late","late");f.queue.pump();check(f.responses()==0,"Destroyed view resumed authentication");
  }
 }
 static void delayedStopEvent()throws Exception {
  try(Fixture f=new Fixture()){f.start();JSONObject emittedBeforeStop=f.auth("target-A","delayed");
   f.a.endpoint.stop();f.queue.pump();
   f.binding(f.a,emittedBeforeStop,f.session,f.context,f.binding);
   check(f.a.replies.isEmpty(),"Auth metadata queued before Stop presented a late consent prompt");
  }
 }
 static void restart()throws Exception {
  try(Fixture f=new Fixture()){f.start();f.binding(f.a,f.auth("target-A","old"),f.session,f.context,f.binding);
   WebViewAuthBridge.Reply reply=f.a.replies.get(0);String oldSession=f.session,oldEpoch=f.epoch;
   f.event(f.a,"Runtime.executionContextsCleared",object(),oldSession);f.session="session-2";f.context=88;f.handshake();
   reply.answer(true,"old","old");f.queue.pump();check(f.responses()==0,"Old worker context reply survived restart");
   JSONObject stale=f.auth("target-A","stale").put("epoch",oldEpoch);f.binding(f.a,stale,oldSession,77,f.binding);
   check(f.a.replies.size()==1,"Detached worker session created a new prompt");
  }
 }
 static void repeatedWorkerRestart()throws Exception {
  try(Fixture f=new Fixture()){f.start();
   for(int i=2;i<=11;i++){
    f.event(f.a,"Runtime.executionContextsCleared",object(),f.session);
    f.session="session-"+i;f.context=80+i;f.handshake();f.message("registered","target-B",null);
   }
   check(f.a.nativeSessions.size()==1&&f.a.nativeSessions.containsKey(f.session),
           "Worker restarts retained old native sessions beyond the eight-session transport bound");
  }
 }
 static void addingViewDuringRestart()throws Exception {
  try(Fixture f=new Fixture()){f.start();Port adding=new Port(f);
   try {
    adding.endpoint.installed(0,WebViewAuthBridge.EXTENSION);f.queue.pump();Call old=f.take("Target.getTargetInfo");
    f.event(f.a,"Runtime.executionContextsCleared",object(),f.session);
    f.answer(old,object("targetInfo",object("targetId","target-C")));
    f.session="session-recovered";f.context=91;f.handshake();
    check(adding.endpoint.target.equals("target-C"),"Outstanding view identity discovery was dropped during restart");
    f.acknowledgeEvaluations();f.message("registered","target-C",null);
    check(adding.ready.contains(true),"View waiting for identity stayed stuck after worker restart");
   } finally {adding.endpoint.close();f.queue.pump();}
  }
 }
 static void wakeAndWorkerOrigin()throws Exception {
  try(Fixture f=new Fixture()){
   f.a.endpoint.installed(0,WebViewAuthBridge.EXTENSION);f.queue.pump();
   f.answer(f.take("Target.getTargetInfo"),object("targetInfo",object("targetId","target-A")));
   f.answer(f.take("Target.setDiscoverTargets"),object());
   f.answer(f.take("Target.getTargets"),object("targetInfos",new JSONArray()));
   f.answer(f.take("ServiceWorker.enable"),object());
   f.answer(f.take("ServiceWorker.startWorker"),object());
   f.queue.deadlines.remove(1).run();f.queue.pump();
   f.answer(f.take("Target.getTargets"),object("targetInfos",new JSONArray().put(object("targetId","worker","type","service_worker","url",WebViewAuthBridge.WORKER))));
   f.answer(f.take("Target.attachToTarget"),object("sessionId",f.session));
   f.event(f.a,"Runtime.executionContextCreated",object("context",object("id",f.context,"origin",WebViewAuthBridge.WORKER)),f.session);
   f.answer(f.take("Runtime.enable"),object());
   Call add=f.take("Runtime.addBinding");f.binding=add.params.getString("name");f.answer(add,object());
   Call hello=f.take("Runtime.evaluate");JSONObject command=hello.dispatch();check(command.getString("op").equals("hello"),"Woken worker handshake missing hello");
   f.epoch=command.getString("epoch");f.channel=command.optString("channel");f.answer(hello,object("result",object("type","boolean","value",true)));
   f.message("ready",null,null);f.acknowledgeEvaluations();f.message("registered","target-A",null);
   check(f.a.ready.contains(true),"Woken worker with worker.js origin did not become ready");
  }
 }
 public static void main(String[] args)throws Exception {
  identity();retirement();restart();delayedStopEvent();repeatedWorkerRestart();addingViewDuringRestart();wakeAndWorkerOrigin();
  // A last view's owner can be destroyed and recreated without leaking the singleton.
  for(int i=0;i<3;i++)try(Fixture f=new Fixture()){f.start();}
  System.out.println("WebViewAuthBridgeTest passed: "+checks+" checks");
 }
}
