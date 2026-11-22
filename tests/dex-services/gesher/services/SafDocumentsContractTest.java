/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.*;
import android.app.servertransaction.*;
import android.content.*;
import android.content.pm.*;
import android.database.Cursor;
import android.net.Uri;
import android.os.*;
import android.permission.PermissionCheckerManager;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsProvider;
import com.android.internal.app.IAppOpsService;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedActivityImpl;
import com.gesher.server.*;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.BooleanSupplier;
import sun.misc.Unsafe;

/** Actual registry, AOSP transport, filesystem, grant store and ATM callbacks. */
public final class SafDocumentsContractTest {
    private static FixtureServiceContext.Scope serviceContexts;
    static final String CLIENT="org.gesher.fixture.saf.client", OTHER="org.gesher.fixture.saf.other";
    static final String AUTH=DocumentsStorageProvider.AUTHORITY;
    static final int UID=10601, OTHER_UID=10602;
    static int PID=911201, OTHER_PID=911202;
    static final int READ=Intent.FLAG_GRANT_READ_URI_PERMISSION, WRITE=Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    static final int PERSIST=Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION, PREFIX=Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
    static final Unsafe U;
    static int checks; static boolean nativeFiles;
    static final String RELAY="gesher.fixture.saf.relay.";
    static final Map<Integer,IBinder> relayControls=new HashMap<>();
    static final java.util.concurrent.atomic.AtomicInteger nativeIncoming=new java.util.concurrent.atomic.AtomicInteger();
    static final java.util.concurrent.atomic.AtomicInteger nativeOtherIncoming=new java.util.concurrent.atomic.AtomicInteger();
    static { try { Field f=Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); U=(Unsafe)f.get(null); }
        catch(Exception e){throw new ExceptionInInitializerError(e);} }
    interface Work { void run() throws Exception; }
    static void check(boolean value,String why){if(!value)throw new AssertionError(why); checks++;}
    static Field field(Class<?> c,String name)throws Exception {
        for(;c!=null;c=c.getSuperclass())try{Field f=c.getDeclaredField(name); f.setAccessible(true); return f;}
        catch(NoSuchFieldException ignored){} throw new NoSuchFieldException(name);
    }
    static Object get(Object o,String n)throws Exception{return field(o.getClass(),n).get(o);}
    static void set(Object o,String n,Object v)throws Exception{field(o.getClass(),n).set(o,v);}
    static Class<?> type(String n)throws Exception{return Class.forName("com.gesher.server."+n);}
    static Object raise(Throwable e)throws Exception{if(e instanceof Error t)throw t;if(e instanceof Exception t)throw t;throw new AssertionError(e);}
    static Object make(Class<?> c,Class<?>[] p,Object...a)throws Exception{
        Constructor<?> k=c.getDeclaredConstructor(p); k.setAccessible(true);
        try{return k.newInstance(a);}catch(InvocationTargetException e){return raise(e.getCause());}
    }
    static Object call(Object o,String n,Class<?>[] p,Object...a)throws Exception{
        Class<?> c=o instanceof Class?(Class<?>)o:o.getClass(); Method m=c.getDeclaredMethod(n,p);m.setAccessible(true);
        try{return m.invoke(o instanceof Class?null:o,a);}catch(InvocationTargetException e){return raise(e.getCause());}
    }
    static void rejects(Class<? extends Throwable> expected,Work action)throws Exception{
        try{action.run();}catch(Throwable e){if(!expected.isInstance(e))throw new AssertionError("Expected "+expected+", received "+e,e);checks++;return;}
        throw new AssertionError("Expected "+expected.getSimpleName());
    }
    static void as(int uid,int pid,Work work)throws Exception{
        long old=Binder.clearCallingIdentity();try{Binder.restoreCallingIdentity(((long)uid<<32)|pid);work.run();}
        finally{Binder.restoreCallingIdentity(old);}
    }

    static void relayReady(String ready)throws IOException{
        java.nio.file.Path path=new File(ready).toPath(),pending=new File(ready+".writing").toPath();
        Files.writeString(pending,android.os.Process.myUid()+" "+android.os.Process.myPid()+"\n");
        Files.move(pending,path);
    }

    /** Ordinary app UIDs cannot register ServiceManager names; the operator owns discovery. */
    static void registryMain(String ready)throws Exception{
        String root=System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("saf-binder-registry",root+"/build.prop",root+"/framework/framework-res.apk");
        if(android.os.Process.myUid()!=android.os.Process.SYSTEM_UID)throw new SecurityException("SAF registry operator UID");
        Map<Integer,IBinder> peers=new HashMap<>();
        Map<Integer,Integer> pids=new HashMap<>();
        ServiceManager.addService(RELAY+"registry",new Binder(){
            { attachInterface(null,RELAY+"registry"); }
            @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
                if(code==INTERFACE_TRANSACTION)return super.onTransact(code,data,reply,flags);
                if(!Binder.isDirectlyHandlingTransaction())throw new SecurityException("SAF registry requires incoming Binder transport");
                data.enforceInterface(RELAY);
                if(code==1){
                    int uid=Binder.getCallingUid(),pid=Binder.getCallingPid();
                    IBinder peer=data.readStrongBinder();
                    if((uid!=UID&&uid!=OTHER_UID)||peer==null||peer instanceof Binder||pid<=0||
                            !RELAY.equals(peer.getInterfaceDescriptor()))
                        throw new SecurityException("SAF registration requires a real ordinary peer");
                    synchronized(peers){
                        if(peers.containsKey(uid))throw new SecurityException("SAF peer already registered");
                        peers.put(uid,peer);pids.put(uid,pid);
                        peer.linkToDeath(()->{synchronized(peers){if(peers.get(uid)==peer){peers.remove(uid);pids.remove(uid);}}},0);
                    }
                    reply.writeNoException();reply.writeInt(uid);reply.writeInt(pid);return true;
                }
                if(code==2){
                    if(Binder.getCallingUid()!=android.os.Process.SYSTEM_UID)throw new SecurityException("SAF discovery requires operator");
                    int uid=data.readInt();
                    synchronized(peers){
                        IBinder peer=peers.get(uid);
                        if(peer==null)throw new IllegalStateException("SAF peer is not registered");
                        reply.writeNoException();reply.writeStrongBinder(peer);reply.writeInt(pids.get(uid));
                    }
                    return true;
                }
                return super.onTransact(code,data,reply,flags);
            }
        });
        SystemServer.startBinderThreads("saf-binder-registry",4);relayReady(ready);Binder.joinThreadPool();
    }

    /** A real separate process forwards to the provider through a BinderProxy. */
    static void relayMain(int expectedUid,String ready)throws Exception{
        String root=System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("saf-binder-relay",root+"/build.prop",root+"/framework/framework-res.apk");
        if((expectedUid!=UID&&expectedUid!=OTHER_UID)||android.os.Process.myUid()!=expectedUid)
            throw new SecurityException("SAF relay requires its ordinary fixture UID");
        IBinder control=new Binder(){
            { attachInterface(null,RELAY); }
            @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
                if(!Binder.isDirectlyHandlingTransaction()||Binder.getCallingUid()!=android.os.Process.SYSTEM_UID)
                    throw new SecurityException("SAF relay control requires the test operator");
                if(code==INTERFACE_TRANSACTION)return super.onTransact(code,data,reply,flags);
                data.enforceInterface(RELAY);
                if(code==1){
                    reply.writeNoException();reply.writeInt(android.os.Process.myUid());reply.writeInt(android.os.Process.myPid());return true;
                }
                if(code==2){
                    IBinder target=data.readStrongBinder();
                    if(target==null||target instanceof Binder)throw new SecurityException("Provider must reside in the other process");
                    IBinder forwarding=new Binder(){
                        @Override protected boolean onTransact(int transaction,Parcel request,Parcel response,int options)throws RemoteException{
                            if(Binder.getCallingUid()!=android.os.Process.SYSTEM_UID)
                                throw new SecurityException("SAF forwarding requires the test operator");
                            long identity=Binder.clearCallingIdentity();
                            try{return target.transact(transaction,request,response,options);}
                            finally{Binder.restoreCallingIdentity(identity);}
                        }
                    };
                    reply.writeNoException();reply.writeStrongBinder(forwarding);return true;
                }
                return super.onTransact(code,data,reply,flags);
            }
        };
        SystemServer.startBinderThreads("saf-binder-relay",4);
        IBinder registry=ServiceManager.getService(RELAY+"registry");
        if(registry==null||registry instanceof Binder)throw new IllegalStateException("SAF operator registry is unavailable");
        Parcel request=Parcel.obtain(),reply=Parcel.obtain();
        try{
            request.writeInterfaceToken(RELAY);request.writeStrongBinder(control);
            if(!registry.transact(1,request,reply,0))throw new IllegalStateException("SAF peer registration failed");
            reply.readException();
            if(reply.readInt()!=expectedUid||reply.readInt()!=android.os.Process.myPid())
                throw new SecurityException("SAF registry observed different native peer credentials");
        }finally{reply.recycle();request.recycle();}
        relayReady(ready);
        Binder.joinThreadPool();
    }

    static void connectRelays()throws Exception{
        check(android.os.Process.myUid()==android.os.Process.SYSTEM_UID,"SAF fixture operator has its real system identity");
        SystemServer.startBinderThreads("saf-provider-fixture",4);
        IBinder registry=ServiceManager.getService(RELAY+"registry");
        check(registry!=null&&!(registry instanceof Binder),"SAF discovery registry is cross-process");
        for(int uid:new int[]{UID,OTHER_UID}){
            IBinder relay;int registeredPid;
            Parcel find=Parcel.obtain(),found=Parcel.obtain();
            try{
                find.writeInterfaceToken(RELAY);find.writeInt(uid);
                check(registry.transact(2,find,found,0),"SAF peer discovery transaction");
                found.readException();relay=found.readStrongBinder();registeredPid=found.readInt();
            }finally{found.recycle();find.recycle();}
            check(relay!=null&&!(relay instanceof Binder),"SAF relay is an actual cross-process BinderProxy");
            check(RELAY.equals(relay.getInterfaceDescriptor()),"SAF relay control descriptor is authenticated");
            Parcel request=Parcel.obtain(),reply=Parcel.obtain();
            try{
                request.writeInterfaceToken(RELAY);
                check(relay.transact(1,request,reply,0),"SAF relay identity transaction");
                reply.readException();
                check(reply.readInt()==uid,"SAF peer native UID agrees with the declared fixture client");
                int pid=reply.readInt();
                check(pid>0&&pid==registeredPid&&pid!=android.os.Process.myPid(),"SAF peer has a distinct authenticated process ID");
                if(uid==UID)PID=pid;else OTHER_PID=pid;
                relayControls.put(uid,relay);
            }finally{reply.recycle();request.recycle();}
        }
        check(PID!=OTHER_PID,"SAF positive and negative callers use separate native processes");
    }

    static IContentProvider remoteTransport(IContentProvider local)throws Exception{
        IBinder target=local.asBinder();
        IBinder checked=new Binder(){
            @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
                int uid=Binder.getCallingUid(),pid=Binder.getCallingPid();
                if(!Binder.isDirectlyHandlingTransaction()||
                        !((uid==UID&&pid==PID)||(uid==OTHER_UID&&pid==OTHER_PID)))
                    throw new SecurityException("Provider requires actual incoming peer Binder credentials");
                nativeIncoming.incrementAndGet();
                if(uid==OTHER_UID)nativeOtherIncoming.incrementAndGet();
                return target.transact(code,data,reply,flags);
            }
        };
        Map<Integer,IContentProvider> peers=new HashMap<>();
        for(var entry:relayControls.entrySet()){
            Parcel request=Parcel.obtain(),reply=Parcel.obtain();
            try{
                request.writeInterfaceToken(RELAY);request.writeStrongBinder(checked);
                check(entry.getValue().transact(2,request,reply,0),"SAF provider is exported through the remote relay");
                reply.readException();IBinder forwarding=reply.readStrongBinder();
                check(forwarding!=null&&!(forwarding instanceof Binder),"SAF provider transport uses a real BinderProxy");
                peers.put(entry.getKey(),ContentProviderNative.asInterface(forwarding));
            }finally{reply.recycle();request.recycle();}
        }
        return (IContentProvider)Proxy.newProxyInstance(IContentProvider.class.getClassLoader(),
                new Class<?>[]{IContentProvider.class},(proxy,method,args)->{
                    IContentProvider peer=peers.get(Binder.getCallingUid());
                    if(peer==null)throw new SecurityException("No native SAF peer for the selected fixture caller");
                    // This selects a real process; the attributed UID in the original
                    // request stays untouched, including the forged-attribution control.
                    try{return method.invoke(peer,args);}catch(InvocationTargetException failure){throw failure.getCause();}
                });
    }
    @SuppressWarnings({"unchecked","rawtypes"}) static Object state(String n)throws Exception{return Enum.valueOf((Class)type("ActivityTaskManagerService$State"),n);}
    static ApkPackage code(String name,int uid)throws Exception{
        PackageImpl p=(PackageImpl)PackageImpl.forTesting(name,"/fixture/"+name+"/base.apk");p.setEnabled(true);p.setTargetSdkVersion(36);
        for(String n:List.of("Origin","Top")){ParsedActivityImpl a=new ParsedActivityImpl();a.setPackageName(name);a.setName(name+"."+n);a.setEnabled(true);p.addActivity(a);}
        p.hideAsFinal();ApkPackage r=(ApkPackage)U.allocateInstance(ApkPackage.class);
        set(r,"packageName",name);set(r,"parsed",p);set(r,"mUid",uid);set(r,"apkPath","/fixture/"+name+"/base.apk");
        set(r,"mDataDir","/fixture/"+name+"/data");set(r,"sharedLibraries",SharedLibraryGraph.EMPTY);set(r,"mEnabledOwner","saf-fixture-install:"+name);return r;
    }
    static final class AppThread extends IApplicationThread.Default{
        final IBinder binder=new Binder();final List<ClientTransactionItem> events=Collections.synchronizedList(new ArrayList<>());
        IActivityClientController controller;boolean acknowledge=true;
        @Override public IBinder asBinder(){return binder;}
        @Override public void scheduleTransaction(ClientTransaction tx){for(ClientTransactionItem item:tx.getTransactionItems()){
            events.add(item);if(!acknowledge)continue;
            try{as(UID,PID,()->{if(item instanceof PauseActivityItem p)controller.activityPaused(p.getActivityToken());
                else if(item instanceof ResumeActivityItem r)controller.activityResumed(r.getActivityToken(),false);});}
            catch(Exception e){throw new AssertionError(e);}
        }}
        @SuppressWarnings("unchecked") List<ResultInfo> results()throws Exception{
            List<ResultInfo> r=new ArrayList<>();synchronized(events){for(var e:events)if(e instanceof ActivityResultItem)r.addAll((List<ResultInfo>)get(e,"mResultInfoList"));}return r;
        }
    }
    /** Restore only the genuine service bindings installed by this isolated fixture. */
    private static final class FixtureBindings implements AutoCloseable {
        private final Field packageManager = field(ActivityThread.class, "sPackageManager");
        private final Object previousPackageManager = packageManager.get(null);
        private final Map<String, IBinder> cache;
        private final boolean hadMount;
        private final IBinder previousMount;

        @SuppressWarnings("unchecked") FixtureBindings() throws Exception {
            cache = (Map<String, IBinder>)field(ServiceManager.class, "sCache").get(null);
            hadMount = cache.containsKey("mount");
            previousMount = cache.get("mount");
        }

        @Override public void close() throws Exception {
            try { packageManager.set(null, previousPackageManager); }
            finally {
                if (hadMount) cache.put("mount", previousMount); else cache.remove("mount");
            }
        }
    }

    static final class Harness{
        final ApkPackage client=code(CLIENT,UID),other=code(OTHER,OTHER_UID);
        final PackageManagerService pm=new PackageManagerService(List.of(client,other,
                code("org.gesher.fixture.saf.server",android.os.Process.myUid())));
        final ActivityTaskManagerService atm;final ActivityManagerService am;final Object providers,process;
        final DocumentsStorageProvider provider;final UriGrants grants;final UriGrantsManagerService ugm;
        final AppThread thread=new AppThread();final TestContext context;final IContentProvider transport;
        final AttributionSource attribution=new AttributionSource(UID,CLIENT,null);
        @SuppressWarnings("unchecked") Harness(File stateFile)throws Exception{
            atm=(ActivityTaskManagerService)make(ActivityTaskManagerService.class,new Class<?>[]{PackageManagerService.class},pm);
            am=(ActivityManagerService)make(ActivityManagerService.class,new Class<?>[]{ActivityTaskManagerService.class,PackageManagerService.class,type("NotificationManagerService")},atm,pm,null);
            providers=get(am,"mProviders");provider=(DocumentsStorageProvider)get(providers,"mDocumentsStorage");grants=(UriGrants)get(providers,"mUriGrants");set(grants,"mStateFile",stateFile);
            Object[] holder=new Object[1];as(UID,PID,()->holder[0]=make(type("ActivityTaskManagerService$ProcessRecord"),new Class<?>[]{ActivityTaskManagerService.class,int.class,IApplicationThread.class},atm,PID,thread));
            process=holder[0];set(process,"packageName",CLIENT);set(process,"processName",CLIENT);set(process,"packageCode",client);set(process,"ready",true);
            ((List<Object>)get(atm,"mProcesses")).add(process);
            as(OTHER_UID,OTHER_PID,()->holder[0]=make(type("ActivityTaskManagerService$ProcessRecord"),new Class<?>[]{ActivityTaskManagerService.class,int.class,IApplicationThread.class},atm,OTHER_PID,new AppThread()));
            set(holder[0],"packageName",OTHER);set(holder[0],"processName",OTHER);set(holder[0],"packageCode",other);set(holder[0],"ready",true);((List<Object>)get(atm,"mProcesses")).add(holder[0]);
            thread.controller=(IActivityClientController)get(atm,"mClientImpl");
            IBinder mount=StandIn.of(android.os.storage.IStorageManager.class,new Services.StorageService()).asBinder();
            Object created=serviceContexts.runtime(pm,atm,am,Map.of("mount",mount));
            field(Services.class,"sLast").set(null,created);
            field(ActivityThread.class,"sPackageManager").set(null,pm);
            ((Map<String,IBinder>)field(ServiceManager.class,"sCache").get(null)).put("mount",mount);
            context=new TestContext(this);call(provider,"attachTo",new Class<?>[]{Context.class},context);
            IContentProvider local=((ContentProviderHolder)call(provider,"holder",new Class<?>[0])).provider;
            transport=nativeFiles?remoteTransport(local):local;
            ugm=new UriGrantsManagerService(pm,atm,am);
        }
        @SuppressWarnings("unchecked") Object activity(String name,String lifecycle)throws Exception{
            ActivityInfo i=new ActivityInfo();i.packageName=CLIENT;i.name=CLIENT+"."+name;i.applicationInfo=client.applicationInfo(0);i.taskAffinity=CLIENT;i.screenOrientation=ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
            Class<?> t=type("ActivityTaskManagerService$ActivityRecord");Object r=make(t,new Class<?>[]{ActivityTaskManagerService.class,ActivityInfo.class,Intent.class,int.class,int.class,String.class,t,String.class,int.class},atm,i,new Intent(),1,PID,CLIENT,null,null,-1);
            set(r,"app",process);set(r,"state",state(lifecycle));((List<Object>)get(atm,"mActivities")).add(r);((List<Object>)get(process,"activities")).add(r);
            List<Integer> order=(List<Integer>)get(atm,"mTaskOrder");if(!order.contains(1))order.add(1);return r;
        }
        void owned(IBinder owner,Uri uri,int modes)throws Exception{call(grants,"grantFromProvider",new Class<?>[]{type("ContentProviders$RegisteredProvider"),IBinder.class,String.class,Uri.class,int.class},get(provider,"registration"),owner,CLIENT,uri,modes);}
        void retire(IBinder owner)throws Exception{call(grants,"revokeByOwner",new Class<?>[]{IBinder.class},owner);}
        Cursor query(Uri uri,String[] projection)throws Exception{Cursor[] r=new Cursor[1];as(UID,PID,()->r[0]=transport.query(attribution,uri,projection,new Bundle(),null));return r[0];}
        Bundle operation(String method,Uri uri,String name)throws Exception{
            Bundle extras=new Bundle();extras.putParcelable(DocumentsContract.EXTRA_URI,uri);extras.putString(Document.COLUMN_DISPLAY_NAME,name);extras.putString(Document.COLUMN_MIME_TYPE,"text/plain");
            Bundle[] r=new Bundle[1];as(UID,PID,()->r[0]=transport.call(attribution,AUTH,method,null,extras));return r[0];
        }
        int start(Intent request,Object origin)throws Exception{return start(request,origin,41);}
        int start(Intent request,Object origin,int requestCode)throws Exception{
            int[] r=new int[1];as(UID,PID,()->r[0]=atm.startActivity(thread,CLIENT,null,request,request.getType(),
                    origin==null?null:(IBinder)get(origin,"token"),"fixture-result",requestCode,0,null,null));return r[0];
        }
        long idleSince()throws Exception{return (Long)call(atm,"idleSince",new Class<?>[0]);}
        void settled()throws Exception{long until=System.nanoTime()+TimeUnit.SECONDS.toNanos(10);while(System.nanoTime()<until){
            boolean pending;synchronized(atm){pending=!((Map<?,?>)get(atm,"mDocumentPickers")).isEmpty();}
            boolean worker=Thread.getAllStackTraces().keySet().stream().anyMatch(t->t.isAlive()&&t.getName().equals("document-picker"));
            if(!pending&&!worker)return;Thread.sleep(10);}throw new AssertionError("Document request did not retire");}
    }
    /** Service locator boundary: policy, AppOps and URI grant checks use production classes. */
    static final class TestContext extends ContextWrapper{
        final Harness h;final AppOpsManager appOps;final PermissionCheckerManager checker;final PackageManager packages;
        TestContext(Harness h)throws Exception{super(null);this.h=h;
            IAppOpsService service=h.pm.appOps();
            field(AppOpsManager.class,"sService").set(null,service);
            appOps=(AppOpsManager)make(AppOpsManager.class,new Class<?>[]{Context.class,IAppOpsService.class},this,service);
            checker=(PermissionCheckerManager)U.allocateInstance(PermissionCheckerManager.class);
            set(checker,"mService",make(type("AppOpsPermissionChecker"),new Class<?>[]{PackageManagerService.class},h.pm));
            set(checker,"mContext",this);
            packages=new ApplicationPackageManager(null,h.pm){@Override public List<ResolveInfo> queryIntentContentProviders(Intent i,int f){return h.pm.queryIntentContentProviders(i,null,f,0).getList();}};
        }
        @Override public String getPackageName(){return "android";}
        @Override public String getOpPackageName(){return "android";}
        @Override public String getAttributionTag(){return null;}
        @Override public AttributionSource getAttributionSource(){return new AttributionSource(1000,"android",null);}
        @Override public int getUserId(){return 0;}
        @Override public ApplicationInfo getApplicationInfo(){return new ApplicationInfo();}
        @Override public PackageManager getPackageManager(){return packages;}
        @Override public String getSystemServiceName(Class<?> c){return c==PermissionCheckerManager.class?Context.PERMISSION_CHECKER_SERVICE:c==AppOpsManager.class?Context.APP_OPS_SERVICE:null;}
        @Override public Object getSystemService(String n){return Context.APP_OPS_SERVICE.equals(n)?appOps:Context.PERMISSION_CHECKER_SERVICE.equals(n)?checker:null;}
        @Override public int checkPermission(String p,int pid,int uid){return h.pm.checkUidPermission(p,uid);}
    }
    static final class Dialog{
        final CountDownLatch opened=new CountDownLatch(1),finish=new CountDownLatch(1);
        String[] selected;Throwable failure;Object request;int ownerPid;
        boolean available=true,holdAfterRetirement;
        volatile BooleanSupplier active;
        interface Locality { boolean check(String windowsPath) throws IOException; }
        Locality locality = path -> { throw new AssertionError("Unexpected locality query: " + path); };
        final List<String> localityPaths = java.util.Collections.synchronizedList(new ArrayList<>());
        void install(Harness h)throws Exception{Class<?> backend=type("DocumentPicker$Backend");Object proxy=Proxy.newProxyInstance(backend.getClassLoader(),new Class<?>[]{backend},(o,m,a)->{
            if(m.getName().equals("available"))return available;
            // Explicit native locality boundary only. Production selection,
            // file creation, result publication and grants remain unmodified.
            if(m.getName().equals("isLocal")){localityPaths.add((String)a[0]);return locality.check((String)a[0]);}
            if(!m.getName().equals("pick"))throw new AssertionError(m);
            request=a[0];ownerPid=(int)a[1];active=(BooleanSupplier)a[2];opened.countDown();long until=System.nanoTime()+TimeUnit.SECONDS.toNanos(8);
            while(!finish.await(10,TimeUnit.MILLISECONDS)){
                if(!holdAfterRetirement&&!active.getAsBoolean())return null;
                if(System.nanoTime()>until)throw new AssertionError("Fixture dialog timed out");
            }
            if(!active.getAsBoolean())return null;if(failure!=null)throw failure;return selected;});set(h.atm,"mDocumentDialog",proxy);
        }
    }

    /** Scripted native choice input only; the actual IntentChooser/ATM owns its session. */
    static final class ChoiceDialog {
        final CountDownLatch opened = new CountDownLatch(1), finish = new CountDownLatch(1);
        volatile BooleanSupplier active;
        int selected = -1;
        void install(Harness h) throws Exception {
            Class<?> api = type("IntentChooser$Dialog");
            Object dialog = Proxy.newProxyInstance(api.getClassLoader(), new Class<?>[]{api}, (o, m, a) -> {
                if (m.getName().equals("available")) return true;
                if (!m.getName().equals("choose")) throw new AssertionError(m);
                active = (BooleanSupplier)a[4]; opened.countDown();
                if (!finish.await(8, TimeUnit.SECONDS)) throw new AssertionError("Choice fixture was not closed");
                return selected;
            });
            set(h.atm, "mIntentChooserDialog", dialog);
        }
    }
    static Uri document(File f)throws IOException{return DocumentsContract.buildDocumentUri(AUTH,f.getCanonicalPath());}
    static Uri tree(File f)throws IOException{return DocumentsContract.buildTreeDocumentUri(AUTH,f.getCanonicalPath());}
    static Intent open(){return new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("text/plain").addCategory(Intent.CATEGORY_OPENABLE);}
    static void provider(File dir)throws Exception{
        Harness h=new Harness(new File(dir,"provider.xml"));File data=Files.createDirectory(new File(dir,"documents").toPath()).toFile();
        File f=new File(data,"one.txt");Files.writeString(f.toPath(),"original");Uri doc=document(f),root=tree(data),parent=DocumentsContract.buildDocumentUriUsingTree(root,data.getCanonicalPath());
        check(DocumentsProvider.class.isAssignableFrom(h.provider.getClass()),"AOSP DocumentsProvider superclass");check(h.provider.getContext()!=null,"Attached before holder publication");
        ProviderInfo info=h.pm.resolveContentProvider(AUTH,0,0);
        check(info!=null&&info.exported&&info.grantUriPermissions&&info.applicationInfo.uid==1000,"Registered real provider identity");
        check(android.Manifest.permission.MANAGE_DOCUMENTS.equals(info.readPermission)&&info.readPermission.equals(info.writePermission),"MANAGE_DOCUMENTS protects both operations");
        try (Cursor roots = h.provider.queryRoots(new String[]{DocumentsContract.Root.COLUMN_FLAGS})) {
            check(roots.moveToFirst() && (roots.getInt(0) & DocumentsContract.Root.FLAG_LOCAL_ONLY) == 0,
                    "Aggregate Windows root does not claim that network mounts are local");
        }
        check(DocumentsContract.isDocumentUri(h.context,doc),"Framework provider-aware verification");
        check(!DocumentsContract.isDocumentUri(h.context,DocumentsContract.buildDocumentUri("not.registered",f.getPath())),"Unregistered authority is not a document provider");
        rejects(SecurityException.class,()->h.query(doc,null));rejects(SecurityException.class,()->h.operation(DocumentsContract.METHOD_CREATE_DOCUMENT,parent,"denied.txt"));
        IBinder owner=new Binder();h.owned(owner,doc,READ);
        try(Cursor c=h.query(doc,new String[]{Document.COLUMN_DISPLAY_NAME,Document.COLUMN_MIME_TYPE,Document.COLUMN_SIZE})){check(c.moveToFirst()&&c.getCount()==1&&"one.txt".equals(c.getString(0)),"Transport returns real file row");check("text/plain".equals(c.getString(1))&&c.getLong(2)==8,"Real MIME and size");}
        rejects(SecurityException.class,()->h.operation(DocumentsContract.METHOD_RENAME_DOCUMENT,doc,"denied.txt"));
        as(UID,PID,()->rejects(SecurityException.class,()->h.transport.openFile(h.attribution,doc,"w",null)));
        check("original".equals(Files.readString(f.toPath())),"Denied write leaves bytes unchanged");
        as(OTHER_UID,OTHER_PID,()->rejects(SecurityException.class,()->h.transport.query(new AttributionSource(OTHER_UID,OTHER,null),doc,null,new Bundle(),null)));
        as(UID,PID,()->rejects(SecurityException.class,()->h.transport.query(new AttributionSource(OTHER_UID,OTHER,null),doc,null,new Bundle(),null)));
        h.owned(owner,root,READ|WRITE|PERSIST|PREFIX);Uri child=DocumentsContract.buildDocumentUriUsingTree(root,f.getCanonicalPath());
        try(Cursor c=h.query(child,null)){check(c.moveToFirst(),"Tree grant authorizes actual descendant");}
        File outside=new File(dir,"outside.txt");Files.writeString(outside.toPath(),"outside");Uri unrelated=DocumentsContract.buildDocumentUriUsingTree(root,outside.getCanonicalPath());
        rejects(SecurityException.class,()->h.query(unrelated,null));rejects(SecurityException.class,()->h.operation(DocumentsContract.METHOD_DELETE_DOCUMENT,unrelated,null));
        Uri made=h.operation(DocumentsContract.METHOD_CREATE_DOCUMENT,parent,"one.txt").getParcelable(DocumentsContract.EXTRA_URI,Uri.class);
        check(made!=null&&DocumentsContract.isTreeUri(made)&&!made.equals(child),"Framework create returns distinct tree-backed URI");
        check(Files.readString(f.toPath()).equals("original")&&new File(DocumentsContract.getDocumentId(made)).isFile(),"Create preserves old data and creates real new file");
        if(nativeFiles)as(UID,PID,()->{
            try(ParcelFileDescriptor fd=h.transport.openFile(h.attribution,made,"rw",null);
                    OutputStream output=new ParcelFileDescriptor.AutoCloseOutputStream(fd)){
                check(fd.getStatSize()==0,"Actual native descriptor opens new document");
                output.write("native document bytes".getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }
            try(ParcelFileDescriptor fd=h.transport.openFile(h.attribution,made,"r",null);
                    InputStream input=new ParcelFileDescriptor.AutoCloseInputStream(fd)){
                check(new String(input.readAllBytes(),java.nio.charset.StandardCharsets.UTF_8).equals("native document bytes"),
                        "Actual Parcel-delivered descriptor reads written document bytes");
            }
        });
        Uri renamed=h.operation(DocumentsContract.METHOD_RENAME_DOCUMENT,made,"renamed.txt").getParcelable(DocumentsContract.EXTRA_URI,Uri.class);
        check(renamed!=null&&new File(DocumentsContract.getDocumentId(renamed)).isFile(),"Actual rename and framework envelope");h.operation(DocumentsContract.METHOD_DELETE_DOCUMENT,renamed,null);
        check(!new File(DocumentsContract.getDocumentId(renamed)).exists(),"Actual delete removes document");
        File absent=new File(data,"absent.txt");Uri missing=document(absent);h.owned(owner,missing,READ|WRITE);
        as(UID,PID,()->rejects(FileNotFoundException.class,()->h.transport.openFile(h.attribution,missing,"w",null)));check(!absent.exists(),"Missing document is not recreated by open");
        try(Cursor c=h.query(parent,new String[]{Document.COLUMN_FLAGS,Document.COLUMN_SIZE})){check(c.moveToFirst()&&(c.getInt(0)&Document.FLAG_DIR_SUPPORTS_CREATE)!=0,"Writable directory advertises create");check((c.getInt(0)&Document.FLAG_SUPPORTS_WRITE)==0&&c.isNull(1),"Directory has no regular-file write capability or invented size");}
        h.retire(owner);rejects(SecurityException.class,()->h.query(doc,null));h.grants.flush();
    }
    static void persistence(File dir)throws Exception{
        File f=new File(dir,"durable.txt");Files.writeString(f.toPath(),"durable");File state=new File(dir,"durable-grants.xml");Uri uri=document(f);IBinder owner=new Binder();
        Harness first=new Harness(state);first.owned(owner,uri,READ|WRITE|PERSIST);as(UID,PID,()->first.ugm.takePersistableUriPermission(uri,READ,null,0));first.grants.flush();first.retire(owner);
        Harness second=new Harness(state);try(Cursor c=second.query(uri,null)){check(c.moveToFirst(),"Persisted read survives reconstructed provider and manager");}
        as(UID,PID,()->rejects(SecurityException.class,()->second.transport.openFile(second.attribution,uri,"w",null)));check(!second.grants.canAccess(second.client,uri,WRITE,0),"Untaken write does not survive restart");
        as(UID,PID,()->second.ugm.releasePersistableUriPermission(uri,READ,null,0));second.grants.flush();Harness third=new Harness(state);rejects(SecurityException.class,()->third.query(uri,null));check(state.isFile(),"Persistence uses real state file");
    }

    static void retain(Harness h, IBinder owner, Uri uri, int modes) throws Exception {
        h.owned(owner, uri, modes | PERSIST | (DocumentsContract.isTreeUri(uri) ? PREFIX : 0));
        as(UID, PID, () -> h.ugm.takePersistableUriPermission(uri, modes, null, 0));
    }

    static Set<Uri> permissionUris(Harness h, boolean persisted) {
        Set<Uri> result = new HashSet<>();
        for (android.content.UriPermission permission : h.grants.list(CLIENT, true, persisted)) {
            result.add(permission.getUri());
        }
        return result;
    }

    static void deletionLifecycle(File dir) throws Exception {
        for (String mode : List.of("complete", "parent-failure", "child-failure")) {
            File fixture = Files.createDirectory(new File(dir, "deletion-" + mode).toPath()).toFile();
            File selected = Files.createDirectory(new File(fixture, "selected").toPath()).toFile();
            File branch = mode.equals("parent-failure") ? selected
                    : Files.createDirectory(new File(selected, "nested").toPath()).toFile();
            File deleted = new File(branch, "deleted.txt"), survivor = new File(branch, "survivor.txt");
            File unrelated = new File(fixture, "unrelated.txt"), stateFile = new File(fixture, "grants.xml");
            Files.writeString(deleted.toPath(), "owned document selected for deletion");
            Files.writeString(unrelated.toPath(), "unrelated owned document");
            Harness h = new Harness(stateFile);
            IBinder owner = new Binder();
            Uri selectedUri = document(selected), deletedUri = document(deleted);
            Uri unrelatedUri = document(unrelated), survivorUri = document(survivor);
            retain(h, owner, selectedUri, READ | WRITE);
            if (!branch.equals(selected)) retain(h, owner, document(branch), READ | WRITE);
            retain(h, owner, tree(branch), READ | WRITE);
            retain(h, owner, deletedUri, READ | WRITE);
            retain(h, owner, unrelatedUri, READ);
            Set<Uri> before = permissionUris(h, true);
            check(before.containsAll(Set.of(selectedUri, deletedUri, tree(branch), unrelatedUri)),
                    "Deletion fixture holds legitimate persisted parent, child, tree and unrelated grants");
            h.grants.flush();

            boolean partial = !mode.equals("complete");
            boolean[] inserted = {false};
            Context originalContext = h.provider.getContext();
            if (partial) {
                // After actual child removal/revocation, a concurrent creator makes
                // the already enumerated directory nonempty. All file operations,
                // grants and the resulting DirectoryNotEmptyException are real;
                // only their timing is controlled at the provider context boundary.
                set(h.provider, "mContext", new ContextWrapper(originalContext) {
                    @Override public void revokeUriPermission(Uri uri, int modes) {
                        super.revokeUriPermission(uri, modes);
                        if (!inserted[0] && deletedUri.equals(uri)) {
                            inserted[0] = true;
                            try {
                                Files.writeString(survivor.toPath(), "concurrently created owned document");
                                retain(h, owner, survivorUri, READ | WRITE);
                            } catch (Exception failure) { throw new AssertionError(failure); }
                        }
                    }
                });
            }
            try {
                if (partial) {
                    try {
                        h.operation(DocumentsContract.METHOD_DELETE_DOCUMENT, selectedUri, null);
                        throw new AssertionError("Nonempty directory deletion must fail");
                    } catch (ParcelableException failure) {
                        check(failure.getCause() instanceof FileNotFoundException,
                                "AOSP transport reports the real partial deletion failure");
                    }
                } else h.operation(DocumentsContract.METHOD_DELETE_DOCUMENT, selectedUri, null);
            } finally { set(h.provider, "mContext", originalContext); }

            check(!deleted.exists() && unrelated.isFile(), "Deleted child is gone and unrelated file survives");
            Set<Uri> expected = new HashSet<>();
            if (partial) {
                check(inserted[0] && selected.isDirectory() && branch.isDirectory() && survivor.isFile(),
                        "A later directory failure preserves its parent and surviving child: " + mode);
                expected.addAll(before); expected.remove(deletedUri); expected.add(survivorUri);
            } else {
                check(!selected.exists() && !branch.exists(), "Successful deletion removes the requested tree");
                expected.add(unrelatedUri);
            }
            check(permissionUris(h, false).equals(expected),
                    "Only successfully deleted documents lose active grant records: " + mode);
            check(permissionUris(h, true).equals(expected),
                    "Only successfully deleted documents lose persisted grant records: " + mode);
            h.grants.flush();
            Harness reloaded = new Harness(stateFile);
            check(permissionUris(reloaded, true).equals(expected),
                    "Deletion retirement and surviving permissions reload from the real durable store: " + mode);
            reloaded.grants.flush();
            System.out.println("SAF_DELETE " + mode + " deletedChildPermissions=0 survivingPermissions="
                    + expected.size() + " durableReload=true");
        }
    }

    static void selection(File dir)throws Exception{
        Harness h=new Harness(new File(dir,"selection.xml"));Object origin=h.activity("Origin","RESUMED");
        File a=new File(dir,"selected-one.txt"),b=new File(dir,"selected-two.txt");Files.writeString(a.toPath(),"one");Files.writeString(b.toPath(),"two");
        Dialog dialog=new Dialog();dialog.install(h);Intent request=open().putExtra(Intent.EXTRA_ALLOW_MULTIPLE,true)
            .putExtra(Intent.EXTRA_MIME_TYPES,new String[]{"text/plain","application/pdf"}).putExtra(DocumentsContract.EXTRA_INITIAL_URI,document(a));
        long before=System.nanoTime();check(h.start(request,origin)==ActivityManager.START_SUCCESS,"Production start accepts request");
        check(System.nanoTime()-before<TimeUnit.SECONDS.toNanos(2),"startActivity returns while dialog stays pending");check(dialog.opened.await(3,TimeUnit.SECONDS),"Production worker opens dialog");
        check(dialog.ownerPid==PID&&(boolean)get(dialog.request,"multiple"),"Native request has actual owner and multiple flag");
        check(((String)get(dialog.request,"filter")).contains("*.txt")&&get(dialog.request,"initialDirectory")!=null,"MIME filters and initial URI reach backend");
        check(get(origin,"state")==state("PAUSED"),"Origin pauses before native UI");dialog.selected=new String[]{a.getPath(),b.getPath()};dialog.finish.countDown();h.settled();
        List<ResultInfo> results=h.thread.results();check(results.size()==1&&results.get(0).mResultCode==Activity.RESULT_OK,"ATM returns one real successful callback");
        Intent result=results.get(0).mData;check(result.getData()==null&&result.getClipData().getItemCount()==2,"Multiple selection uses ClipData");
        check(h.grants.canAccess(h.client,document(a),READ,0)&&h.grants.canAccess(h.client,document(b),READ,0),"All grants available before callback");
        check(get(origin,"state")==state("RESUMED"),"Origin resumes after result");int resultAt=-1,resumeAt=-1;
        for(int i=0;i<h.thread.events.size();i++){if(h.thread.events.get(i)instanceof ActivityResultItem)resultAt=i;if(h.thread.events.get(i)instanceof ResumeActivityItem)resumeAt=i;}
        check(resultAt>=0&&resumeAt>resultAt,"Result precedes onResume");h.retire((IBinder)get(origin,"token"));check(!h.grants.canAccess(h.client,document(a),READ,0),"Untaken grants retire with activity owner");h.grants.flush();
    }
    static void failures(File dir)throws Exception{
        for(String scenario:List.of("cancel","backend","missing","mime","create","retire","process","superseded","disabled-process","package","grant")){
            Harness h=new Harness(new File(dir,scenario+"-grants.xml"));Object origin=h.activity("Origin","RESUMED");Dialog dialog=new Dialog();dialog.install(h);
            File f=new File(dir,"error-"+scenario+".txt");if(!scenario.equals("missing")&&!scenario.equals("create"))Files.writeString(f.toPath(),"existing");
            Intent intent=scenario.equals("create")?new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain").putExtra(Intent.EXTRA_TITLE,"suggested.txt"):
                scenario.equals("mime")?open().setType("image/png"):open();
            check(h.start(intent,origin)==ActivityManager.START_SUCCESS&&dialog.opened.await(3,TimeUnit.SECONDS),"Request opened: "+scenario);
            if(scenario.equals("backend"))dialog.failure=new IOException("Explicit fixture backend failure");else if(!scenario.equals("cancel"))dialog.selected=new String[]{scenario.equals("create")?new File(f,"no-parent.txt").getPath():f.getPath()};
            if(scenario.equals("retire"))set(origin,"finishing",true);if(scenario.equals("process"))set(h.process,"exiting",true);
            if(scenario.equals("superseded"))set(h.process,"pidSuperseded",true);
            if(scenario.equals("disabled-process"))set(h.process,"enabledProcessLifetime",(BooleanSupplier)()->false);
            if(scenario.equals("package")){@SuppressWarnings("unchecked")Map<String,ApkPackage> packages=(Map<String,ApkPackage>)get(h.pm,"mPackages");synchronized(h.pm){packages.put(CLIENT,code(CLIENT,UID));}}
            if(scenario.equals("grant"))set(h.providers,"mRegisteredProviders",Map.of());dialog.finish.countDown();h.settled();List<ResultInfo> results=h.thread.results();
            if(List.of("retire","process","superseded","disabled-process","package").contains(scenario)){
                check(results.isEmpty(),"No late callback after retirement: "+scenario);
                check(h.thread.events.stream().noneMatch(e->e instanceof ResumeActivityItem),
                        "Retired owner process is not resumed: "+scenario);
            }
            else check(results.size()==1&&results.get(0).mResultCode==Activity.RESULT_CANCELED&&results.get(0).mData==null,"Failure cannot report URI success: "+scenario);
            check(!h.grants.canAccess(h.client,document(f),READ,0),"No grants on failed request: "+scenario);if(scenario.equals("create"))check(!f.exists(),"Failed create never invents parent directories");h.grants.flush();
        }
    }

    static ResultInfo finishSelection(Harness h, Dialog dialog, String... paths) throws Exception {
        check(dialog.opened.await(3, TimeUnit.SECONDS), "Native boundary opens before fixture selection");
        dialog.selected = paths;
        dialog.finish.countDown();
        h.settled();
        List<ResultInfo> results = h.thread.results();
        check(results.size() == 1, "Selection has exactly one result");
        return results.get(0);
    }

    static void selectionCounts(File dir) throws Exception {
        File documents = Files.createDirectory(new File(dir, "selection-counts").toPath()).toFile();
        String[] paths = new String[257];
        for (int i = 0; i < paths.length; i++) {
            File file = new File(documents, i + ".txt");
            Files.writeString(file.toPath(), "owned document " + i);
            paths[i] = file.getPath();
        }
        String[] mimes = new String[65];
        for (int i = 0; i < mimes.length - 1; i++) mimes[i] = "application/x-fixture-" + i;
        mimes[64] = "text/plain"; // The accepting filter must survive past entry 64.
        Harness h = new Harness(new File(dir, "selection-counts.xml"));
        Object origin = h.activity("Origin", "RESUMED");
        Dialog dialog = new Dialog(); dialog.install(h);
        try {
            check(h.start(open().putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                    .putExtra(Intent.EXTRA_MIME_TYPES, mimes), origin) == ActivityManager.START_SUCCESS,
                    "All 65 valid MIME filters are accepted");
            ResultInfo result = finishSelection(h, dialog, paths);
            check(((String[])get(dialog.request, "mimeTypes")).length == 65,
                    "Native request retains every MIME filter");
            check(result.mResultCode == Activity.RESULT_OK && result.mData.getData() == null
                    && result.mData.getClipData().getItemCount() == paths.length,
                    "AOSP multiple selection returns all 257 ordinary short-path documents");
            for (int i = 0; i < paths.length; i++) {
                Uri uri = document(new File(paths[i]));
                check(result.mData.getClipData().getItemAt(i).getUri().equals(uri)
                        && h.grants.canAccess(h.client, uri, READ | WRITE, 0),
                        "Selected URI and permission are preserved at index " + i);
            }
            check(dialog.localityPaths.isEmpty(), "Ordinary requests do not impose LOCAL_ONLY");
        } finally { dialog.finish.countDown(); h.settled(); h.grants.flush(); }
    }

    static void localOnly(File dir) throws Exception {
        for (String action : List.of(Intent.ACTION_OPEN_DOCUMENT, Intent.ACTION_GET_CONTENT,
                Intent.ACTION_CREATE_DOCUMENT, Intent.ACTION_OPEN_DOCUMENT_TREE)) {
            boolean create = Intent.ACTION_CREATE_DOCUMENT.equals(action);
            boolean treeRequest = Intent.ACTION_OPEN_DOCUMENT_TREE.equals(action);
            for (String locality : create ? List.of("local", "remote", "unverifiable", "changed")
                    : List.of("local", "remote", "unverifiable")) {
                File location = Files.createTempDirectory(dir.toPath(), "locality-").toFile().getCanonicalFile();
                File chosen = treeRequest ? location : new File(location, "document.txt");
                if (!create && !treeRequest) Files.writeString(chosen.toPath(), "owned existing content");
                Harness h = new Harness(new File(location.getParentFile(), location.getName() + ".xml"));
                Object origin = h.activity("Origin", "RESUMED");
                Dialog dialog = new Dialog();
                int[] queries = {0};
                dialog.locality = path -> {
                    queries[0]++;
                    if (create && queries[0] == 1) check(!chosen.exists(),
                            "Locality is checked before CREATE_DOCUMENT has filesystem side effects");
                    if (locality.equals("unverifiable")) throw new IOException("Explicit unavailable Windows locality query");
                    return locality.equals("local") || locality.equals("changed") && queries[0] == 1;
                };
                dialog.install(h);
                try {
                    Intent request = new Intent(action).putExtra(Intent.EXTRA_LOCAL_ONLY, true);
                    if (!treeRequest) request.setType("text/plain");
                    check(h.start(request, origin) == ActivityManager.START_SUCCESS,
                            "LOCAL_ONLY request starts with native validation: " + action + "/" + locality);
                    ResultInfo result = finishSelection(h, dialog, chosen.getPath());
                    check((boolean)get(dialog.request, "localOnly"), "EXTRA_LOCAL_ONLY reaches the native request");
                    boolean accepted = locality.equals("local");
                    Uri expected = treeRequest ? tree(chosen) : document(chosen);
                    if (accepted) {
                        check(result.mResultCode == Activity.RESULT_OK && result.mData.getData().equals(expected),
                                "Actual available local selection is returned: " + action);
                        check(h.grants.canAccess(h.client, expected, READ, 0), "Local result owns its real URI grant");
                        if (create) check(chosen.isFile() && chosen.length() == 0, "Local CREATE_DOCUMENT creates the requested file");
                    } else {
                        check(result.mResultCode == Activity.RESULT_CANCELED && result.mData == null,
                                "Remote/unverifiable final validation cannot report success: " + locality);
                        check(!h.grants.canAccess(h.client, expected, READ, 0), "Rejected selection has no URI permission");
                        if (create) check(!chosen.exists(), "Rejected CREATE_DOCUMENT leaves no new document");
                    }
                    check(queries[0] == (create && (accepted || locality.equals("changed")) ? 2 : 1),
                            "Create checks its parent before mutation and its actual result before publication");
                    check(get(origin, "state") == state("RESUMED"), "Locality failure/cancellation releases native UI coverage");
                    if (!create && !treeRequest) check(Files.readString(chosen.toPath()).equals("owned existing content"),
                            "Selection validation preserves existing content");
                } finally { dialog.finish.countDown(); h.settled(); h.grants.flush(); }
            }
        }

        // Canonicalization must pass the resolved target to the host query, not
        // classify a local-looking alias by its display name. Skip only on the
        // Windows ART track where creating symlinks may require administrator rights.
        if (!nativeFiles) {
            File target = new File(dir, "locality-target.txt"); Files.writeString(target.toPath(), "target");
            File alias = new File(dir, "local-looking-alias.txt");
            Files.createSymbolicLink(alias.toPath(), target.toPath());
            Harness h = new Harness(new File(dir, "locality-alias.xml"));
            Object origin = h.activity("Origin", "RESUMED");
            Dialog dialog = new Dialog(); dialog.locality = path -> false; dialog.install(h);
            try {
                h.start(open().putExtra(Intent.EXTRA_LOCAL_ONLY, true), origin);
                ResultInfo result = finishSelection(h, dialog, alias.getPath());
                String resolved = (String)call(type("Zygote"), "windowsPath", new Class<?>[]{String.class}, target.getCanonicalPath());
                check(dialog.localityPaths.equals(List.of(resolved)) && result.mResultCode == Activity.RESULT_CANCELED,
                        "A local-looking symlink is checked using its resolved target and cannot bypass locality");
            } finally { dialog.finish.countDown(); h.settled(); h.grants.flush(); }
        }
    }

    static void actions(File dir) throws Exception {
        File existing = new File(dir, "save-existing.txt");
        Files.writeString(existing.toPath(), "keep existing bytes");
        Harness create = new Harness(new File(dir, "create-action.xml"));
        Object owner = create.activity("Origin", "RESUMED");
        Dialog save = new Dialog(); save.install(create);
        check(create.start(new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain")
                .putExtra(Intent.EXTRA_TITLE, "suggested.txt")
                .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true), owner) == ActivityManager.START_SUCCESS,
                "CREATE_DOCUMENT starts a real save request");
        ResultInfo saved = finishSelection(create, save, existing.getPath());
        check((int)get(save.request, "mode") == 1 && !(boolean)get(save.request, "multiple")
                && "suggested.txt".equals(get(save.request, "suggestedName"))
                && "text/plain".equals(libcore.content.type.MimeMap.getDefault()
                    .guessMimeTypeFromExtension((String)get(save.request, "extension"))),
                "Save mode, name and MIME extension reach native boundary");
        check(saved.mResultCode == Activity.RESULT_OK, "Actual create succeeds");
        Uri made = saved.mData.getData();
        File madeFile = new File(DocumentsContract.getDocumentId(made));
        check(!made.equals(document(existing)) && madeFile.isFile() && madeFile.length() == 0
                && Files.readString(existing.toPath()).equals("keep existing bytes"),
                "Save selection never overwrites an existing file");
        check(create.grants.canAccess(create.client, made, READ | WRITE, 0)
                && (saved.mData.getFlags() & PERSIST) != 0, "Created document has real read/write persistable grant");
        create.grants.flush();

        Harness content = new Harness(new File(dir, "content-action.xml"));
        Object source = content.activity("Origin", "RESUMED");
        Dialog get = new Dialog(); get.install(content);
        content.start(new Intent(Intent.ACTION_GET_CONTENT).setType("text/plain"), source);
        ResultInfo got = finishSelection(content, get, existing.getPath());
        check(got.mResultCode == Activity.RESULT_OK && got.mData.getData().equals(document(existing)),
                "GET_CONTENT returns selected real document");
        check((got.mData.getFlags() & (READ | WRITE | PERSIST | PREFIX)) == READ
                && !content.grants.canAccess(content.client, document(existing), WRITE, 0),
                "GET_CONTENT delegates read access only");
        as(UID, PID, () -> rejects(SecurityException.class,
                () -> content.ugm.takePersistableUriPermission(got.mData.getData(), READ, null, 0)));
        content.grants.flush();

        Harness tree = new Harness(new File(dir, "tree-action.xml"));
        Object treeSource = tree.activity("Origin", "RESUMED");
        Dialog folder = new Dialog(); folder.install(tree);
        tree.start(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), treeSource);
        ResultInfo picked = finishSelection(tree, folder, dir.getPath());
        check(picked.mResultCode == Activity.RESULT_OK && DocumentsContract.isTreeUri(picked.mData.getData())
                && (picked.mData.getFlags() & PREFIX) != 0, "Tree selection returns actual prefix grant");
        Uri child = DocumentsContract.buildDocumentUriUsingTree(picked.mData.getData(), existing.getCanonicalPath());
        try (Cursor cursor = tree.query(child, null)) { check(cursor.moveToFirst(), "Selected tree opens existing descendant through AOSP transport"); }
        tree.grants.flush();

        for (String scenario : List.of("count", "directory", "root", "create-grant")) {
            Harness h = new Harness(new File(dir, "invalid-" + scenario + ".xml"));
            Object origin = h.activity("Origin", "RESUMED");
            Dialog dialog = new Dialog(); dialog.install(h);
            Intent request = scenario.equals("root") ? new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                    : scenario.equals("create-grant") ? new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain") : open();
            h.start(request, origin);
            check(dialog.opened.await(3, TimeUnit.SECONDS), "Invalid selection request opens: " + scenario);
            File newFile = new File(dir, "grant-failed-create.txt");
            if (scenario.equals("create-grant")) set(h.providers, "mRegisteredProviders", Map.of());
            ResultInfo result = finishSelection(h, dialog, scenario.equals("count")
                    ? new String[]{existing.getPath(), madeFile.getPath()}
                    : new String[]{scenario.equals("root") ? "/" : scenario.equals("directory") ? dir.getPath() : newFile.getPath()});
            check(result.mResultCode == Activity.RESULT_CANCELED && result.mData == null,
                    "Invalid selection cannot report success: " + scenario);
            if (scenario.equals("create-grant")) check(!newFile.exists(), "Failed grant removes its unused newly created file");
            h.grants.flush();
        }
    }

    static void lifecycle(File dir) throws Exception {
        Harness h = new Harness(new File(dir, "covered.xml"));
        Object origin = h.activity("Origin", "PAUSED"), top = h.activity("Top", "RESUMED");
        Dialog dialog = new Dialog(); dialog.install(h);
        h.start(open().addFlags(Intent.FLAG_ACTIVITY_NO_USER_ACTION), origin);
        check(dialog.opened.await(3, TimeUnit.SECONDS), "Request from paused source opens above actual top");
        PauseActivityItem pause = (PauseActivityItem)h.thread.events.stream().filter(e -> e instanceof PauseActivityItem).findFirst().orElseThrow();
        check(pause.getActivityToken().equals(get(top, "token")) && !(boolean)get(pause, "mUserLeaving"),
                "Picker pauses covered top and honors NO_USER_ACTION");
        check(h.start(open(), origin) == ActivityManager.START_CANCELED, "Duplicate owner request is bounded");
        dialog.finish.countDown(); h.settled();
        check(get(top, "state") == state("RESUMED") && get(origin, "state") == state("PAUSED"),
                "Picker resumes covered top without promoting paused result recipient");
        check(h.thread.results().isEmpty() && ((List<?>)get(origin, "pendingResults")).size() == 1,
                "Paused recipient retains its pending cancellation until normal resume");
        h.grants.flush();

        Harness delayed = new Harness(new File(dir, "delayed-pause.xml"));
        Object delayedOrigin = delayed.activity("Origin", "RESUMED");
        delayed.thread.acknowledge = false;
        Dialog delayedDialog = new Dialog(); delayedDialog.install(delayed);
        delayed.start(open(), delayedOrigin);
        check(delayedDialog.opened.await(3, TimeUnit.SECONDS), "Pause acknowledgment wait is bounded");
        delayedDialog.finish.countDown(); delayed.settled();
        check(get(delayedOrigin, "state") == state("PAUSING") && delayed.thread.results().isEmpty(),
                "Completion does not resume before outstanding pause acknowledgment");
        delayed.thread.acknowledge = true;
        as(UID, PID, () -> delayed.thread.controller.activityPaused((IBinder)get(delayedOrigin, "token")));
        check(get(delayedOrigin, "state") == state("RESUMED") && delayed.thread.results().size() == 1,
                "Late pause acknowledgment delivers pending result and resumes once");
        delayed.grants.flush();

        Harness newTask = new Harness(new File(dir, "new-task.xml"));
        Object newTaskOrigin = newTask.activity("Origin", "RESUMED");
        Dialog newTaskDialog = new Dialog(); newTaskDialog.install(newTask);
        newTask.start(open().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), newTaskOrigin);
        check(newTaskDialog.opened.await(3, TimeUnit.SECONDS), "NEW_TASK request opens");
        List<ResultInfo> cancellation = newTask.thread.results();
        check(cancellation.size() == 1 && cancellation.get(0).mResultCode == Activity.RESULT_CANCELED,
                "NEW_TASK immediately cancels activity-for-result relationship");
        newTaskDialog.finish.countDown(); newTask.settled();
        check(newTask.thread.results().size() == 1, "NEW_TASK completion does not deliver a second result");
        newTask.grants.flush();
    }

    static void workersRetired(String name) throws Exception {
        for (Thread thread : Thread.getAllStackTraces().keySet()) {
            if (thread.isAlive() && thread.getName().equals(name)) {
                thread.join(3000);
                check(!thread.isAlive(), "Native UI worker did not retire: " + name);
            }
        }
    }

    static int pending(Harness h, String name) throws Exception {
        synchronized (h.atm) { return ((Map<?,?>)get(h.atm, name)).size(); }
    }

    static void awaitPending(Harness h, int documents, int choices) throws Exception {
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
        while (System.nanoTime() < until) {
            if (pending(h, "mDocumentPickers") == documents && pending(h, "mIntentChoosers") == choices) return;
            Thread.sleep(5);
        }
        throw new AssertionError("Native UI reservations did not retire");
    }

    static Intent chooser() {
        return Intent.createChooser(new Intent().setComponent(new ComponentName(CLIENT, CLIENT + ".Top")),
                "Native UI fixture").putExtra(Intent.EXTRA_AUTO_LAUNCH_SINGLE_CHOICE, false);
    }

    /** Real AppTask admission must retain its exact task across asynchronous native UI. */
    static void appTaskNativeUi(File dir) throws Exception {
        for (boolean choose : new boolean[]{false, true}) for (boolean retire : new boolean[]{false, true}) {
            String name = "app-task-" + (choose ? "chooser" : "document") + "-" + retire;
            Harness h = new Harness(new File(dir, name + ".xml"));
            Object origin = h.activity("Origin", "RESUMED");
            // Seed the same task metadata that normal activity publication records.
            synchronized (h.atm) {
                call(h.atm, "recordAppTaskLocked", new Class<?>[]{origin.getClass(), boolean.class}, origin, false);
            }
            if (retire) {
                // Another live task keeps the caller process alive. The pending
                // request must retire because its task disappeared, independently
                // of process death or package replacement.
                Object keeper = h.activity("Origin", "PAUSED"); set(keeper, "taskId", 2);
                @SuppressWarnings("unchecked") List<Integer> order = (List<Integer>)get(h.atm, "mTaskOrder");
                order.add(2);
            }
            IAppTask[] handle = new IAppTask[1];
            as(UID, PID, () -> handle[0] = IAppTask.Stub.asInterface(h.atm.getAppTasks(CLIENT).get(0)));
            Dialog documents = new Dialog(); documents.holdAfterRetirement = true; documents.install(h);
            ChoiceDialog choices = new ChoiceDialog(); choices.selected = 0; choices.install(h);
            File created = new File(dir, name + ".txt");
            documents.selected = new String[]{created.getPath()};
            try {
                Intent request = choose ? chooser() : new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain");
                as(UID, PID, () -> check(handle[0].startActivity(h.thread.asBinder(), CLIENT, null,
                        request, request.getType(), null) == ActivityManager.START_SUCCESS,
                        "AppTask admits the requested native UI: " + name));
                check((choose ? choices.opened : documents.opened).await(3, TimeUnit.SECONDS),
                        "Native UI opens in the explicit task without an activity result token");
                BooleanSupplier active = choose ? choices.active : documents.active;
                check(active.getAsBoolean() && get(origin, "state") == state("PAUSED"),
                        "Explicit task is genuinely covered while its native UI is active");
                Object replacement = null;
                if (retire) {
                    as(UID, PID, () -> handle[0].finishAndRemoveTask());
                    check(!active.getAsBoolean(), "Removing the task retires the pending native session");
                    as(UID, PID, () -> h.thread.controller.activityDestroyed((IBinder)get(origin, "token")));
                    as(UID, PID, () -> check(handle[0].getTaskInfo() == null,
                            "Destroy acknowledgment completes removal before the fixture reuses its id"));
                    check(!(boolean)get(h.process, "exiting") && !(boolean)get(h.process, "dead"),
                            "The caller process remains live while its selected task is removed");
                    replacement = h.activity("Origin", "RESUMED");
                    synchronized (h.atm) {
                        call(h.atm, "recordAppTaskLocked", new Class<?>[]{replacement.getClass(), boolean.class}, replacement, false);
                    }
                    check(!active.getAsBoolean(), "Reusing the numeric task id cannot revive the old native session");
                }
                documents.finish.countDown(); choices.finish.countDown();
                h.settled(); workersRetired("intent-chooser"); awaitPending(h, 0, 0);
                check(h.thread.results().isEmpty(), "AppTask launch does not invent an activity-result recipient");
                check((int)get(origin, "nativeUiCount") == 0 && (int)get(origin, "chooserCoverCount") == 0,
                        "Native session releases exactly its own coverage");
                if (retire) {
                    check(!created.exists() && get(replacement, "state") == state("RESUMED")
                                    && ((List<?>)get(h.atm, "mActivities")).stream().noneMatch(record -> {
                                        try { return ((ActivityInfo)get(record, "info")).name.endsWith(".Top"); }
                                        catch (Exception failure) { throw new AssertionError(failure); }
                                    }), "Retired session cannot create a document or launch into the replacement task");
                } else if (choose) {
                    ActivityManager.RecentTaskInfo[] task = new ActivityManager.RecentTaskInfo[1];
                    as(UID, PID, () -> task[0] = handle[0].getTaskInfo());
                    check(task[0].taskId == 1 && task[0].numActivities == 2
                                    && task[0].topActivity.equals(new ComponentName(CLIENT, CLIENT + ".Top")),
                            "Chosen activity starts inside the exact original AppTask");
                } else {
                    check(created.isFile() && get(origin, "state") == state("RESUMED"),
                            "Active explicit-task document request creates its selected file and uncovers the task");
                }
            } finally {
                documents.finish.countDown(); choices.finish.countDown();
                h.settled(); workersRetired("intent-chooser"); h.grants.flush();
            }
        }
    }

    static void idleOwnership(File dir) throws Exception {
        File selected = new File(dir, "headless-selected.txt");
        Files.writeString(selected.toPath(), "actual selected document");
        for (String mode : List.of("cancel", "no-poll", "backend", "runtime", "linkage", "missing", "process", "package", "selected")) {
            Harness h = new Harness(new File(dir, "idle-" + mode + ".xml"));
            set(h.atm, "mIdleSince", 1L);
            check(h.idleSince() == 1, "Activity-free caller starts with an existing idle interval");
            Dialog dialog = new Dialog(); dialog.holdAfterRetirement = true; dialog.install(h);
            check(h.start(open().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null, -1) == ActivityManager.START_SUCCESS,
                    "Headless picker starts: " + mode);
            check(dialog.opened.await(3, TimeUnit.SECONDS), "Headless picker opens: " + mode);
            check(((List<?>)get(h.atm, "mActivities")).isEmpty() && pending(h, "mDocumentPickers") == 1
                    && dialog.active.getAsBoolean(), "Real native session owns the headless request without an invented activity");
            check((Long)get(h.atm, "mIdleSince") == 0, "Picker admission resets idle without a monitor poll: " + mode);
            if (!mode.equals("no-poll")) check(h.idleSince() == 0, "Open picker keeps the device busy: " + mode);
            if (mode.equals("backend")) dialog.failure = new IOException("Fixture native dialog unavailable");
            if (mode.equals("runtime")) dialog.failure = new IllegalStateException("Fixture native dialog failed");
            if (mode.equals("linkage")) dialog.failure = new UnsatisfiedLinkError("Fixture native backend disappeared");
            if (mode.equals("missing")) dialog.selected = new String[]{new File(dir, "not-selected.txt").getPath()};
            if (mode.equals("selected")) dialog.selected = new String[]{selected.getPath()};
            if (mode.equals("process")) call(h.atm, "processDied", new Class<?>[]{h.process.getClass()}, h.process);
            if (mode.equals("package")) {
                @SuppressWarnings("unchecked") Map<String,ApkPackage> packages = (Map<String,ApkPackage>)get(h.pm, "mPackages");
                synchronized (h.pm) { packages.put(CLIENT, code(CLIENT, UID)); }
            }
            if (mode.equals("process") || mode.equals("package")) {
                check(!dialog.active.getAsBoolean() && pending(h, "mDocumentPickers") == 1 && h.idleSince() == 0,
                        "Retired owner keeps busy ownership until its actual native worker returns: " + mode);
            }
            long closedAfter = SystemClock.uptimeMillis();
            dialog.finish.countDown(); h.settled();
            long finishedIdle = (Long)get(h.atm, "mIdleSince");
            check(pending(h, "mDocumentPickers") == 0 && finishedIdle >= closedAfter && finishedIdle > 1,
                    "Final picker cleanup starts a fresh idle interval: " + mode);
            check(h.idleSince() == finishedIdle && h.thread.results().isEmpty()
                    && ((List<?>)get(h.atm, "mActivities")).isEmpty(),
                    "Completion is stable and does not invent a result recipient: " + mode);
            h.grants.flush();
        }
        Harness unavailable = new Harness(new File(dir, "idle-unavailable.xml"));
        Dialog noBackend = new Dialog(); noBackend.available = false; noBackend.install(unavailable);
        set(unavailable.atm, "mIdleSince", 1L);
        check(unavailable.start(open().addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), null, -1)
                == ActivityManager.START_INTENT_NOT_RESOLVED && pending(unavailable, "mDocumentPickers") == 0
                && unavailable.idleSince() == 1, "Unavailable picker never acquires busy ownership");
        unavailable.grants.flush();
    }

    static void combinedNativeUi(File dir) throws Exception {
        for (String order : List.of("document-first", "chooser-first")) {
            for (String mode : List.of("headless", "resumed", "late-pause", "partial-pause")) {
                boolean headless = mode.equals("headless");
                boolean delayed = mode.endsWith("pause");
                Harness h = new Harness(new File(dir, "combined-" + order + "-" + mode + ".xml"));
                Object origin = headless ? null : h.activity("Origin", "RESUMED");
                if (headless) set(h.atm, "mIdleSince", 1L);
                if (delayed) h.thread.acknowledge = false;
                Dialog documents = new Dialog(); documents.install(h);
                ChoiceDialog choices = new ChoiceDialog(); choices.install(h);
                Intent documentRequest = open(), choiceRequest = chooser();
                if (headless) {
                    documentRequest.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    choiceRequest.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                }
                if (order.equals("document-first")) {
                    check(h.start(documentRequest, origin, headless ? -1 : 41) == ActivityManager.START_SUCCESS,
                            "Document request admitted before chooser");
                    check(documents.opened.await(3, TimeUnit.SECONDS), "First document UI opens");
                    check(h.start(choiceRequest, origin, headless ? -1 : 42) == ActivityManager.START_SUCCESS,
                            "Chooser admitted above document request");
                } else {
                    check(h.start(choiceRequest, origin, headless ? -1 : 42) == ActivityManager.START_SUCCESS,
                            "Chooser admitted before document request");
                    check(choices.opened.await(3, TimeUnit.SECONDS), "First choice UI opens");
                    check(h.start(documentRequest, origin, headless ? -1 : 41) == ActivityManager.START_SUCCESS,
                            "Document request admitted above chooser");
                }
                check(documents.opened.await(3, TimeUnit.SECONDS) && choices.opened.await(3, TimeUnit.SECONDS)
                        && pending(h, "mDocumentPickers") == 1 && pending(h, "mIntentChoosers") == 1
                        && h.idleSince() == 0, "Both native sessions retain their independent reservations");
                if (!headless) check(get(origin, "state") == state(delayed ? "PAUSING" : "PAUSED"),
                        "Shared covered activity remains under its actual pause lifecycle");
                // Close the first admitted session; the other must retain the device
                // and its coverage until its own native worker has actually returned.
                if (order.equals("document-first")) {
                    documents.finish.countDown(); awaitPending(h, 0, 1); workersRetired("document-picker");
                    check(choices.active.getAsBoolean(), "Document cleanup preserves live chooser");
                } else {
                    choices.finish.countDown(); awaitPending(h, 1, 0); workersRetired("intent-chooser");
                    check(documents.active.getAsBoolean(), "Chooser cleanup preserves live document picker");
                }
                check(h.idleSince() == 0 && h.thread.results().isEmpty(), "Remaining native UI retains busy lifetime and pending results");
                if (!headless) check(get(origin, "state") == state(delayed ? "PAUSING" : "PAUSED")
                        && h.thread.events.stream().noneMatch(e -> e instanceof ResumeActivityItem),
                        "Closing one native session cannot resume the activity under the other");
                if (mode.equals("partial-pause")) {
                    h.thread.acknowledge = true;
                    as(UID, PID, () -> h.thread.controller.activityPaused((IBinder)get(origin, "token")));
                    check(get(origin, "state") == state("PAUSED") && h.thread.results().isEmpty()
                            && h.thread.events.stream().noneMatch(e -> e instanceof ResumeActivityItem),
                            "Late pause acknowledgment preserves the remaining native coverage");
                }
                long closedAfter = SystemClock.uptimeMillis();
                documents.finish.countDown(); choices.finish.countDown();
                h.settled(); workersRetired("intent-chooser"); awaitPending(h, 0, 0);
                if (mode.equals("late-pause")) {
                    check(get(origin, "state") == state("PAUSING") && h.thread.results().isEmpty()
                            && h.thread.events.stream().noneMatch(e -> e instanceof ResumeActivityItem),
                            "Both completed dialogs wait for the outstanding real pause acknowledgment");
                    h.thread.acknowledge = true;
                    as(UID, PID, () -> h.thread.controller.activityPaused((IBinder)get(origin, "token")));
                }
                if (headless) {
                    check(h.idleSince() >= closedAfter && h.thread.results().isEmpty()
                            && ((List<?>)get(h.atm, "mActivities")).isEmpty(), "Last native UI starts headless idle without creating an activity");
                } else {
                    check(get(origin, "state") == state("RESUMED") && h.thread.results().size() == 2,
                            "Last native UI resumes once with both real cancellations");
                    check(h.thread.events.stream().filter(e -> e instanceof ResumeActivityItem).count() == 1,
                            "Combined completion schedules one resume");
                }
                h.grants.flush();
            }
        }
    }

    /** Also used by the detached-window fixture, which shares this actual Harness. */
    static void withServiceContext(Work operation) throws Exception {
        FixtureServiceContext.Scope previous = serviceContexts;
        try (FixtureBindings bindings = new FixtureBindings();
                FixtureServiceContext.Scope owned = new FixtureServiceContext.Scope()) {
            serviceContexts = owned;
            operation.run();
        } finally { serviceContexts = previous; }
    }

    /** Invoked in three separate JVM/ART processes; only the real XML store is shared. */
    public static void restartPhase(String phase, File directory) throws Exception {
        withServiceContext(() -> restartPhaseWithServiceContext(phase, directory));
    }

    private static void restartPhaseWithServiceContext(String phase, File directory) throws Exception {
        field(Class.forName("com.gesher.app.locale.SystemLocale"),"sIs24Hour").set(null,Boolean.TRUE);
        File data = new File(directory, "persisted.txt"), state = new File(directory, "grants.xml");
        if (phase.equals("write")) Files.writeString(data.toPath(), "survives process exit");
        Harness h = new Harness(state); Uri uri = document(data);
        if (phase.equals("write")) {
            h.owned(new Binder(), uri, READ | WRITE | PERSIST);
            as(UID, PID, () -> h.ugm.takePersistableUriPermission(uri, READ | WRITE, null, 0));
            as(UID, PID, () -> h.ugm.releasePersistableUriPermission(uri, WRITE, null, 0));
            h.grants.flush();
            check(state.isFile(), "First process writes durable state");
        } else if (phase.equals("read")) {
            try (Cursor cursor = h.query(uri, null)) { check(cursor.moveToFirst(), "Second process reloads persisted read through provider transport"); }
            check(!h.grants.canAccess(h.client, uri, WRITE, 0), "Released write does not return after process restart");
            as(UID, PID, () -> rejects(SecurityException.class,
                    () -> h.ugm.takePersistableUriPermission(uri, WRITE, null, 0)));
            as(UID, PID, () -> h.ugm.releasePersistableUriPermission(uri, READ, null, 0));
            h.grants.flush();
        } else if (phase.equals("released")) {
            rejects(SecurityException.class, () -> h.query(uri, null));
            check(!h.grants.canAccess(h.client, uri, READ | WRITE, 0), "Third process preserves release");
        } else throw new IllegalArgumentException("Unknown restart phase");
        System.out.println("SafDocumentsContractTest restart " + phase + " passed");
    }
    public static void contracts(boolean descriptors) throws Exception {
        withServiceContext(() -> contractsWithServiceContext(descriptors));
    }

    private static void contractsWithServiceContext(boolean descriptors) throws Exception {
        nativeFiles=descriptors;field(Class.forName("com.gesher.app.locale.SystemLocale"),"sIs24Hour").set(null,Boolean.TRUE);
        File dir=Files.createTempDirectory("gesher-saf-production-").toFile().getCanonicalFile();
        try{provider(dir);persistence(dir);deletionLifecycle(dir);selection(dir);selectionCounts(dir);localOnly(dir);failures(dir);actions(dir);lifecycle(dir);idleOwnership(dir);combinedNativeUi(dir);appTaskNativeUi(dir);
            if(nativeFiles){
                check(nativeIncoming.get()>nativeOtherIncoming.get()&&nativeOtherIncoming.get()>0,
                        "SAF exercised actual incoming Binder transactions from both ordinary peers");
                System.out.println("SAF_BINDER_NATIVE peer credential provenance verified: "+UID+"/"+PID+" and "+OTHER_UID+"/"+OTHER_PID+
                        "; incoming="+nativeIncoming.get()+"; ungranted="+nativeOtherIncoming.get());
            }
            System.out.println("SafDocumentsContractTest passed: "+checks+" checks; nativeFiles="+nativeFiles);}
        finally{try(var paths=Files.walk(dir.toPath())){for(var p:paths.sorted(Comparator.reverseOrder()).toList())Files.deleteIfExists(p);}}
    }
    public static void main(String[] args)throws Exception{
        if(args.length==2&&args[0].equals("registry")){registryMain(args[1]);return;}
        if(args.length==3&&args[0].equals("relay")){relayMain(Integer.parseInt(args[1]),args[2]);return;}
        System.loadLibrary("android_runtime");
        nativeFiles=true;
        connectRelays();
        if(args.length==2)restartPhase(args[0],new File(args[1]));else contracts(true);
    }
}
