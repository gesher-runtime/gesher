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
#include <cstdio>
#include <gesher/application_shared_memory.h>

#include "core_jni_helpers.h"
#include "services_zygote.h"
#include "services_dexopt_watch.h"
#include "../usb_storage/protected_runtime_client.h"

// Its layout checks use offsetof on a class with atomics, which Soong's flags
// let pass (-Wno-invalid-offsetof).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#include "com_android_internal_os_ApplicationSharedMemory.cpp"
#pragma clang diagnostic pop

namespace {

static jint ProtectedApplicationMemoryCreate(JNIEnv* env, jclass clazz) {
    const jint fd = nativeCreate(env, clazz);
    if (fd < 0) return fd;
    if (env->ExceptionCheck()) {
        close(fd);
        return -1;
    }
    if (gesher::PublishApplicationSharedMemory(fd, sizeof(SharedMemory)) < 0) {
        const int error = errno;
        close(fd);
        jniThrowExceptionFmt(env, "java/lang/RuntimeException",
                            "Cannot publish protected ApplicationSharedMemory: %s", strerror(error));
        return -1;
    }
    return fd;
}

static jint ProtectedApplicationMemoryReadOnly(JNIEnv* env, jclass clazz, jint fd) {
    if (!gesher::UsesProtectedBinder()) return nativeDupAsReadOnly(env, clazz, fd);
    const int result = gesher::DuplicateApplicationSharedMemoryReadOnly(fd);
    if (result < 0) {
        jniThrowExceptionFmt(env, "java/lang/RuntimeException",
                            "Cannot duplicate protected ApplicationSharedMemory: %s", strerror(errno));
    }
    return result;
}

}  // namespace

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
    {"nativeCreate", "()I", GESHER_JNI(ProtectedApplicationMemoryCreate)},
    {"nativeMap", "(IZ)J", GESHER_JNI(nativeMap)},
    {"nativeInit", "(J)V", GESHER_JNI(nativeInit)},
    {"nativeUnmap", "(J)V", GESHER_JNI(nativeUnmap)},
    {"nativeDupAsReadOnly", "(I)I", GESHER_JNI(ProtectedApplicationMemoryReadOnly)},
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
                         jobjectArray unset, jstring logPath, jboolean breakaway) {
    return gesher::zygote::Spawn(ZygoteString(env, commandLine), ZygoteStrings(env, set),
                                 ZygoteStrings(env, unset), ZygoteString(env, logPath),
                                 breakaway == JNI_TRUE);
}

static jboolean Zygote_protectedRuntimeEnabled(JNIEnv*, jclass) {
    return gesher::usb_storage::ProtectedRuntimeEnabled() ? JNI_TRUE : JNI_FALSE;
}

static jint Zygote_startProtectedRuntime(JNIEnv* env, jclass, jstring apk) {
    return static_cast<jint>(gesher::usb_storage::StartProtectedRuntime(ZygoteString(env, apk)));
}

static jint Zygote_spawnProtected(JNIEnv* env, jclass, jstring packageName,
        jstring processName, jstring apkPath, jstring apkSha256, jstring signerSha256,
        jint uid, jint targetSdk, jlong startSequence, jobjectArray environment) {
    if (uid < 10000 || targetSdk <= 0 || startSequence <= 0) return -87;
    gesher::usb_storage::ProtectedSpawnSpec spec;
    spec.package_name = ZygoteString(env, packageName);
    spec.process_name = ZygoteString(env, processName);
    spec.apk_path = ZygoteString(env, apkPath);
    spec.apk_sha256 = ZygoteString(env, apkSha256);
    spec.signer_sha256 = ZygoteString(env, signerSha256);
    spec.uid = static_cast<uint32_t>(uid);
    spec.target_sdk = static_cast<uint32_t>(targetSdk);
    spec.start_sequence = static_cast<uint64_t>(startSequence);
    spec.environment = ZygoteStrings(env, environment);
    if (env->ExceptionCheck()) return -8;
    return gesher::zygote::SpawnProtected(spec);
}

static jint Zygote_spawnInAppContainer(JNIEnv* env, jclass, jstring commandLine,
                                       jobjectArray set, jobjectArray unset, jstring logPath,
                                       jstring name, jstring displayName,
                                       jobjectArray capabilities) {
    gesher::zygote::AppContainerSpec spec;
    spec.name = ZygoteString(env, name);
    spec.display_name = ZygoteString(env, displayName);
    spec.capabilities = ZygoteStrings(env, capabilities);
    return gesher::zygote::SpawnInAppContainer(ZygoteString(env, commandLine),
                                               ZygoteStrings(env, set), ZygoteStrings(env, unset),
                                               ZygoteString(env, logPath), spec);
}

static jint Zygote_ensureAppContainer(JNIEnv* env, jclass, jstring name, jstring displayName) {
    return static_cast<jint>(gesher::zygote::EnsureAppContainer(ZygoteString(env, name),
                                                                ZygoteString(env, displayName)));
}

static jint Zygote_grantToAppContainer(JNIEnv* env, jclass, jstring path, jstring name,
                                       jint access, jboolean inherit) {
    return static_cast<jint>(gesher::zygote::GrantToAppContainer(
        ZygoteString(env, path), ZygoteString(env, name), static_cast<uint32_t>(access),
        inherit == JNI_TRUE));
}

static jint Zygote_grantToCapability(JNIEnv* env, jclass, jstring path, jstring capability,
                                     jint access, jboolean inherit) {
    return static_cast<jint>(gesher::zygote::GrantToCapability(
        ZygoteString(env, path), ZygoteString(env, capability), static_cast<uint32_t>(access),
        inherit == JNI_TRUE));
}

static jint Zygote_deleteAppContainer(JNIEnv* env, jclass, jstring name) {
    return static_cast<jint>(gesher::zygote::DeleteAppContainer(ZygoteString(env, name)));
}

static jint Zygote_spawnCompiler(JNIEnv* env, jclass, jstring commandLine, jobjectArray set,
                                 jobjectArray unset, jstring logPath, jint memory, jint cpu) {
    return gesher::zygote::Spawn(ZygoteString(env, commandLine), ZygoteStrings(env, set),
                                 ZygoteStrings(env, unset), ZygoteString(env, logPath),
                                 false, static_cast<uint32_t>(memory), static_cast<uint32_t>(cpu));
}
static jint Zygote_waitChild(JNIEnv*, jclass, jint pid, jlong identity, jint timeout,
                             jlong cancel, jboolean memory) {
    return gesher::zygote::WaitChild(pid, identity, timeout, static_cast<intptr_t>(cancel),
                                     memory == JNI_TRUE);
}
static jlong Zygote_availableMemory(JNIEnv*, jclass) {
    return static_cast<jlong>(gesher::zygote::AvailableMemory());
}

static jint Zygote_restartWithUid(JNIEnv* env, jclass, jint uid) {
    uint32_t code = 0;
    uint32_t failure = gesher::zygote::RestartWithUid(uid, &code);
    if (failure != 0) {
        char message[128];
        std::snprintf(message, sizeof(message), "Standalone VM restart failed: Win32 error %u", failure);
        jniThrowException(env, "java/lang/IllegalStateException", message);
        return 0;
    }
    // JNI/Java int retains all 32 bits, including an application's high-bit exit status.
    return static_cast<jint>(code);
}

static jlong Zygote_startTime(JNIEnv*, jclass, jint pid) {
    return gesher::zygote::StartTime(pid);
}

static jlong Zygote_childStartTime(JNIEnv*, jclass, jint pid) {
    return gesher::zygote::ChildStartTime(pid);
}

static jint Zygote_exitCode(JNIEnv*, jclass, jint pid) {
    return gesher::zygote::ExitCode(pid);
}

static jboolean Zygote_kill(JNIEnv*, jclass, jint pid) {
    return gesher::zygote::Kill(pid) ? JNI_TRUE : JNI_FALSE;
}

static jboolean Zygote_killChild(JNIEnv*, jclass, jint pid, jlong startTime) {
    return gesher::zygote::KillChild(pid, startTime) ? JNI_TRUE : JNI_FALSE;
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
    {"nativeProtectedRuntimeEnabled", "()Z", GESHER_JNI(Zygote_protectedRuntimeEnabled)},
    {"nativeStartProtectedRuntime", "(Ljava/lang/String;)I", GESHER_JNI(Zygote_startProtectedRuntime)},
    {"nativeSpawnProtected",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
     "IIJ[Ljava/lang/String;)I", GESHER_JNI(Zygote_spawnProtected)},
    {"nativeSpawn",
     "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Z)I",
     GESHER_JNI(Zygote_spawn)},
    {"nativeSpawnInAppContainer",
     "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;"
     "Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)I",
     GESHER_JNI(Zygote_spawnInAppContainer)},
    {"nativeEnsureAppContainer", "(Ljava/lang/String;Ljava/lang/String;)I",
     GESHER_JNI(Zygote_ensureAppContainer)},
    {"nativeGrantToAppContainer", "(Ljava/lang/String;Ljava/lang/String;IZ)I",
     GESHER_JNI(Zygote_grantToAppContainer)},
    {"nativeGrantToCapability", "(Ljava/lang/String;Ljava/lang/String;IZ)I",
     GESHER_JNI(Zygote_grantToCapability)},
    {"nativeDeleteAppContainer", "(Ljava/lang/String;)I", GESHER_JNI(Zygote_deleteAppContainer)},
    {"nativeSpawnCompiler", "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;II)I",
     GESHER_JNI(Zygote_spawnCompiler)},
    {"nativeWaitChild", "(IJIJZ)I", GESHER_JNI(Zygote_waitChild)},
    {"nativeAvailableMemory", "()J", GESHER_JNI(Zygote_availableMemory)},
    {"nativeRestartWithUid", "(I)I", GESHER_JNI(Zygote_restartWithUid)},
    {"nativeStartTime", "(I)J", GESHER_JNI(Zygote_startTime)},
    {"nativeChildStartTime", "(I)J", GESHER_JNI(Zygote_childStartTime)},
    {"nativeExitCode", "(I)I", GESHER_JNI(Zygote_exitCode)},
    {"nativeKill", "(I)Z", GESHER_JNI(Zygote_kill)},
    {"nativeKillChild", "(IJ)Z", GESHER_JNI(Zygote_killChild)},
    {"nativeJoinJob", "(I)Z", GESHER_JNI(Zygote_joinJob)},
    {"nativeLock", "(Ljava/lang/String;I)J", GESHER_JNI(Zygote_lock)},
    {"nativeUnlock", "(J)V", GESHER_JNI(Zygote_unlock)},
    {"nativeCreateEvent", "(Ljava/lang/String;)J", GESHER_JNI(Zygote_createEvent)},
    {"nativeWaitEvent", "(JI)Z", GESHER_JNI(Zygote_waitEvent)},
    {"nativeSignalEvent", "(Ljava/lang/String;)Z", GESHER_JNI(Zygote_signalEvent)},
    {"nativeWaitForBinder", "(Ljava/lang/String;II)Z", GESHER_JNI(Zygote_waitForBinder)},
    {"nativeCommandLine", "()Ljava/lang/String;", GESHER_JNI(Zygote_commandLine)},
    {"nativeExePath", "()Ljava/lang/String;", GESHER_JNI(Zygote_exePath)},
};

static void ProfileWatchError(JNIEnv* env, uint32_t error) {
    char message[128];
    std::snprintf(message, sizeof(message), "Profile directory notifications failed: Win32 error %u", error);
    jniThrowException(env, "java/io/IOException", message);
}
static jlong ProfileWatch_open(JNIEnv* env, jclass, jstring path) {
    uint32_t error = 0;
    uint64_t token = gesher::dexopt::OpenProfileWatch(ZygoteString(env, path), &error);
    if (token == 0) ProfileWatchError(env, error);
    return static_cast<jlong>(token);
}
static jobjectArray ProfileWatch_next(JNIEnv* env, jclass, jlong token) {
    std::vector<std::u16string> names;
    uint32_t error = 0;
    if (!gesher::dexopt::ReadProfileWatch(static_cast<uint64_t>(token), &names, &error)) {
        ProfileWatchError(env, error);
        return nullptr;
    }
    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr) return nullptr;
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(names.size()), string_class, nullptr);
    env->DeleteLocalRef(string_class);
    if (result == nullptr) return nullptr;
    for (size_t i = 0; i < names.size(); ++i) {
        jstring name = ZygoteJavaString(env, names[i]);
        if (name == nullptr) return nullptr;
        env->SetObjectArrayElement(result, static_cast<jsize>(i), name);
        env->DeleteLocalRef(name);
        if (env->ExceptionCheck()) return nullptr;
    }
    return result;
}
static void ProfileWatch_wake(JNIEnv*, jclass, jlong token) {
    gesher::dexopt::WakeProfileWatch(static_cast<uint64_t>(token));
}
static void ProfileWatch_close(JNIEnv*, jclass, jlong token) {
    gesher::dexopt::CloseProfileWatch(static_cast<uint64_t>(token));
}
static const JNINativeMethod gProfileWatchMethods[] = {
    {"open", "(Ljava/lang/String;)J", GESHER_JNI(ProfileWatch_open)},
    {"next", "(J)[Ljava/lang/String;", GESHER_JNI(ProfileWatch_next)},
    {"wake", "(J)V", GESHER_JNI(ProfileWatch_wake)},
    {"close", "(J)V", GESHER_JNI(ProfileWatch_close)},
};

int register_gesher_Zygote(JNIEnv* env) {
    RegisterGesherNatives(env, "com/gesher/server/Zygote", gZygoteMethods, NELEM(gZygoteMethods));
    return RegisterGesherNatives(env, "com/gesher/server/DexoptProfileNotifications",
                                gProfileWatchMethods, NELEM(gProfileWatchMethods));
}

}  // namespace android
