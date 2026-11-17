#!/usr/bin/env python3
"""Run the complete production JavaBidirectionalStream with explicit host stubs.

The network, executor and framework boundary classes below are TEST FIXTURES.
This checks response presence and terminal lifecycle, NOT actual HTTP/2, ART,
Windows integration, scheduling races, performance, or full API conformance.
Usage: python test_pr87_response_presence.py --source <JavaBidirectionalStream.java>
Requires a JDK on PATH; no network access, repository modification or dependencies.
"""
from __future__ import annotations
import argparse
import hashlib
import shutil
import subprocess
import tempfile
from pathlib import Path

STUBS = {
    'android/net/Network.java': 'package android.net; public class Network {}\n',
    'android/net/http/HeaderBlock.java': '''package android.net.http;
import java.util.*; public abstract class HeaderBlock {
 public abstract List<Map.Entry<String,String>> getAsList();
}''',
    'android/net/http/UrlResponseInfo.java': '''package android.net.http;
import java.util.*; public abstract class UrlResponseInfo {
 public abstract HeaderBlock getHeaders(); public abstract int getHttpStatusCode();
 public abstract String getHttpStatusText(); public abstract String getNegotiatedProtocol();
 public abstract long getReceivedByteCount(); public abstract String getUrl();
 public abstract List<String> getUrlChain(); public abstract boolean wasCached();
}''',
    'android/net/http/HttpException.java': '''package android.net.http;
public class HttpException extends Exception { public HttpException(String m,Throwable t){super(m,t);} }
''',
    'android/net/http/CallbackException.java': '''package android.net.http;
public class CallbackException extends HttpException {public CallbackException(String m,Throwable t){super(m,t);}}
''',
    'android/net/http/NetworkException.java': '''package android.net.http;
public abstract class NetworkException extends HttpException {
 public static final int ERROR_OTHER=11; public NetworkException(String m,Throwable t){super(m,t);}
 public abstract int getErrorCode(); public abstract boolean isImmediatelyRetryable();
}''',
    'android/net/http/BidirectionalStream.java': '''package android.net.http;
import java.nio.ByteBuffer;
public abstract class BidirectionalStream {
 public static final int STREAM_PRIORITY_MEDIUM=2;
 public abstract HeaderBlock getHeaders(); public abstract String getHttpMethod();
 public abstract int getPriority(); public abstract int getTrafficStatsTag();
 public abstract int getTrafficStatsUid(); public abstract boolean hasTrafficStatsTag();
 public abstract boolean hasTrafficStatsUid();
 public abstract boolean isDelayRequestHeadersUntilFirstFlushEnabled();
 public abstract boolean isDone(); public abstract void start(); public abstract void cancel();
 public abstract void write(ByteBuffer b,boolean end); public abstract void flush();
 public abstract void read(ByteBuffer b);
 public abstract static class Builder {
  public abstract Builder addHeader(String n,String v); public abstract Builder setHttpMethod(String v);
  public abstract Builder setDelayRequestHeadersUntilFirstFlushEnabled(boolean v);
  public abstract Builder setPriority(int v); public abstract Builder setTrafficStatsTag(int v);
  public abstract Builder setTrafficStatsUid(int v); public abstract BidirectionalStream build();
 }
 public abstract static class Callback {
  public abstract void onStreamReady(BidirectionalStream s);
  public abstract void onResponseHeadersReceived(BidirectionalStream s,UrlResponseInfo i);
  public abstract void onReadCompleted(BidirectionalStream s,UrlResponseInfo i,ByteBuffer b,boolean end);
  public abstract void onWriteCompleted(BidirectionalStream s,UrlResponseInfo i,ByteBuffer b,boolean end);
  public abstract void onResponseTrailersReceived(BidirectionalStream s,UrlResponseInfo i,HeaderBlock t);
  public abstract void onSucceeded(BidirectionalStream s,UrlResponseInfo i);
  public abstract void onFailed(BidirectionalStream s,UrlResponseInfo i,HttpException e);
  public abstract void onCanceled(BidirectionalStream s,UrlResponseInfo i);
 }
}''',
    'android/net/http/JavaHttpEngine.java': '''package android.net.http;
import java.util.*; import java.util.concurrent.*; import java.net.*;
final class JavaHttpEngine {
 final Executor io; final boolean http2Enabled; final String agent="test";
 int active,releases;
 JavaHttpEngine(boolean h,Executor e){http2Enabled=h;io=e;}
 void register(Object o){active++;} void release(Object o){active--;releases++;} void checkOpen(){}
 static URL url(String s){try{return new URL(s);}catch(Exception e){throw new IllegalArgumentException(e);}}
 static String token(String s){return s;}
 static void header(List<Map.Entry<String,String>> h,String n,String v){h.add(Map.entry(n,v));}
 static final class Headers extends HeaderBlock {
  final List<Map.Entry<String,String>> items;
  Headers(List<Map.Entry<String,String>> h){items=List.copyOf(h);}
  public List<Map.Entry<String,String>> getAsList(){return items;}
 }
 // Deliberately a scheduling boundary stub, not a concurrency/race test.
 static final class SerialExecutor implements Executor {
  final Executor executor; SerialExecutor(Executor e){executor=e;}
  public void execute(Runnable r){executor.execute(r);}
 }
}''',
    'android/net/http/Http2Transport.java': '''package android.net.http;
import android.net.Network; import java.net.URL; import java.io.*; import java.util.*;
final class Http2Transport {
 static int connects,starts,closes; static boolean connectFails,readFails;
 static void reset(){connects=starts=closes=0;connectFails=readFails=false;}
 void connect(URL u,Network n)throws IOException{connects++;if(connectFails)throw new IOException("fixture connect failure");}
 void close(){closes++;}
 void start(URL u,String m,List<Map.Entry<String,String>> h,String a,boolean b){starts++;}
 List<Map.Entry<String,String>> headers(){return List.of(Map.entry(":status","200"));}
 void setResponseEncoding(String s){} void write(byte[] b){} void flush(boolean end){}
 byte[] read(int n)throws IOException{if(readFails)throw new IOException("fixture read failure");return null;}
}''',
    'android/net/http/ResponsePresenceTest.java': '''package android.net.http;
import java.net.ProtocolException; import java.nio.*; import java.util.*; import java.util.concurrent.*;
public final class ResponsePresenceTest {
 static int checks, failures;
 static void check(String name,boolean ok){checks++;if(!ok)failures++;System.out.println((ok?"PASS ":"FAIL ")+name);}
 static class Manual implements Executor {
  final ArrayDeque<Runnable> tasks=new ArrayDeque<>();
  public void execute(Runnable r){tasks.add(r);}
  void one(){if(tasks.isEmpty())throw new AssertionError("No queued task");tasks.remove().run();}
  void drain(){int count=0;while(!tasks.isEmpty()){if(++count>30)throw new AssertionError("Unbounded fixture tasks");one();}}
 }
 static class Recorder extends BidirectionalStream.Callback {
  int ready,failed,canceled,succeeded,headers,written; UrlResponseInfo terminal,writeInfo,response; HttpException error;
  boolean cancelOnHeaders;
  public void onStreamReady(BidirectionalStream s){ready++;}
  public void onResponseHeadersReceived(BidirectionalStream s,UrlResponseInfo i){headers++;response=i;if(cancelOnHeaders)s.cancel();}
  public void onReadCompleted(BidirectionalStream s,UrlResponseInfo i,ByteBuffer b,boolean end){}
  public void onWriteCompleted(BidirectionalStream s,UrlResponseInfo i,ByteBuffer b,boolean end){written++;writeInfo=i;}
  public void onResponseTrailersReceived(BidirectionalStream s,UrlResponseInfo i,HeaderBlock t){}
  public void onSucceeded(BidirectionalStream s,UrlResponseInfo i){succeeded++;terminal=i;}
  public void onFailed(BidirectionalStream s,UrlResponseInfo i,HttpException e){failed++;terminal=i;error=e;}
  public void onCanceled(BidirectionalStream s,UrlResponseInfo i){canceled++;terminal=i;}
 }
 static JavaBidirectionalStream stream(JavaHttpEngine e,Recorder r){return new JavaBidirectionalStream.Builder(e,"http://127.0.0.1/fixture",Runnable::run,r,null).build() instanceof JavaBidirectionalStream s ? s : null;}
 public static void main(String[] args){
  Http2Transport.reset(); JavaHttpEngine e=new JavaHttpEngine(false,Runnable::run); Recorder r=new Recorder();
  JavaBidirectionalStream s=stream(e,r); s.start();
  check("disabled: one failure, no ready/success",r.failed==1 && r.ready==0 && r.succeeded==0);
  check("disabled: no invented response",r.terminal==null);
  check("disabled: protocol failure retained",r.error!=null && r.error.getCause() instanceof ProtocolException);
  check("disabled: transport never opened",Http2Transport.connects==0 && Http2Transport.starts==0);
  check("disabled: engine released exactly once",s.isDone() && e.active==0 && e.releases==1);
  s.cancel();check("disabled: later cancel is silent",r.canceled==0 && r.failed==1 && e.releases==1);

  Http2Transport.reset(); Http2Transport.connectFails=true; e=new JavaHttpEngine(true,Runnable::run);r=new Recorder();s=stream(e,r);s.start();
  check("connect failure: no invented response",r.failed==1 && r.terminal==null);
  check("connect failure: actual attempt and release",Http2Transport.connects==1 && e.releases==1 && e.active==0);

  Http2Transport.reset(); Manual queue=new Manual(); e=new JavaHttpEngine(true,queue);r=new Recorder();s=stream(e,r);
  s.cancel();check("cancel before start: no terminal callback",r.canceled==0 && r.failed==0 && !s.isDone());
  s.start();s.cancel();queue.drain();
  check("cancel before IO: null response",r.canceled==1 && r.terminal==null);
  check("cancel before IO: no connection",Http2Transport.connects==0 && e.active==0 && e.releases==1);

  Http2Transport.reset();queue=new Manual();e=new JavaHttpEngine(true,queue);r=new Recorder();s=stream(e,r);
  // Keep response-header work queued until the outgoing write callback has run.
  JavaBidirectionalStream.Builder b=new JavaBidirectionalStream.Builder(e,"http://127.0.0.1/fixture",Runnable::run,r,null);
  b.setDelayRequestHeadersUntilFirstFlushEnabled(true);s=(JavaBidirectionalStream)b.build();s.start();queue.one();
  ByteBuffer payload=ByteBuffer.allocateDirect(1);payload.put((byte)7).flip();s.write(payload,true);s.flush();queue.one();
  check("write before response: callback has null info",r.written==1 && r.writeInfo==null && r.headers==0);
  queue.drain();
  check("headers: real status retained",r.headers==1 && r.response!=null && r.response.getHttpStatusCode()==200);
  check("success: same received response, h2",r.succeeded==1 && r.terminal==r.response && r.terminal.getNegotiatedProtocol().equals("h2"));
  check("success: engine released once",e.releases==1 && e.active==0);

  Http2Transport.reset();Http2Transport.readFails=true;e=new JavaHttpEngine(true,Runnable::run);r=new Recorder();s=stream(e,r);s.start();
  check("failure after headers: preserve real response",r.failed==1 && r.headers==1 && r.terminal==r.response && r.terminal!=null);

  Http2Transport.reset();e=new JavaHttpEngine(true,Runnable::run);r=new Recorder();r.cancelOnHeaders=true;s=stream(e,r);s.start();
  check("cancel after headers: preserve real response",r.canceled==1 && r.headers==1 && r.terminal==r.response && r.terminal!=null);
  check("cancel after headers: no later success/failure",r.succeeded==0 && r.failed==0 && e.active==0 && e.releases==1);
  System.out.println("RESULT "+checks+" checks, "+failures+" failures; host stubs, not Windows/ART");
  if(failures!=0)System.exit(1);
 }
}'''
}

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    args=parser.parse_args()
    data=args.source.read_bytes()
    blob=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
    print('Production source Git blob:',blob,flush=True)
    javac=shutil.which('javac'); java=shutil.which('java')
    if not javac or not java:
        parser.error('JDK 17 or later must provide javac and java on PATH')
    with tempfile.TemporaryDirectory(prefix='gesher-pr87-qa-') as tmp:
        work=Path(tmp)
        for relative,content in STUBS.items():
            file=work/relative; file.parent.mkdir(parents=True,exist_ok=True);file.write_text(content,encoding='utf-8')
        production=work/'android/net/http/JavaBidirectionalStream.java';production.write_bytes(data)
        out=work/'classes';out.mkdir()
        subprocess.run([javac,'-d',str(out),*[str(p) for p in work.rglob('*.java')]],check=True,timeout=40)
        return subprocess.run([java,'-cp',str(out),'android.net.http.ResponsePresenceTest'],timeout=10).returncode

if __name__=='__main__':
    raise SystemExit(main())
