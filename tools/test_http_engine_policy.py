#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run the HTTP engine's real OkHttp protocol/header contracts on a host JVM.

The pinned HTTP provider, connection pool, framing and engine are production
sources. Context, Network, Android socket tagging and Platform policy are explicit
host boundaries. Windows runs the same public fixture with real platform classes.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--java-home',type=Path,required=True)
parser.add_argument('--okhttp-source',type=Path,required=True)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
temporary=tempfile.TemporaryDirectory(prefix='gesher-http-policy-')
out=Path(temporary.name)
aosp=args.okhttp_source/'repackaged'
jdk=args.java_home/'bin'
classes=out/'classes'; classes.mkdir(parents=True,exist_ok=True)
stubs={
'android/content/Context.java':'package android.content; public class Context {}',
'android/content/ContextWrapper.java':'package android.content; public class ContextWrapper extends Context {public ContextWrapper(Context c){}}',
'android/net/Network.java':'''package android.net; public class Network {
public java.net.URLConnection openConnection(java.net.URL u) throws java.io.IOException{return u.openConnection();}
public javax.net.SocketFactory getSocketFactory(){return javax.net.SocketFactory.getDefault();}
public java.net.InetAddress getByName(String name)throws java.net.UnknownHostException{return java.net.InetAddress.getByName(name);}}''',
'android/security/NetworkSecurityPolicy.java':'''package android.security; public class NetworkSecurityPolicy {
public static NetworkSecurityPolicy getInstance(){return new NetworkSecurityPolicy();}
public boolean isCleartextTrafficPermitted(String host){return true;}}''',
'android/compat/annotation/UnsupportedAppUsage.java':'package android.compat.annotation; public @interface UnsupportedAppUsage {}',
'com/android/okhttp/internal/Platform.java':'''package com.android.okhttp.internal;
import java.net.*; import java.io.*; import java.util.*; import javax.net.ssl.*;
import com.android.okhttp.Protocol; import com.android.okhttp.internal.tls.*;
// Explicit host platform boundary. HTTP connection, headers, pooling, framing,
// TLS negotiation and socket transport remain pinned production OkHttp.
public class Platform {
 private static final Platform INSTANCE=new Platform();
 public static Platform get(){return INSTANCE;}
 public String getPrefix(){return "X-Android";}
 public void logW(String text){System.err.println(text);}
 public void tagSocket(Socket socket){} public void untagSocket(Socket socket){}
 public void connectSocket(Socket socket,InetSocketAddress address,int timeout)throws IOException{socket.connect(address,timeout);}
 public void configureTlsExtensions(SSLSocket socket,String host,List<Protocol> protocols){
  SSLParameters p=socket.getSSLParameters();
  p.setApplicationProtocols(protocols.stream().map(Object::toString).toArray(String[]::new));
  if(host!=null)try{p.setServerNames(Collections.singletonList(new SNIHostName(host)));}catch(IllegalArgumentException ignored){}
  socket.setSSLParameters(p);
 }
 public void afterHandshake(SSLSocket socket){}
 public String getSelectedProtocol(SSLSocket socket){return socket.getApplicationProtocol();}
 public X509TrustManager trustManager(SSLSocketFactory factory){
  try {TrustManagerFactory f=TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
   f.init((java.security.KeyStore)null); for(TrustManager m:f.getTrustManagers())if(m instanceof X509TrustManager)return (X509TrustManager)m;
  }catch(Exception e){throw new IllegalStateException(e);}throw new IllegalStateException("No X509 trust manager");
 }
 public TrustRootIndex trustRootIndex(X509TrustManager trust){return new RealTrustRootIndex(trust.getAcceptedIssuers());}
}'''}
sources=[]
for name,content in stubs.items():
 p=out/'source'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content);sources.append(p)
for part in ['okhttp','okhttp-urlconnection','okio/okio']:
 sources+=sorted((aosp/part/'src/main/java').rglob('*.java'))
sources+=[aosp/'android/src/main/java/com/android/okhttp/internal/Version.java']
sources+=sorted((root/'app/java/android/net/http').glob('*.java'))
sources+=[root/'tests/net/HttpEngineProtocolPolicyTest.java']
args=[str(jdk/'javac'),'--release','17','-encoding','UTF-8','-d',str(classes),*map(str,sources)]
subprocess.run(args,check=True,timeout=60)
subprocess.run([str(jdk/'java'),'-cp',str(classes),'HttpEngineProtocolPolicyTest','--host'],check=True,timeout=35)
temporary.cleanup()
