#!/usr/bin/env python3
"""Run the production broker and SpeechRecognizer client against local fixtures.

By default use the built framework classes. When checking against an older build
of those classes, use --unpatched-aosp-base to compile the patched client from
the pinned pre-patch frameworks/base checkout. Use --broker-only to execute the
complete SpeechRecognitionManagerService and SpeechRecognitionContractTest on a
standalone host JDK without prebuilt framework jars.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CLIENT = Path('core/java/android/speech/SpeechRecognizerImpl.java')
CLIENT_PATCH = ROOT / 'patches/frameworks/base/0784-speech-recognition-connection-lifecycle.patch'

BROKER_HOST_STUBS = {
    'android/Manifest.java': '''package android;
public final class Manifest {
    public static final class permission {
        public static final String MANAGE_SPEECH_RECOGNITION = "android.permission.MANAGE_SPEECH_RECOGNITION";
    }
}''',
    'android/os/RemoteException.java': '''package android.os;
public class RemoteException extends Exception {
    public RemoteException() {}
    public RemoteException(String message) { super(message); }
}''',
    'android/os/IBinder.java': '''package android.os;
public interface IBinder {
    interface DeathRecipient { void binderDied(); }
    void linkToDeath(DeathRecipient recipient, int flags) throws RemoteException;
    boolean unlinkToDeath(DeathRecipient recipient, int flags);
    default boolean isBinderAlive() { return true; }
    default boolean pingBinder() { return true; }
    default IInterface queryLocalInterface(String descriptor) { return null; }
}''',
    'android/os/IInterface.java': '''package android.os;
public interface IInterface { IBinder asBinder(); }''',
    'android/os/Binder.java': '''package android.os;
public class Binder implements IBinder {
    @Override public void linkToDeath(DeathRecipient recipient, int flags) {}
    @Override public boolean unlinkToDeath(DeathRecipient recipient, int flags) { return true; }
}''',
    'android/os/Bundle.java': '''package android.os;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
public final class Bundle {
    public static final Bundle EMPTY = new Bundle();
    private final Map<String, Object> mValues = new HashMap<>();
    public void putStringArrayList(String key, ArrayList<String> value) { mValues.put(key, value); }
}''',
    'android/os/Process.java': '''package android.os;
public final class Process {
    public static final int INVALID_PID = -1;
    public static boolean isIsolated(int uid) { return uid >= 99000 && uid <= 99999; }
}''',
    'android/app/IBinderSession.java': '''package android.app;
public interface IBinderSession {}''',
    'android/app/IServiceConnection.java': '''package android.app;
import android.content.ComponentName;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;
public interface IServiceConnection extends IInterface {
    void connected(ComponentName name, IBinder service, IBinderSession session, boolean dead)
            throws RemoteException;
    abstract class Stub extends Binder implements IServiceConnection {
        @Override public IBinder asBinder() { return this; }
    }
}''',
    'android/content/ComponentName.java': '''package android.content;
import java.util.Objects;
public final class ComponentName {
    private final String mPackage;
    private final String mClass;
    public ComponentName(String pkg, String cls) {
        mPackage = Objects.requireNonNull(pkg);
        mClass = Objects.requireNonNull(cls);
    }
    public String getPackageName() { return mPackage; }
    public String getClassName() { return mClass; }
    public String flattenToShortString() { return mPackage + "/" + mClass; }
    public static ComponentName unflattenFromString(String str) {
        if (str == null) return null;
        int sep = str.indexOf('/');
        if (sep <= 0 || sep + 1 >= str.length()) return null;
        String pkg = str.substring(0, sep);
        String cls = str.substring(sep + 1);
        if (cls.startsWith(".")) cls = pkg + cls;
        return new ComponentName(pkg, cls);
    }
    @Override public boolean equals(Object o) {
        return o instanceof ComponentName other
                && mPackage.equals(other.mPackage) && mClass.equals(other.mClass);
    }
    @Override public int hashCode() { return Objects.hash(mPackage, mClass); }
    @Override public String toString() { return flattenToShortString(); }
}''',
    'android/content/Context.java': '''package android.content;
public abstract class Context {
    public static final int DEVICE_ID_DEFAULT = 0;
    public static final String SPEECH_RECOGNITION_SERVICE = "speech_recognition";
}''',
    'android/content/AttributionSource.java': '''package android.content;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
public final class AttributionSource {
    private final int mUid;
    private final int mPid;
    private final String mPackageName;
    private final int mDeviceId;
    private final AttributionSource mNext;
    private final IBinder mToken;
    private final Object mState = new Object();
    private AttributionSource(int uid, int pid, String packageName, int deviceId,
            AttributionSource next, IBinder token) {
        mUid = uid; mPid = pid; mPackageName = packageName;
        mDeviceId = deviceId; mNext = next; mToken = token;
    }
    public int getUid() { return mUid; }
    public int getPid() { return mPid; }
    public String getPackageName() { return mPackageName; }
    public int getDeviceId() { return mDeviceId; }
    public AttributionSource getNext() { return mNext; }
    public Object asState() { return mState; }
    public AttributionSource withToken(IBinder token) {
        return new AttributionSource(mUid, mPid, mPackageName, mDeviceId, mNext, token);
    }
    public static final class Builder {
        private final int mUid;
        private int mPid = Process.INVALID_PID;
        private String mPackageName;
        private int mDeviceId = Context.DEVICE_ID_DEFAULT;
        private AttributionSource mNext;
        private IBinder mToken = new Binder();
        public Builder(int uid) { mUid = uid; }
        public Builder(AttributionSource current) {
            mUid = current.mUid; mPid = current.mPid; mPackageName = current.mPackageName;
            mDeviceId = current.mDeviceId; mNext = current.mNext; mToken = current.mToken;
        }
        public Builder setPackageName(String value) { mPackageName = value; return this; }
        public Builder setPid(int value) { mPid = value; return this; }
        public Builder setDeviceId(int value) { mDeviceId = value; return this; }
        public Builder setNext(AttributionSource value) { mNext = value; return this; }
        public AttributionSource build() {
            return new AttributionSource(mUid, mPid, mPackageName, mDeviceId, mNext, mToken);
        }
    }
}''',
    'android/content/Intent.java': '''package android.content;
import java.util.HashMap;
import java.util.Map;
public class Intent {
    private final String mAction;
    private ComponentName mComponent;
    private final Map<String, Object> mExtras = new HashMap<>();
    public Intent(String action) { mAction = action; }
    public Intent(Intent other) {
        mAction = other.mAction; mComponent = other.mComponent; mExtras.putAll(other.mExtras);
    }
    public Intent setComponent(ComponentName component) { mComponent = component; return this; }
    public ComponentName getComponent() { return mComponent; }
    public Intent putExtra(String name, String value) { mExtras.put(name, value); return this; }
    public Intent putExtra(String name, boolean value) { mExtras.put(name, value); return this; }
    public Intent putExtra(String name, int value) { mExtras.put(name, value); return this; }
    public String getStringExtra(String name) { return (String) mExtras.get(name); }
    public boolean getBooleanExtra(String name, boolean def) {
        return mExtras.get(name) instanceof Boolean b ? b : def;
    }
    public int getIntExtra(String name, int def) {
        return mExtras.get(name) instanceof Integer i ? i : def;
    }
}''',
    'android/content/pm/ApplicationInfo.java': '''package android.content.pm;
public class ApplicationInfo { public boolean enabled = true; }''',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public abstract class PackageManager {
    public static final int PERMISSION_GRANTED = 0;
    public static final int PERMISSION_DENIED = -1;
}''',
    'android/content/pm/ServiceInfo.java': '''package android.content.pm;
import android.content.ComponentName;
public class ServiceInfo {
    public String packageName = "";
    public String name = "";
    public boolean exported;
    public boolean enabled;
    public ApplicationInfo applicationInfo;
    public ComponentName getComponentName() { return new ComponentName(packageName, name); }
}''',
    'android/content/pm/ResolveInfo.java': '''package android.content.pm;
public class ResolveInfo { public ServiceInfo serviceInfo; }''',
    'android/content/res/Resources.java': '''package android.content.res;
public class Resources {
    private static final Resources SYSTEM = new Resources();
    public static Resources getSystem() { return SYSTEM; }
    public String getString(int id) { return null; }
}''',
    'com/android/internal/R.java': '''package com.android.internal;
public final class R {
    public static final class string {
        public static final int config_defaultOnDeviceSpeechRecognitionService = 1;
    }
}''',
    'android/speech/RecognitionService.java': '''package android.speech;
public abstract class RecognitionService {
    public static final String SERVICE_INTERFACE = "android.speech.RecognitionService";
}''',
    'android/speech/RecognizerIntent.java': '''package android.speech;
public final class RecognizerIntent {
    public static final String ACTION_RECOGNIZE_SPEECH = "android.speech.action.RECOGNIZE_SPEECH";
    public static final String EXTRA_LANGUAGE = "android.speech.extra.LANGUAGE";
    public static final String EXTRA_PARTIAL_RESULTS = "android.speech.extra.PARTIAL_RESULTS";
}''',
    'android/speech/SpeechRecognizer.java': '''package android.speech;
public final class SpeechRecognizer {
    public static final int ERROR_NETWORK_TIMEOUT = 1;
    public static final int ERROR_NETWORK = 2;
    public static final int ERROR_AUDIO = 3;
    public static final int ERROR_SERVER = 4;
    public static final int ERROR_CLIENT = 5;
    public static final int ERROR_SPEECH_TIMEOUT = 6;
    public static final int ERROR_NO_MATCH = 7;
    public static final int ERROR_RECOGNIZER_BUSY = 8;
    public static final int ERROR_INSUFFICIENT_PERMISSIONS = 9;
    public static final int ERROR_TOO_MANY_REQUESTS = 10;
    public static final int ERROR_SERVER_DISCONNECTED = 11;
    public static final int ERROR_LANGUAGE_NOT_SUPPORTED = 12;
    public static final int ERROR_LANGUAGE_UNAVAILABLE = 13;
    public static final String RESULTS_RECOGNITION = "results_recognition";
}''',
    'android/speech/RecognitionSupport.java': '''package android.speech;
import java.util.List;
public final class RecognitionSupport {
    public static final class Builder {
        public Builder setInstalledOnDeviceLanguages(List<String> languages) { return this; }
        public RecognitionSupport build() { return new RecognitionSupport(); }
    }
}''',
    'android/speech/IModelDownloadListener.java': '''package android.speech;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;
public interface IModelDownloadListener extends IInterface {
    void onProgress(int completedPercent) throws RemoteException;
    void onSuccess() throws RemoteException;
    void onScheduled() throws RemoteException;
    void onError(int error) throws RemoteException;
    abstract class Stub extends Binder implements IModelDownloadListener {
        @Override public IBinder asBinder() { return this; }
    }
}''',
    'android/speech/IRecognitionListener.java': '''package android.speech;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;
public interface IRecognitionListener extends IInterface {
    void onReadyForSpeech(Bundle params) throws RemoteException;
    void onBeginningOfSpeech() throws RemoteException;
    void onRmsChanged(float rmsdB) throws RemoteException;
    void onBufferReceived(byte[] buffer) throws RemoteException;
    void onEndOfSpeech() throws RemoteException;
    void onError(int error) throws RemoteException;
    void onResults(Bundle results) throws RemoteException;
    void onPartialResults(Bundle partialResults) throws RemoteException;
    void onSegmentResults(Bundle segmentResults) throws RemoteException;
    void onEndOfSegmentedSession() throws RemoteException;
    void onLanguageDetection(Bundle results) throws RemoteException;
    void onEvent(int eventType, Bundle params) throws RemoteException;
    abstract class Stub extends Binder implements IRecognitionListener {
        @Override public IBinder asBinder() { return this; }
    }
}''',
    'android/speech/IRecognitionSupportCallback.java': '''package android.speech;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;
public interface IRecognitionSupportCallback extends IInterface {
    void onSupportResult(RecognitionSupport recognitionSupport) throws RemoteException;
    void onError(int error) throws RemoteException;
    abstract class Stub extends Binder implements IRecognitionSupportCallback {
        @Override public IBinder asBinder() { return this; }
    }
}''',
    'android/speech/IRecognitionService.java': '''package android.speech;
import android.content.AttributionSource;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;
public interface IRecognitionService extends IInterface {
    void startListening(Intent recognizerIntent, IRecognitionListener listener,
            AttributionSource attributionSource) throws RemoteException;
    void stopListening(IRecognitionListener listener) throws RemoteException;
    void cancel(IRecognitionListener listener, boolean isShutdown) throws RemoteException;
    void checkRecognitionSupport(Intent recognizerIntent, AttributionSource attributionSource,
            IRecognitionSupportCallback callback) throws RemoteException;
    void triggerModelDownload(Intent recognizerIntent, AttributionSource attributionSource,
            IModelDownloadListener listener) throws RemoteException;
    abstract class Stub extends Binder implements IRecognitionService {
        @Override public IBinder asBinder() { return this; }
        public static IRecognitionService asInterface(IBinder obj) {
            return obj instanceof IRecognitionService service ? service : null;
        }
    }
}''',
    'android/speech/IRecognitionServiceManagerCallback.java': '''package android.speech;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;
public interface IRecognitionServiceManagerCallback extends IInterface {
    void onSuccess(IRecognitionService service) throws RemoteException;
    void onError(int errorCode) throws RemoteException;
    abstract class Stub extends Binder implements IRecognitionServiceManagerCallback {
        @Override public IBinder asBinder() { return this; }
    }
}''',
    'android/speech/IRecognitionServiceManager.java': '''package android.speech;
import android.content.ComponentName;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;
public interface IRecognitionServiceManager extends IInterface {
    void createSession(ComponentName componentName, IBinder clientToken, boolean onDevice,
            IRecognitionServiceManagerCallback callback) throws RemoteException;
    void setTemporaryComponent(ComponentName componentName) throws RemoteException;
    abstract class Stub extends Binder implements IRecognitionServiceManager {
        @Override public IBinder asBinder() { return this; }
    }
}''',
    'com/gesher/server/CallerIdentity.java': '''package com.gesher.server;
final class CallerIdentity {
    String callerPackage() { return null; }
    int callerPid() { return 0; }
    int callerUid() { return 0; }
}''',
    'com/gesher/server/ActivityManagerService.java': '''package com.gesher.server;
import android.app.IServiceConnection;
import android.content.Intent;
import java.util.function.BooleanSupplier;
public final class ActivityManagerService {
    CallerIdentity callers() { return new CallerIdentity(); }
    int bindSystemService(Intent intent, IServiceConnection conn, BooleanSupplier guard) { return 0; }
    void unbindService(IServiceConnection conn) {}
}''',
    'com/gesher/server/ActivityTaskManagerService.java': '''package com.gesher.server;
import android.os.IInterface;
public final class ActivityTaskManagerService {
    static final class ProcessRecord {
        IInterface thread;
        boolean dead;
        boolean exiting;
        String packageName;
    }
    ProcessRecord callerProcess(int pid) { return null; }
}''',
    'com/gesher/server/PermissionPolicy.java': '''package com.gesher.server;
import android.os.IBinder;
public final class PermissionPolicy {
    public int checkPermission(String pkg, String perm) { return -1; }
    public boolean isRegisteredAttributionSource(Object state) { return false; }
    public IBinder registerAttributionSource(Object state) { return null; }
}''',
    'com/gesher/server/ApkPackage.java': '''package com.gesher.server;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import java.util.List;
public final class ApkPackage {
    public String packageName;
    public enum ComponentKind { SERVICE }
    public boolean componentEnabled(String cls, ComponentKind kind) { return false; }
    public List<ResolveInfo> queryIntentServices(Intent intent, String resolvedType, long flags) {
        return List.of();
    }
}''',
    'com/gesher/server/PackageManagerService.java': '''package com.gesher.server;
import java.util.function.Consumer;
public final class PackageManagerService {
    void onPackageRemoval(Consumer<String> listener) {}
    public ApkPackage get(String name) { return null; }
    public boolean isRemovalPending(String name) { return false; }
    public PermissionPolicy permissions() { return new PermissionPolicy(); }
}''',
}


def build_native(cc, java_home, source, output):
    subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC', '-pthread',
                    '-I' + str(java_home / 'include'), '-I' + str(java_home / 'include/linux'),
                    str(ROOT / 'tests' / source / 'framework_host.c'), '-o', str(output)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    for argument in ('framework-classes', 'core-classes', 'app-classes'):
        parser.add_argument('--' + argument, type=Path)
    parser.add_argument('--unpatched-aosp-base', type=Path)
    parser.add_argument('--source-ref', help='Use an older broker as an admission/dispatch negative control')
    parser.add_argument('--broker-only', action='store_true',
                        help='Run complete broker contract suite against isolated host stubs without prebuilt framework jars')
    args = parser.parse_args()
    if not args.broker_only and not all((args.framework_classes, args.core_classes, args.app_classes)):
        parser.error('--framework-classes, --core-classes and --app-classes are required unless --broker-only is set')
    cc = shutil.which('cc')
    if cc is None:
        parser.error('a native C compiler is required')
    with tempfile.TemporaryDirectory(prefix='gesher-speech-') as temporary:
        work = Path(temporary)
        binder = work / 'libspeech_binder_host.so'
        build_native(cc, args.java_home, 'scheduler', binder)
        broker_path = 'app/java/com/gesher/server/SpeechRecognitionManagerService.java'
        broker_text = (subprocess.run(['git', 'show', args.source_ref + ':' + broker_path],
                                     cwd=ROOT, capture_output=True, text=True, check=True).stdout
                       if args.source_ref else (ROOT / broker_path).read_text())
        # Count actual visits to request dispatch in the complete production
        # broker, without shipping test counters or relying on host timing.
        counter_anchor = '    private static final int MAX_CONNECTIONS_PER_UID = 10;'
        dispatch_anchor = '            final boolean dispatch(IRecognitionService provider) {'
        if broker_text.count(counter_anchor) != 1 or broker_text.count(dispatch_anchor) != 1:
            raise RuntimeError('Cannot instrument speech request dispatch unambiguously')
        broker_text = broker_text.replace(counter_anchor,
                counter_anchor + '\n    public volatile long testRequestDispatchVisits;'
                + '\n    public volatile long testQuotaAdmissionVisits;')
        broker_text = broker_text.replace(dispatch_anchor,
                dispatch_anchor + '\n                testRequestDispatchVisits++;')
        # Observe actual quota work in the old session scan or the current UID
        # lookup. Identical behavior assertions run against either source.
        quota_anchor = '        for (Session session : mSessions.values()) {'
        if quota_anchor not in broker_text:
            quota_anchor = '        Map<ComponentName, Integer> connections = mConnectionsByUid.get(uid);'
        if broker_text.count(quota_anchor) != 1:
            raise RuntimeError('Cannot instrument speech quota admission unambiguously')
        broker_text = broker_text.replace(quota_anchor,
                quota_anchor + '\n            testQuotaAdmissionVisits++;')
        broker = work / 'SpeechRecognitionManagerService.java'
        broker.write_text(broker_text)
        if args.broker_only:
            stubs_dir = work / 'stubs'
            stub_sources = []
            for rel, content in BROKER_HOST_STUBS.items():
                target = stubs_dir / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content, encoding='utf-8')
                stub_sources.append(target)
            classes = work / 'classes'
            classes.mkdir()
            sources = [broker, ROOT / 'tests/dex-services/com/gesher/server/SpeechRecognitionContractTest.java',
                       *stub_sources]
            subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                            '-d', str(classes), *map(str, sources)], check=True)
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                            '-Dgesher.test.speech.dispatch-budget=true',
                            '-Dgesher.test.speech.quota-budget=true',
                            '-cp', str(classes),
                            'com.gesher.server.SpeechRecognitionContractTest', str(binder)],
                           check=True, timeout=45)
            return

        looper = work / 'libspeech_looper_host.so'
        build_native(cc, args.java_home, 'cookie-callbacks', looper)
        classpath = os.pathsep.join(str(p.resolve()) for p in
                                    (args.app_classes, args.framework_classes, args.core_classes))
        sources = [broker, *[ROOT / path for path in (
            'tests/dex-services/com/gesher/server/SpeechRecognitionContractTest.java',
            'tests/dex-services/gesher/services/SpeechRecognizerConnectionTest.java',
            'tests/cookie-callbacks/PerfettoTrace.java')]]
        if args.unpatched_aosp_base:
            client = work / CLIENT
            client.parent.mkdir(parents=True)
            shutil.copyfile(args.unpatched_aosp_base / CLIENT, client)
            with CLIENT_PATCH.open('rb') as patch:
                subprocess.run(['patch', '--batch', '--silent', '-p1'], stdin=patch, cwd=work, check=True)
            sources.append(client)
        subprocess.run([str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(work), *map(str, sources)], check=True)
        cleaner = work / 'cleaner' / 'sun' / 'misc'
        cleaner.mkdir(parents=True)
        for file in (args.core_classes / 'sun' / 'misc').glob('Cleaner*.class'):
            shutil.copy2(file, cleaner / file.name)
        java = [str(args.java_home / 'bin/java'), '-XX:+UnlockDiagnosticVMOptions',
                '-XX:-BytecodeVerificationRemote', '--patch-module',
                'jdk.unsupported=' + str(work / 'cleaner'), '-cp', str(work) + os.pathsep + classpath]
        subprocess.run([*java, '-Dgesher.test.speech.dispatch-budget=true',
                        '-Dgesher.test.speech.quota-budget=true',
                        'com.gesher.server.SpeechRecognitionContractTest', str(binder)],
                       check=True, timeout=45)
        subprocess.run([*java, 'gesher.services.SpeechRecognizerConnectionTest', str(binder), str(looper)],
                       check=True, timeout=45)


if __name__ == '__main__':
    main()
