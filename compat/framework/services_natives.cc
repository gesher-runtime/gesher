// Gesher: natives the framework's own app-process path needs (an app attached
// to a system server: ActivityThread.attach, bindApplication, LoadedApk's
// class loader), registered by android_runtime.cc. Services track,
// docs/services.md.
//
// - com.android.internal.os.ApplicationSharedMemory: frameworks/base's own
//   core/jni source, included here so that its static functions can be
//   registered through GESHER_JNI (System V thunks) without another JNI table
//   patch. The region is libcutils' host ashmem (a temporary file), which
//   travels to the app process as a file descriptor over binder (binderd
//   duplicates its handle), as ActivityManagerService sends it in
//   bindApplication.
// - com.android.internal.os.ClassLoaderFactory.createClassloaderNamespace:
//   libnativeloader's linker namespaces do not exist on Windows (gesher-base's
//   dlopen loads a library by path); every class loader gets the success
//   answer, null, so LoadedApk's own class loader path runs.
// - android.app.ActivityThread's two natives (android_app_ActivityThread.cpp):
//   nPurgePendingResources (hwui's caches and SQLite's memory, none here) and
//   nInitZygoteChildHeapProfiling (heapprofd, which does not exist), no-ops.
// - com.android.internal.os.DebugStore (ActivityThread records its lifecycle
//   events there when the debug_store flag is on): the native store
//   (libdebugstore, for dumpsys) is not built; events are dropped.
// - com.gesher.server.Zygote (gesher-app.jar): the Gesher device's processes
//   (services_zygote.cc): starting them detached, their liveness, a job that
//   ends them with the system server, a named mutex and event.

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <unistd.h>

#include "core_jni_helpers.h"
#include "services_zygote.h"

// Its layout checks use offsetof on a class with atomics, which Soong's flags
// let pass (-Wno-invalid-offsetof).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#include "com_android_internal_os_ApplicationSharedMemory.cpp"
#pragma clang diagnostic pop

namespace android {

// Registers a table; not fatal when the class lacks a method (the natives tests
// load stand-ins of some of these classes without natives): logged and skipped.
int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count) {
    jclass clazz = env->FindClass(className);
    if (clazz == nullptr) {
        env->ExceptionClear();
        return 0;
    }
    if (env->RegisterNatives(clazz, methods, count) < 0) {
        env->ExceptionClear();
        ALOGI("libandroid_runtime: %s does not declare these natives; skipped", className);
    }
    env->DeleteLocalRef(clazz);
    return 0;
}

static const JNINativeMethod gGesherApplicationSharedMemoryMethods[] = {
    {"nativeCreate", "()I", GESHER_JNI(nativeCreate)},
    {"nativeMap", "(IZ)J", GESHER_JNI(nativeMap)},
    {"nativeInit", "(J)V", GESHER_JNI(nativeInit)},
    {"nativeUnmap", "(J)V", GESHER_JNI(nativeUnmap)},
    {"nativeDupAsReadOnly", "(I)I", GESHER_JNI(nativeDupAsReadOnly)},
    {"nativeSetLatestNetworkTimeUnixEpochMillisAtZeroElapsedRealtimeMillis", "(JJ)V",
     GESHER_JNI(nativeSetLatestNetworkTimeUnixEpochMillisAtZeroElapsedRealtimeMillis)},
    {"nativeGetLatestNetworkTimeUnixEpochMillisAtZeroElapsedRealtimeMillis", "(J)J",
     GESHER_JNI(nativeGetLatestNetworkTimeUnixEpochMillisAtZeroElapsedRealtimeMillis)},
    {"nativeGetSystemNonceBlock", "(J)J", GESHER_JNI(nativeGetSystemNonceBlock)},
    {"nativeWriteSystemFeaturesCache", "(J[I)V", GESHER_JNI(nativeWriteSystemFeaturesCache)},
    {"nativeReadSystemFeaturesCache", "(J)[I", GESHER_JNI(nativeReadSystemFeaturesCache)},
    {"nativeSetCurrentAnimatorScale", "(JF)V", GESHER_JNI(nativeSetCurrentAnimatorScale)},
    {"nativeGetCurrentAnimatorScale", "(J)F", GESHER_JNI(nativeGetCurrentAnimatorScale)},
};

int register_gesher_ApplicationSharedMemory(JNIEnv* env) {
    return RegisterGesherNatives(env, "com/android/internal/os/ApplicationSharedMemory",
                                gGesherApplicationSharedMemoryMethods,
                                NELEM(gGesherApplicationSharedMemoryMethods));
}

static jstring ClassLoaderFactory_createClassloaderNamespace(
        JNIEnv*, jclass, jobject /* classLoader */, jint /* targetSdkVersion */,
        jstring /* librarySearchPath */, jstring /* libraryPermittedPath */,
        jboolean /* isShared */, jstring /* dexPath */, jstring /* sonameList */) {
    return nullptr;
}

static const JNINativeMethod gClassLoaderFactoryMethods[] = {
    {"createClassloaderNamespace",
     "(Ljava/lang/ClassLoader;ILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;"
     "Ljava/lang/String;)Ljava/lang/String;",
     GESHER_JNI(ClassLoaderFactory_createClassloaderNamespace)},
};

int register_gesher_ClassLoaderFactory(JNIEnv* env) {
    return RegisterGesherNatives(env, "com/android/internal/os/ClassLoaderFactory",
                                gClassLoaderFactoryMethods, NELEM(gClassLoaderFactoryMethods));
}

static void ActivityThread_noOp(JNIEnv*, jobject) {}

static const JNINativeMethod gActivityThreadMethods[] = {
    {"nPurgePendingResources", "()V", GESHER_JNI(ActivityThread_noOp)},
    {"nInitZygoteChildHeapProfiling", "()V", GESHER_JNI(ActivityThread_noOp)},
};

int register_gesher_ActivityThread(JNIEnv* env) {
    return RegisterGesherNatives(env, "android/app/ActivityThread", gActivityThreadMethods,
                                NELEM(gActivityThreadMethods));
}

static jlong DebugStore_beginEvent(JNIEnv*, jclass, jstring, jobject) { return 0; }
static void DebugStore_endEvent(JNIEnv*, jclass, jlong, jobject) {}
static void DebugStore_recordEvent(JNIEnv*, jclass, jstring, jobject) {}

static const JNINativeMethod gDebugStoreMethods[] = {
    {"beginEventNative", "(Ljava/lang/String;Ljava/util/List;)J",
     GESHER_JNI(DebugStore_beginEvent)},
    {"endEventNative", "(JLjava/util/List;)V", GESHER_JNI(DebugStore_endEvent)},
    {"recordEventNative", "(Ljava/lang/String;Ljava/util/List;)V",
     GESHER_JNI(DebugStore_recordEvent)},
};

int register_gesher_DebugStore(JNIEnv* env) {
    return RegisterGesherNatives(env, "com/android/internal/os/DebugStore", gDebugStoreMethods,
                                NELEM(gDebugStoreMethods));
}

// --- com.gesher.server.Zygote ---------------------------------------------------------

static std::u16string ZygoteString(JNIEnv* env, jstring s) {
    if (s == nullptr) return std::u16string();
    const jchar* chars = env->GetStringChars(s, nullptr);
    std::u16string result(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(s));
    env->ReleaseStringChars(s, chars);
    return result;
}

static std::vector<std::u16string> ZygoteStrings(JNIEnv* env, jobjectArray array) {
    std::vector<std::u16string> result;
    if (array == nullptr) return result;
    for (jsize i = 0, n = env->GetArrayLength(array); i < n; i++) {
        jstring s = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        result.push_back(ZygoteString(env, s));
        env->DeleteLocalRef(s);
    }
    return result;
}

static jstring ZygoteJavaString(JNIEnv* env, const std::u16string& s) {
    return env->NewString(reinterpret_cast<const jchar*>(s.data()),
                          static_cast<jsize>(s.size()));
}

static jint Zygote_spawn(JNIEnv* env, jclass, jstring commandLine, jobjectArray set,
                         jobjectArray unset, jstring logPath, jboolean breakaway, jint uid,
                         jstring package, jstring install, jboolean installed) {
    return gesher::zygote::Spawn(ZygoteString(env, commandLine), ZygoteStrings(env, set),
                                 ZygoteStrings(env, unset), ZygoteString(env, logPath),
                                 breakaway == JNI_TRUE, uid, ZygoteString(env, package),
                                 ZygoteString(env, install), installed == JNI_TRUE);
}

static jboolean Zygote_revokeInstall(JNIEnv* env, jclass, jstring package, jstring install) {
    return gesher::zygote::RevokeInstall(ZygoteString(env, package), ZygoteString(env, install))
        ? JNI_TRUE : JNI_FALSE;
}
static jboolean Zygote_provisionInstalled(JNIEnv* env, jclass, jstring package, jstring install,
    jint uid, jobjectArray read, jstring data) {
    return gesher::zygote::ProvisionInstalled(ZygoteString(env, package), ZygoteString(env, install),
        uid, ZygoteStrings(env, read), ZygoteString(env, data)) ? JNI_TRUE : JNI_FALSE;
}
static jboolean Zygote_authority(JNIEnv*, jclass) {
    return gesher::zygote::IsAuthority() ? JNI_TRUE : JNI_FALSE;
}
static jboolean Zygote_verifyRestrictedGuest(JNIEnv*, jclass) {
    return gesher::zygote::VerifyRestrictedGuest() ? JNI_TRUE : JNI_FALSE;
}
static jboolean Zygote_retireInstalled(JNIEnv*, jclass) {
    return gesher::zygote::RetireInstalled() ? JNI_TRUE : JNI_FALSE;
}
static jboolean Zygote_ownedInstall(JNIEnv* env, jclass, jint pid, jstring package, jstring install) {
    return gesher::zygote::OwnedInstall(pid, ZygoteString(env, package), ZygoteString(env, install))
        ? JNI_TRUE : JNI_FALSE;
}
static jintArray Zygote_startDevice(JNIEnv* env, jclass, jobjectArray commands,
                                   jobjectArray logs, jobjectArray set) {
    auto result = gesher::zygote::StartDevice(ZygoteStrings(env, commands),
        ZygoteStrings(env, logs), ZygoteStrings(env, set));
    jintArray array = env->NewIntArray(static_cast<jsize>(result.size()));
    if (array && !result.empty()) env->SetIntArrayRegion(array, 0,
        static_cast<jsize>(result.size()), result.data());
    return array;
}

static jlong Zygote_startTime(JNIEnv*, jclass, jint pid) {
    return gesher::zygote::StartTime(pid);
}

static jlong Zygote_exitStatus(JNIEnv*, jclass, jint pid) {
    return gesher::zygote::ExitStatus(pid);
}

static jint Zygote_exitCode(JNIEnv*, jclass, jint pid) {
    return gesher::zygote::ExitCode(pid);
}

static jboolean Zygote_kill(JNIEnv*, jclass, jint pid) {
    return gesher::zygote::Kill(pid) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Zygote_joinJob(JNIEnv*, jclass, jint pid) {
    return gesher::zygote::JoinJob(pid) ? JNI_TRUE : JNI_FALSE;
}

static jlong Zygote_lock(JNIEnv* env, jclass, jstring name, jint timeoutMs) {
    return gesher::zygote::Lock(ZygoteString(env, name), timeoutMs);
}

static void Zygote_unlock(JNIEnv*, jclass, jlong handle) {
    gesher::zygote::Unlock(static_cast<intptr_t>(handle));
}

static jlong Zygote_createEvent(JNIEnv* env, jclass, jstring name) {
    return gesher::zygote::MakeEvent(ZygoteString(env, name));
}

static jboolean Zygote_waitEvent(JNIEnv*, jclass, jlong handle, jint timeoutMs) {
    return gesher::zygote::WaitEvent(static_cast<intptr_t>(handle), timeoutMs) ? JNI_TRUE
                                                                               : JNI_FALSE;
}

static jboolean Zygote_signalEvent(JNIEnv* env, jclass, jstring name) {
    return gesher::zygote::SignalEvent(ZygoteString(env, name)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" int gesher_vm_entrypoint_index();
static jint Zygote_entrypointIndex(JNIEnv*, jclass) {
    return gesher_vm_entrypoint_index();
}

static jstring Zygote_commandLine(JNIEnv* env, jclass) {
    return ZygoteJavaString(env, gesher::zygote::CommandLine());
}
static jboolean Zygote_waitForBinder(JNIEnv* env, jclass, jstring device, jint pid, jint timeout) {
    return gesher::zygote::WaitForBinder(ZygoteString(env, device), pid, timeout) ? JNI_TRUE : JNI_FALSE;
}

static jstring Zygote_exePath(JNIEnv* env, jclass) {
    return ZygoteJavaString(env, gesher::zygote::ExePath());
}

static const JNINativeMethod gZygoteMethods[] = {
    {"nativeRetireInstalled", "()Z", GESHER_JNI(Zygote_retireInstalled)},
    {"verifyRestrictedGuest", "()Z", GESHER_JNI(Zygote_verifyRestrictedGuest)},
    {"nativeProvisionInstalled", "(Ljava/lang/String;Ljava/lang/String;I[Ljava/lang/String;Ljava/lang/String;)Z", GESHER_JNI(Zygote_provisionInstalled)},
    {"nativeStartDevice", "([Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)[I", GESHER_JNI(Zygote_startDevice)},
    {"nativeIsAuthority", "()Z", GESHER_JNI(Zygote_authority)},
    {"nativeOwnedInstall", "(ILjava/lang/String;Ljava/lang/String;)Z", GESHER_JNI(Zygote_ownedInstall)},
    {"nativeSpawn",
     "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;ZILjava/lang/String;Ljava/lang/String;Z)I",
     GESHER_JNI(Zygote_spawn)},
    {"nativeRevokeInstall", "(Ljava/lang/String;Ljava/lang/String;)Z", GESHER_JNI(Zygote_revokeInstall)},
    {"nativeStartTime", "(I)J", GESHER_JNI(Zygote_startTime)},
    {"nativeExitCode", "(I)I", GESHER_JNI(Zygote_exitCode)},
    {"nativeExitStatus", "(I)J", GESHER_JNI(Zygote_exitStatus)},
    {"nativeKill", "(I)Z", GESHER_JNI(Zygote_kill)},
    {"nativeJoinJob", "(I)Z", GESHER_JNI(Zygote_joinJob)},
    {"nativeLock", "(Ljava/lang/String;I)J", GESHER_JNI(Zygote_lock)},
    {"nativeUnlock", "(J)V", GESHER_JNI(Zygote_unlock)},
    {"nativeCreateEvent", "(Ljava/lang/String;)J", GESHER_JNI(Zygote_createEvent)},
    {"nativeWaitEvent", "(JI)Z", GESHER_JNI(Zygote_waitEvent)},
    {"nativeSignalEvent", "(Ljava/lang/String;)Z", GESHER_JNI(Zygote_signalEvent)},
    {"nativeWaitForBinder", "(Ljava/lang/String;II)Z", GESHER_JNI(Zygote_waitForBinder)},
    {"nativeEntrypointIndex", "()I", GESHER_JNI(Zygote_entrypointIndex)},
    {"nativeCommandLine", "()Ljava/lang/String;", GESHER_JNI(Zygote_commandLine)},
    {"nativeExePath", "()Ljava/lang/String;", GESHER_JNI(Zygote_exePath)},
};

int register_gesher_Zygote(JNIEnv* env) {
    return RegisterGesherNatives(env, "com/gesher/server/Zygote", gZygoteMethods,
                                NELEM(gZygoteMethods));
}

}  // namespace android
