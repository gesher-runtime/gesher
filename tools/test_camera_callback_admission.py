#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run whole production CameraDeviceUser/CameraCallbacks, with explicit host device seams.

Only Android framework value objects, native metadata sizing and physical capture
are adapted. Request admission/selection, locks, queues, callback dispatch,
flush/disconnect, credit accounting and producer lifecycle are production code.
The Windows camera-admission gate additionally uses real ART metadata allocations.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
'android/hardware/ICameraService.java': '''package android.hardware; public interface ICameraService {
 int ERROR_ILLEGAL_ARGUMENT=3, ERROR_DISCONNECTED=4, ERROR_INVALID_OPERATION=10, ERROR_MAX_CAMERAS_IN_USE=8; }''',
'android/os/IBinder.java': 'package android.os; public interface IBinder {}',
'android/os/RemoteException.java': 'package android.os; public class RemoteException extends Exception {}',
'android/os/ServiceSpecificException.java': '''package android.os; public class ServiceSpecificException extends RuntimeException {
 public final int errorCode; public ServiceSpecificException(int c,String m){super(m);errorCode=c;} }''',
'android/os/SystemClock.java': '''package android.os; public class SystemClock {
 public static long uptimeMillis(){return System.nanoTime()/1000000;} public static long elapsedRealtime(){return uptimeMillis();}
 public static long elapsedRealtimeNanos(){return System.nanoTime();} }''',
'android/util/Size.java': '''package android.util; public class Size {
 final int w,h; public Size(int w,int h){this.w=w;this.h=h;} public int getWidth(){return w;} public int getHeight(){return h;}
 public boolean equals(Object o){return o instanceof Size&&((Size)o).w==w&&((Size)o).h==h;} public int hashCode(){return w*31+h;} }''',
'android/view/Surface.java': 'package android.view; public class Surface {}',
'android/hardware/common/fmq/MQDescriptor.java': 'package android.hardware.common.fmq; public class MQDescriptor {}',
'android/hardware/camera2/params/SessionConfiguration.java': 'package android.hardware.camera2.params; public class SessionConfiguration {}',
'android/hardware/camera2/params/OutputConfiguration.java': '''package android.hardware.camera2.params; import android.view.Surface;
 public class OutputConfiguration { public java.util.List<Surface> getSurfaces(){return java.util.List.of(new Surface());} }''',
'android/hardware/camera2/utils/SurfaceUtils.java': '''package android.hardware.camera2.utils; import android.view.Surface; import android.util.Size;
 public class SurfaceUtils {public static int getSurfaceFormat(Surface s){return 1;} public static Size getSurfaceSize(Surface s){return new Size(16,16);} }''',
'android/hardware/camera2/utils/SubmitInfo.java': '''package android.hardware.camera2.utils; public class SubmitInfo {
 final int id; final long last; public SubmitInfo(int id,long last){this.id=id;this.last=last;}
 public int getRequestId(){return id;} public long getLastFrameNumber(){return last;} }''',
'android/hardware/camera2/CameraMetadata.java': '''package android.hardware.camera2; public class CameraMetadata {
 public static final int CONTROL_AE_STATE_CONVERGED=2,CONTROL_AF_STATE_INACTIVE=0,CONTROL_AWB_STATE_CONVERGED=2,FLASH_STATE_UNAVAILABLE=0,LENS_STATE_STATIONARY=0; }''',
'android/hardware/camera2/CaptureResult.java': '''package android.hardware.camera2; public class CaptureResult {
 public static final Object SENSOR_TIMESTAMP=new Object(),SENSOR_EXPOSURE_TIME=new Object(),SENSOR_FRAME_DURATION=new Object(),REQUEST_PIPELINE_DEPTH=new Object(),CONTROL_AE_STATE=new Object(),CONTROL_AF_STATE=new Object(),CONTROL_AWB_STATE=new Object(),FLASH_STATE=new Object(),LENS_STATE=new Object(); }''',
'android/hardware/camera2/CaptureRequest.java': '''package android.hardware.camera2; import android.hardware.camera2.impl.CameraMetadataNative;
 public class CaptureRequest { public static final Object JPEG_QUALITY=new Object(); public long bytes=128; public int[] streams=new int[0],surfaces=new int[0];
 public CameraMetadataNative getNativeCopy(){CameraMetadataNative.copies++;return new CameraMetadataNative(bytes);}
 public int[] getStreamIds(){return streams;} public int[] getSurfaceIds(){return surfaces;} }''',
'android/hardware/camera2/impl/CameraMetadataNative.java': '''package android.hardware.camera2.impl;
 public class CameraMetadataNative { public static int copies; public long bytes=128;
 public CameraMetadataNative(){} public CameraMetadataNative(long n){bytes=n;} public CameraMetadataNative(CameraMetadataNative m){bytes=m.bytes;}
 public long getMetadataPtr(){return bytes;} public <T> T get(Object k){return null;} public <T> void set(Object k,T value){} }''',
'android/hardware/camera2/CameraMetadataInfo.java': '''package android.hardware.camera2; import android.hardware.camera2.impl.CameraMetadataNative;
 public class CameraMetadataInfo { public final CameraMetadataNative value; CameraMetadataInfo(CameraMetadataNative m){value=m;}
 public static CameraMetadataInfo metadata(CameraMetadataNative m){return new CameraMetadataInfo(m);} }''',
'android/hardware/camera2/impl/PhysicalCaptureResultInfo.java': 'package android.hardware.camera2.impl; public class PhysicalCaptureResultInfo {}',
'android/hardware/camera2/impl/CaptureResultExtras.java': '''package android.hardware.camera2.impl; public class CaptureResultExtras {
 public final int request,subsequence,stream; public final long frame;
 public CaptureResultExtras(int r,int s,int a,int p,long f,int c,int e,String id,long l,long q,long z,boolean b,long t){request=r;subsequence=s;frame=f;stream=e;}
 public int getRequestId(){return request;} public long getFrameNumber(){return frame;} public int getSubsequenceId(){return subsequence;} }''',
'android/hardware/camera2/ICameraDeviceCallbacks.java': '''package android.hardware.camera2;
 import android.os.*; import android.hardware.camera2.impl.*;
 public interface ICameraDeviceCallbacks {
 int ERROR_CAMERA_DISCONNECTED=0,ERROR_CAMERA_DEVICE=1,ERROR_CAMERA_REQUEST=3,ERROR_CAMERA_RESULT=4,ERROR_CAMERA_BUFFER=5;
 void onDeviceError(int e,CaptureResultExtras x)throws RemoteException; void onDeviceIdle()throws RemoteException;
 void onCaptureStarted(CaptureResultExtras x,long t)throws RemoteException;
 void onResultReceived(CameraMetadataInfo m,CaptureResultExtras x,PhysicalCaptureResultInfo[] p)throws RemoteException;
 void onPrepared(int s)throws RemoteException; void onRepeatingRequestError(long l,int r)throws RemoteException;
 IBinder asBinder(); void onRequestQueueEmpty()throws RemoteException; void onClientSharedAccessPriorityChanged(boolean p)throws RemoteException;
 class Default implements ICameraDeviceCallbacks {
 public void onDeviceError(int e,CaptureResultExtras x)throws RemoteException{} public void onDeviceIdle()throws RemoteException{}
 public void onCaptureStarted(CaptureResultExtras x,long t)throws RemoteException{}
 public void onResultReceived(CameraMetadataInfo m,CaptureResultExtras x,PhysicalCaptureResultInfo[] p)throws RemoteException{}
 public void onPrepared(int s)throws RemoteException{} public void onRepeatingRequestError(long l,int r)throws RemoteException{}
 public IBinder asBinder(){return null;} public void onRequestQueueEmpty(){} public void onClientSharedAccessPriorityChanged(boolean p){}
 } }''',
'android/hardware/camera2/ICameraDeviceUser.java': '''package android.hardware.camera2;
 import android.hardware.camera2.impl.*;import android.hardware.camera2.params.*;import android.hardware.camera2.utils.*;import android.hardware.common.fmq.*;
 public interface ICameraDeviceUser {int NORMAL_MODE=0,AUDIO_RESTRICTION_NONE=0;long NO_IN_FLIGHT_REPEATING_FRAMES=-1;
 class Default implements ICameraDeviceUser {
 public void beginConfigure(){} public int[] endConfigure(int m,CameraMetadataNative p,long t){return null;}
 public boolean isSessionConfigurationSupported(SessionConfiguration s){return false;}
 public int createStream(OutputConfiguration c){return 0;} public void deleteStream(int i){}
 public void updateOutputConfiguration(int i,OutputConfiguration c){} public void finalizeOutputConfigurations(int i,OutputConfiguration c){}
 public int createInputStream(int w,int h,int f,boolean m){return 0;} public void prepare(int i){} public void prepare2(int n,int i){} public void tearDown(int i){}
 public CameraMetadataNative createDefaultRequest(int t){return null;} public CameraMetadataNative getCameraInfo(){return null;}
 public MQDescriptor getCaptureResultMetadataQueue(){return null;} public boolean isPrimaryClient(){return false;}
 public int getGlobalAudioRestriction(){return 0;} public void setCameraAudioRestriction(int m){}
 public SubmitInfo submitRequest(CaptureRequest r,boolean s){return null;} public SubmitInfo submitRequestList(CaptureRequest[] r,boolean s){return null;}
 public long cancelRequest(int id){return 0;} public long flush(){return 0;} public void waitUntilIdle(){} public void disconnect(){}
 } }''',
'com/gesher/server/CameraService.java': '''package com.gesher.server; import android.hardware.camera2.impl.CameraMetadataNative;
 class CameraService {static final int HAL_BLOB=33;static final long FRAME_DURATION_NS=33333333;
 static void log(String s){} static CameraMetadataNative defaultRequest(WindowsCamera.Device d,int i){return new CameraMetadataNative();}
 static CameraMetadataNative characteristics(WindowsCamera.Device d){return new CameraMetadataNative();} void closed(String s,CameraDeviceUser u){} }''',
'com/gesher/server/CameraFrames.java': '''package com.gesher.server; import android.view.Surface;
 class CameraFrames {static final int OK=0;static boolean isReader(Surface s){return false;}
 static byte[] jpeg(byte[] a,int w,int h,int tw,int th,int q){return new byte[0];}
 static volatile Runnable afterDelivery;
 static int deliver(Surface s,byte[] a,int w,int h,long t,byte[] j){
 Runnable hook=afterDelivery;if(hook!=null)hook.run();return OK;} }''',
'com/gesher/server/WindowsCamera.java': '''package com.gesher.server;
 class WindowsCamera {static final class Device{final String id="host",name="explicit capture adapter";}
 static final class OpenException extends Exception{} static volatile boolean failRead;static volatile int reads,closes;
 static volatile java.util.concurrent.CountDownLatch readEntered,readRelease;
 static final class Capture{final int width=16,height=16;int read(byte[] b){synchronized(WindowsCamera.class){reads++;}
 java.util.concurrent.CountDownLatch gate=readRelease;if(gate!=null){readEntered.countDown();try{gate.await();}catch(InterruptedException e){throw new RuntimeException(e);}}
 return failRead?-1:0;}
 void close(){synchronized(WindowsCamera.class){closes++;}}}
 static Capture open(String id,int w,int h)throws OpenException{return new Capture();} }''',
}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref', help='Compare the exact old production owners in measurement mode')
    parser.add_argument('--measure', action='store_true')
    args=parser.parse_args()
    if args.source_ref and not args.measure: parser.error('--source-ref requires --measure')
    with tempfile.TemporaryDirectory(prefix='gesher-camera-admission-') as temp:
        work=Path(temp); sources=[]
        for relative,source in STUBS.items():
            path=work/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(source);sources.append(str(path))
        for name in ('CameraCallbacks','CameraDeviceUser'):
            path=f'app/java/com/gesher/server/{name}.java'
            source=(subprocess.check_output(['git','show',args.source_ref+':'+path],cwd=ROOT,text=True)
                    if args.source_ref else (ROOT/path).read_text())
            print(name+' production SHA256 '+hashlib.sha256(source.encode()).hexdigest(),flush=True)
            if name=='CameraCallbacks' and not args.source_ref:
                boundary='private static native long nativeMetadataBytes(long metadata);'
                if source.count(boundary)!=1:raise ValueError('native metadata-size seam changed')
                source=source.replace(boundary,'private static long nativeMetadataBytes(long metadata) { return metadata; }')
            path=work/f'{name}.java';path.write_text(source);sources.append(str(path))
        main_class='CameraAdmissionMeasure' if args.measure else 'CameraAdmissionHost'
        sources.append(str(ROOT/f'tests/camera-admission/{main_class}.java'))
        if not args.measure:
            sources.append(str(ROOT/'tests/dex-services/gesher/services/CameraRequestRaceTest.java'))
        subprocess.run([str(args.java_home/'bin/javac'),'-J-XX:-UsePerfData','-encoding','UTF-8','-d',str(work),*sources],check=True,timeout=60)
        subprocess.run([str(args.java_home/'bin/java'),'-XX:-UsePerfData','-Xmx64m','-cp',str(work),'com.gesher.server.'+main_class],check=True,timeout=90)
        if not args.measure:
            subprocess.run([str(args.java_home/'bin/java'),'-XX:-UsePerfData','-Xmx64m','-cp',str(work),
                            'gesher.services.CameraRequestRaceTest'],check=True,timeout=90)
if __name__=='__main__':main()
