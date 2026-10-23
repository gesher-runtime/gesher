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

#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <unistd.h>

#include "core_jni_helpers.h"

// Its layout checks use offsetof on a class with atomics, which Soong's flags
// let pass (-Wno-invalid-offsetof).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#include "com_android_internal_os_ApplicationSharedMemory.cpp"
#pragma clang diagnostic pop

namespace android {

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
    return RegisterMethodsOrDie(env, "com/android/internal/os/ApplicationSharedMemory",
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
    return RegisterMethodsOrDie(env, "com/android/internal/os/ClassLoaderFactory",
                                gClassLoaderFactoryMethods, NELEM(gClassLoaderFactoryMethods));
}

static void ActivityThread_noOp(JNIEnv*, jobject) {}

static const JNINativeMethod gActivityThreadMethods[] = {
    {"nPurgePendingResources", "()V", GESHER_JNI(ActivityThread_noOp)},
    {"nInitZygoteChildHeapProfiling", "()V", GESHER_JNI(ActivityThread_noOp)},
};

int register_gesher_ActivityThread(JNIEnv* env) {
    return RegisterMethodsOrDie(env, "android/app/ActivityThread", gActivityThreadMethods,
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
    return RegisterMethodsOrDie(env, "com/android/internal/os/DebugStore", gDebugStoreMethods,
                                NELEM(gDebugStoreMethods));
}

}  // namespace android
