#!/usr/bin/env python3
"""Execute production worker-unavailable gates and per-origin storage queries.

Android framework scheduling/content and the native bridge are recording seams.
Prepared worker interception code is tested separately by explicitly constructing
and publishing an isolated fixture instance; it is not production support.
No browser or network is used; Windows WebView2 execution is a separate gate.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile
from test_install_metadata_comparison import method

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
'android/Manifest.java': '''package android; public class Manifest { public static class permission { public static final String INTERNET="android.permission.INTERNET"; } }''',
'android/content/pm/PackageManager.java': '''package android.content.pm; public class PackageManager { public static final int PERMISSION_GRANTED=0; }''',
'android/os/Process.java': '''package android.os; public class Process { public static int myPid(){return 11;} public static int myUid(){return 22;} }''',
'android/os/Looper.java': '''package android.os; public class Looper { public static final Looper MAIN=new Looper(); public static Looper myLooper(){return MAIN;} public static Looper getMainLooper(){return MAIN;} }''',
'android/os/Handler.java': '''package android.os; public class Handler { public static int posts; public Handler(Looper l){} public boolean post(Runnable r){posts++;r.run();return true;} }''',
'android/net/Uri.java': '''package android.net; public class Uri { private final String v; private Uri(String v){this.v=v;} public static Uri parse(String v){return new Uri(v);} public String getScheme(){int c=v.indexOf(':');return c<0?null:v.substring(0,c);} public String toString(){return v;} }''',
'android/content/Context.java': '''package android.content; public class Context { public boolean internet=true; public final ContentResolver resolver=new ContentResolver(); public int checkPermission(String p,int pid,int uid){return internet?0:-1;} public ContentResolver getContentResolver(){return resolver;} }''',
'android/content/ContentResolver.java': '''package android.content; public class ContentResolver { public int reads; public java.io.InputStream openInputStream(android.net.Uri uri) throws java.io.IOException {reads++;return new java.io.ByteArrayInputStream(new byte[]{7,8});} public String getType(android.net.Uri uri){return "text/plain";} }''',
'android/app/ActivityThread.java': '''package android.app; public class ActivityThread { public static int calls; public static android.content.Context app; public static android.content.Context currentApplication(){calls++;return app;} }''',
'android/webkit/ServiceWorkerController.java': '''package android.webkit; public abstract class ServiceWorkerController { public abstract ServiceWorkerWebSettings getServiceWorkerWebSettings(); public abstract void setServiceWorkerClient(ServiceWorkerClient c); }''',
'android/webkit/ServiceWorkerClient.java': '''package android.webkit; public abstract class ServiceWorkerClient { public WebResourceResponse shouldInterceptRequest(WebResourceRequest r){return null;} }''',
'android/webkit/ServiceWorkerWebSettings.java': '''package android.webkit; public abstract class ServiceWorkerWebSettings { public abstract void setCacheMode(int v); public abstract int getCacheMode(); public abstract void setAllowContentAccess(boolean v); public abstract boolean getAllowContentAccess(); public abstract void setAllowFileAccess(boolean v); public abstract boolean getAllowFileAccess(); public abstract void setBlockNetworkLoads(boolean v); public abstract boolean getBlockNetworkLoads(); }''',
'android/webkit/WebSettings.java': '''package android.webkit; public class WebSettings { public static final int LOAD_DEFAULT=-1,LOAD_NORMAL=0,LOAD_CACHE_ELSE_NETWORK=1,LOAD_NO_CACHE=2,LOAD_CACHE_ONLY=3; }''',
'android/webkit/WebResourceRequest.java': '''package android.webkit; public interface WebResourceRequest { android.net.Uri getUrl(); boolean isForMainFrame(); boolean isRedirect(); boolean hasGesture(); String getMethod(); java.util.Map<String,String> getRequestHeaders(); }''',
'android/webkit/WebResourceResponse.java': '''package android.webkit; public class WebResourceResponse { final String mime,encoding,reason; final int status; final java.util.Map<String,String> headers; final java.io.InputStream data; public WebResourceResponse(String m,String e,java.io.InputStream d){this(m,e,200,"OK",null,d);} public WebResourceResponse(String m,String e,int s,String r,java.util.Map<String,String> h,java.io.InputStream d){mime=m;encoding=e;status=s;reason=r;headers=h;data=d;} public String getMimeType(){return mime;} public String getEncoding(){return encoding;} public int getStatusCode(){return status;} public String getReasonPhrase(){return reason;} public java.util.Map<String,String> getResponseHeaders(){return headers;} public java.io.InputStream getData(){return data;} }''',
'android/webkit/ValueCallback.java': '''package android.webkit; public interface ValueCallback<T>{void onReceiveValue(T value);}''',
'android/webkit/WebStorage.java': '''package android.webkit; public class WebStorage { public void getOrigins(ValueCallback<java.util.Map> c){} public void getUsageForOrigin(String o,ValueCallback<Long> c){} public void getQuotaForOrigin(String o,ValueCallback<Long> c){} public void deleteOrigin(String o){} public void deleteAllData(){} }''',
'com/gesher/app/webview/GesherWebView.java': '''package com.gesher.app.webview; final class GesherWebView { static boolean fail; static int configurations; static void configure(android.content.Context c){configurations++;if(fail)throw new IllegalStateException("configuration unavailable");} }''',
'com/gesher/app/webview/FileUrls.java': '''package com.gesher.app.webview; final class FileUrls { static boolean lastAllowed; static String fromWebView2(String v){return v.replace("https://gesher-file.example/","file:///");} static android.webkit.WebResourceResponse load(android.content.Context c,String v,boolean allowed){lastAllowed=allowed;return new android.webkit.WebResourceResponse("text/plain",null,allowed?200:404,allowed?"OK":"Not Found",null,new java.io.ByteArrayInputStream(new byte[0]));} }''',
'com/gesher/app/webview/WebView2Host.java': '''package com.gesher.app.webview;
final class WebView2Host {
 interface GlobalCallback {void onResult(int t,int hr,String v);}
 static boolean accepted=true,enable=true; static int hr; static String json="{\\"usage\\":1234,\\"quota\\":99999}";
 static int queries,enables,threadChecks; static String origin; static boolean webViewThread;
 static final int DATA_ALL_DOM_STORAGE=32,DATA_FILE_SYSTEMS=1,DATA_INDEXED_DB=2,DATA_LOCAL_STORAGE=4,DATA_WEB_SQL=8,DATA_CACHE_STORAGE=16;
 static boolean clearOriginData(String o,GlobalCallback cb){return true;}
 static boolean clearBrowsingData(int flags,GlobalCallback cb){return true;}
 static GlobalCallback pending;
 static boolean queryOriginStorage(String o,GlobalCallback cb){queries++;origin=o;if(!accepted)return false;pending=cb;return true;}
 static boolean enableServiceWorkers(GlobalCallback cb){enables++;cb.onResult(2,enable?0:-1,"");return true;}
 static boolean onWebViewThread(){threadChecks++;return webViewThread;}
 static final java.util.Map<Long,Result> responses=new java.util.concurrent.ConcurrentHashMap<>();
 record Result(int status,String reason,String headers,byte[] body){}
 static boolean respondToServiceWorker(long id,int s,String r,String h,byte[] b){responses.put(id,new Result(s,r,h,b));return true;}
}''',
'org/json/JSONObject.java': '''package org.json;
// Recording scalar-parser boundary; this fixture does not validate org.json itself.
public class JSONObject { private final String s; public JSONObject(String s){this.s=s;}
 public Object get(String key) throws Exception {java.util.regex.Matcher m=java.util.regex.Pattern.compile("\\\""+key+"\\\"\\\\s*:\\\\s*([^,}]+)").matcher(s);if(!m.find())throw new Exception();String v=m.group(1).trim();if(v.startsWith("\\\""))return v;if(v.equals("null"))return null;if(!v.contains(".")&&!v.contains("e")&&!v.contains("E"))return Long.valueOf(v);return Double.valueOf(v);}
}''',
'org/chromium/support_lib_boundary/ServiceWorkerClientBoundaryInterface.java': '''package org.chromium.support_lib_boundary; public interface ServiceWorkerClientBoundaryInterface { android.webkit.WebResourceResponse shouldInterceptRequest(android.webkit.WebResourceRequest request); }''',
}

TEST = r'''package com.gesher.app.webview;
import android.webkit.*;
import java.io.*;
import java.lang.reflect.*;
import java.util.*;
public class WorkerContractHost {
 static int checks;
 static void check(boolean v,String m){checks++;if(!v)throw new AssertionError(m);}
 static void throwsType(Class<?> type,Runnable call,String m){try{call.run();throw new AssertionError(m);}catch(Throwable t){check(type.isInstance(t),m+": "+t);}}
 static WebView2Host.Result response(long id) throws Exception {long until=System.nanoTime()+2_000_000_000L;while(!WebView2Host.responses.containsKey(id)&&System.nanoTime()<until)Thread.yield();check(WebView2Host.responses.containsKey(id),"response completed");return WebView2Host.responses.get(id);}
 static class Stream extends ByteArrayInputStream {boolean closed; Stream(byte[] b){super(b);} public void close(){closed=true;} }
 public static void main(String[] args) throws Exception {
  android.content.Context app=new android.content.Context();
  android.app.ActivityThread.app=app;
  // Prepared internals only: production get() is unavailable. No application
  // path performs this explicit construction/publication used by this fixture.
  GesherServiceWorkers controller=new GesherServiceWorkers(app);
  Field instance=GesherServiceWorkers.class.getDeclaredField("sInstance");
  instance.setAccessible(true);instance.set(null,controller);
  ServiceWorkerWebSettings settings=controller.getServiceWorkerWebSettings();
  check(settings.getCacheMode()==-1,"default cache mode");
  check(settings.getAllowFileAccess()&&settings.getAllowContentAccess(),"Chromium default file/content permission");
  check(!settings.getBlockNetworkLoads(),"INTERNET default");
  for(int mode:new int[]{0,1,2,3,99})throwsType(UnsupportedOperationException.class,()->settings.setCacheMode(mode),"unsupported cache mode must not succeed");
  settings.setCacheMode(-1);check(settings.getCacheMode()==-1,"failed setter leaves mode intact");
  GesherServiceWorkers.Settings denied=new GesherServiceWorkers.Settings(false);
  check(denied.getBlockNetworkLoads(),"missing INTERNET defaults blocked");
  throwsType(SecurityException.class,()->denied.setBlockNetworkLoads(false),"permission cannot be bypassed");
  check(denied.getBlockNetworkLoads(),"failed permission change intact");
  final WebResourceRequest[] seen={null};
  controller.setServiceWorkerClient(new ServiceWorkerClient(){public WebResourceResponse shouldInterceptRequest(WebResourceRequest r){seen[0]=r;return null;}});
  check(controller.intercept("https://example.invalid/p","POST","X-Test: value\r\nAccept: text/plain\r\n")==null,"null client result preserves Chromium request");
  check(seen[0].getMethod().equals("POST")&&seen[0].getUrl().toString().equals("https://example.invalid/p"),"method/url preserved");
  check(!seen[0].isForMainFrame()&&!seen[0].hasGesture()&&!seen[0].isRedirect(),"worker request metadata");
  check(seen[0].getRequestHeaders().get("X-Test").equals("value"),"headers preserved");
  throwsType(UnsupportedOperationException.class,()->seen[0].getRequestHeaders().put("X","bad"),"request headers immutable");
  throwsType(UnsupportedOperationException.class,()->settings.setBlockNetworkLoads(true),"unsupported network cancellation does not succeed");
  check(!settings.getBlockNetworkLoads(),"failed block request leaves current state intact");
  android.content.Context offlineApp=new android.content.Context();offlineApp.internet=false;
  GesherServiceWorkers offline=new GesherServiceWorkers(offlineApp);
  throwsType(SecurityException.class,()->offline.intercept("HTTPS://example.invalid/a","GET",null),"case-insensitive INTERNET enforcement");
  final Stream intercepted=new Stream(new byte[]{1,2,3});
  controller.setServiceWorkerClient(new ServiceWorkerClient(){public WebResourceResponse shouldInterceptRequest(WebResourceRequest r){return new WebResourceResponse("application/test","utf-8",207,"Multi",Map.of("X-Source","app"),intercepted);}});
  check(GesherServiceWorkers.dispatch(1,"https://example.invalid/a","GET",null),"request accepted asynchronously");
  WebView2Host.Result r=response(1);
  check(r.status()==207&&Arrays.equals(r.body(),new byte[]{1,2,3}),"app status/body preserved");
  check(r.headers().contains("Content-Type: application/test; charset=utf-8")&&r.headers().contains("X-Source: app"),"response metadata");
  check(intercepted.closed,"response stream closed");
  controller.setServiceWorkerClient(null);settings.setBlockNetworkLoads(false);
  check(GesherServiceWorkers.dispatch(2,"https://example.invalid/a","GET",null),"cleared client accepted");
  check(response(2).status()==0,"null client actually resumes original request");
  instance.set(null,offline);
  GesherServiceWorkers.dispatch(3,"https://example.invalid/a","GET",null);
  check(response(3).status()<0,"missing INTERNET is never released to network");instance.set(null,controller);
  offline.setServiceWorkerClient(new ServiceWorkerClient(){public WebResourceResponse shouldInterceptRequest(WebResourceRequest r){return new WebResourceResponse("text/plain",null,new ByteArrayInputStream(new byte[]{6}));}});
  check(offline.intercept("https://example.invalid/a","GET",null).getData().read()==6,"offline client response does not require INTERNET");
  controller.setServiceWorkerClient(new ServiceWorkerClient(){public WebResourceResponse shouldInterceptRequest(WebResourceRequest r){throw new IllegalStateException("app failure");}});
  GesherServiceWorkers.dispatch(4,"https://example.invalid/a","GET",null);check(response(4).status()<0,"callback failure stays denied");
  Stream badHeaders=new Stream(new byte[]{1});
  controller.setServiceWorkerClient(new ServiceWorkerClient(){public WebResourceResponse shouldInterceptRequest(WebResourceRequest r){return new WebResourceResponse("text/plain",null,200,"OK",Map.of("X","bad\r\nInjected: yes"),badHeaders);}});
  GesherServiceWorkers.dispatch(5,"https://example.invalid/a","GET",null);check(response(5).status()<0&&badHeaders.closed,"invalid headers denied and stream released");
  controller.setServiceWorkerClient(null);settings.setAllowFileAccess(false);
  controller.intercept("https://gesher-file.example/private.txt","GET",null);check(!FileUrls.lastAllowed,"mapped file permission enforced");
  settings.setAllowFileAccess(true);controller.intercept("https://gesher-file.example/private.txt","GET",null);check(FileUrls.lastAllowed,"mapped file setting reaches loader");
  settings.setAllowContentAccess(false);
  throwsType(SecurityException.class,()->controller.intercept("content://provider/a","GET",null),"content denied before resolver");check(app.resolver.reads==0,"denied content not opened");
  settings.setAllowContentAccess(true);check(controller.intercept("content://provider/a","GET",null).getData().read()==7&&app.resolver.reads==1,"real resolver boundary used");
  SupportLib.ServiceWorkers adapter=new SupportLib.ServiceWorkers(controller);
  InvocationHandler settingsHandler=adapter.getServiceWorkerWebSettings();
  Method content=ServiceWorkerWebSettings.class.getMethod("setAllowContentAccess",boolean.class);
  try{settingsHandler.invoke(null,content,new Object[]{false});}catch(Throwable e){throw new AssertionError(e);}
  check(!settings.getAllowContentAccess(),"isolated AndroidX adapter shares its prepared controller");
  SupportLib.Converter converter=new SupportLib.Converter();
  InvocationHandler converted=converter.convertServiceWorkerSettings((Object)settings);
  check(converter.convertServiceWorkerSettings(converted)==settings,"AndroidX settings converter preserves exact object");
  throwsType(IllegalArgumentException.class,()->converter.convertServiceWorkerSettings(new Object()),"converter rejects other object types");
  throwsType(IllegalArgumentException.class,()->converter.convertServiceWorkerSettings((InvocationHandler)(proxy,m,a)->null),"converter rejects foreign settings handlers");
  adapter.setServiceWorkerClient((proxy,m,a)->new WebResourceResponse("text/plain",null,new ByteArrayInputStream(new byte[]{9})));
  check(controller.intercept("https://example.invalid/a","GET",null).getData().read()==9,"AndroidX client dispatched");
  adapter.setServiceWorkerClient(null);settings.setBlockNetworkLoads(false);
  check(controller.intercept("https://example.invalid/a","GET",null)==null,"AndroidX null removes client");
  instance.set(null,null);
  check(WebView2Host.enables==0,"prepared fixture never activates a native worker filter");
  final long[] value={-1};final int[] calls={0};
  WebStorageQueries.query("https://storage.invalid",false,v->{value[0]=v;calls[0]++;});
  check(value[0]==-1&&WebView2Host.origin.equals("https://storage.invalid"),"asynchronous native query forwarding");
  WebView2Host.pending.onResult(3,0,"{\"usage\":1234,\"quota\":99999}");
  check(value[0]==1234&&calls[0]==1,"actual usage delivered");WebView2Host.pending.onResult(3,0,"{\"usage\":9}");check(calls[0]==1,"duplicate native callback ignored");
  WebStorageQueries.query("https://storage.invalid",true,v->value[0]=v);WebView2Host.pending.onResult(3,0,"{\"usage\":1234,\"quota\":99999}");check(value[0]==99999,"actual quota not invented");
  WebStorageQueries.query("https://storage.invalid",false,v->value[0]=v);WebView2Host.pending.onResult(3,-1,"{\"usage\":1234}");check(value[0]==0,"backend error returns Android zero failure value");
  WebView2Host.accepted=false;WebStorageQueries.query("bad",false,v->value[0]=v);check(value[0]==0,"rejected admission completes callback");
  int before=WebView2Host.queries;WebStorageQueries.query("https://x.invalid",false,null);check(WebView2Host.queries==before,"null callback creates no native work");
  for(String json:new String[]{"{}","{\"usage\":-1}","{\"usage\":1.5}","{\"usage\":\"1234\"}","{\"usage\":1e30}","{\"usage\":null}"})check(WebStorageQueries.measurement(json,false)==0,"invalid measurement rejected: "+json);
  check(WebStorageQueries.measurement("{\"usage\":9223372036854775807}",false)==Long.MAX_VALUE,"integer quota values retain long precision");
  GesherWebView.fail=true;value[0]=-1;WebStorageQueries.query("https://storage.invalid",false,v->value[0]=v);GesherWebView.fail=false;
  check(value[0]==0,"configuration failure still completes once");
  check(android.os.Handler.posts>=4,"callback handed to calling Looper");
  System.out.println("Isolated prepared worker internals and WebStorage query contracts: "+checks+" checks passed");
 }
}
'''


def compile_and_run(files, classes, args, expected_failure=None):
    with tempfile.TemporaryDirectory(prefix='gesher-worker-contracts-') as tmp:
        work=Path(tmp)
        for name, text in files.items():
            path=work/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
        classpath = str(work) + (os.pathsep + str(args.core_classes.resolve()) if args.core_classes else '')
        subprocess.run([str(args.java_home/'bin/javac'), '-encoding', 'UTF-8', '-cp', classpath, '-d', str(work),
                        *(str(work/name) for name in files)], check=True,timeout=60)
        for name in classes:
            result = subprocess.run([str(args.java_home/'bin/java'),'-XX:-UsePerfData','-Xmx128m','-cp',classpath,
                        'com.gesher.app.webview.' + name], capture_output=True, text=True, timeout=20)
            if expected_failure:
                if result.returncode != 1 or expected_failure not in result.stderr:
                    raise RuntimeError('Worker gate negative control failed for the wrong reason: '
                                       + result.stdout + result.stderr)
                print('Worker gate negative control rejected:', args.negative_control)
            else:
                print(result.stdout, end='');print(result.stderr, end='');result.check_returncode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', required=True, type=Path)
    parser.add_argument('--core-classes', type=Path, help='Optional actual Android org.json classes')
    parser.add_argument('--negative-control', choices=('feature-query', 'controller'),
                        help='Inject an activation into one actual entry method; require rejection')
    args = parser.parse_args()
    source = (ROOT / 'app/java/com/gesher/app/webview/SupportLib.java').read_text()
    fields = source[source.index('    static final String[] FEATURES = {'):source.index('\n    };', source.index('    static final String[] FEATURES = {')) + 7]
    factory = method(source, 'public static final class Factory')
    feature_query = method(factory, 'public String[] getSupportedFeatures()')
    if args.negative_control == 'feature-query':
        # A recorded, usable callee makes an accidental lookup observable even
        # while the real controller is independently unavailable.
        guard = 'return FEATURES.clone();'
        if feature_query.count(guard) != 1:
            raise ValueError('Feature gate changed; update the negative control')
        feature_query = feature_query.replace(guard, 'GesherServiceWorkers.get();\n' + guard)
    if args.negative_control != 'controller':
        feature_files = {
            'com/gesher/app/webview/SupportLib.java': '''package com.gesher.app.webview;
import java.util.*;
final class SupportLib {''' + fields + '\nstatic final class Factory {' + feature_query + '}}',
            'com/gesher/app/webview/GesherServiceWorkers.java': '''package com.gesher.app.webview;
// Recording callee, deliberately usable: a downstream throw must not mask a
// feature query trying to initialize the worker controller.
final class GesherServiceWorkers {
 static int lookups; static boolean available=true;
 static GesherServiceWorkers get(){lookups++;if(!available)throw new UnsupportedOperationException();return new GesherServiceWorkers();}
}''',
            'com/gesher/app/webview/WorkerFeaturesHost.java':
                (ROOT / 'tests/webview/worker_features_host.java.in').read_text(),
        }
        compile_and_run(feature_files, ['WorkerFeaturesHost'], args,
                        'feature query must not initialize workers' if args.negative_control else None)
        if args.negative_control:
            return
    adapters = '''package com.gesher.app.webview;
import java.lang.reflect.*;import java.util.concurrent.*;import android.webkit.*;
public final class SupportLib { private static final String BOUNDARY="org.chromium.support_lib_boundary.";
''' + fields + '\n' + '\n'.join(method(source, signature) for signature in [
        'static final class Boundary implements InvocationHandler', 'static Object call(',
        'public static final class ServiceWorkers', 'public static final class WorkerSettings'])
    adapters += '\npublic static final class Converter {' + method(source, 'public InvocationHandler convertServiceWorkerSettings(Object settings)') + method(source, 'public Object convertServiceWorkerSettings(InvocationHandler settings)') + '}\n'
    adapters += '\npublic static final class Factory {' + feature_query + method(factory, 'public InvocationHandler getServiceWorkerController()') + '}\n}'
    files = dict(STUBS)
    if args.core_classes: del files['org/json/JSONObject.java']
    files['com/gesher/app/webview/SupportLib.java'] = adapters
    for name in ('GesherServiceWorkers.java', 'WebStorageQueries.java', 'GesherWebStorage.java'):
        files['com/gesher/app/webview/' + name] = (ROOT / 'app/java/com/gesher/app/webview' / name).read_text()
    provider = (ROOT / 'app/java/com/gesher/app/webview/GesherWebViewFactory.java').read_text()
    files['com/gesher/app/webview/GesherWebViewFactory.java'] = '''package com.gesher.app.webview;
import android.webkit.*;
// The two public provider methods are exact production extractions. Unrelated
// provider construction is omitted; controller/storage behavior is not stubbed.
final class GesherWebViewFactory {
 private final Object mLock=new Object();
 private ServiceWorkerController mServiceWorkers;
 private GesherWebStorage mStorage;
''' + method(provider, 'public ServiceWorkerController getServiceWorkerController()') + method(provider, 'public WebStorage getWebStorage()') + '}\n'
    if args.negative_control == 'controller':
        path = 'com/gesher/app/webview/GesherServiceWorkers.java'
        original = method(files[path], 'static GesherServiceWorkers get()')
        opening = original.index('{') + 1
        # Keep the unsupported exception: this control catches activation before
        # failure, rather than merely detecting that a controller was returned.
        changed = original[:opening] + '\nWebView2Host.enableServiceWorkers((type, hr, value) -> {});' + original[opening:]
        files[path] = files[path].replace(original, changed)
    files['com/gesher/app/webview/WorkerGateHost.java'] = (ROOT / 'tests/webview/worker_gate_host.java.in').read_text()
    files['com/gesher/app/webview/WorkerContractHost.java'] = TEST
    compile_and_run(files, ['WorkerGateHost'] if args.negative_control else ['WorkerGateHost', 'WorkerContractHost'],
                    args, 'unavailable controller must not enable a native filter' if args.negative_control else None)

if __name__=='__main__': main()
