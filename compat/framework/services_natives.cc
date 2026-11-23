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
#include <map>
#include <mutex>
#include <gesher/system_properties.h>

#include "core_jni_helpers.h"
#include "cohost_permission_server.h"
#include "../posix/src/standalone_input.h"
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
                         jobjectArray unset, jstring logPath, jboolean breakaway) {
    return gesher::zygote::Spawn(ZygoteString(env, commandLine), ZygoteStrings(env, set),
                                 ZygoteStrings(env, unset), ZygoteString(env, logPath),
                                 breakaway == JNI_TRUE);
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

static jintArray Zygote_startProtectedDevice(JNIEnv* env, jclass, jstring device,
                                             jobjectArray commands, jobjectArray logs,
                                             jobjectArray environment, jboolean scoped) {
    auto started = gesher::zygote::StartProtectedDevice(ZygoteString(env, device),
        ZygoteStrings(env, commands), ZygoteStrings(env, logs), ZygoteStrings(env, environment),
        scoped == JNI_TRUE);
    jintArray result = env->NewIntArray(static_cast<jsize>(started.size()));
    if (result != nullptr && !started.empty()) {
        std::vector<jint> pids(started.begin(), started.end());
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(pids.size()), pids.data());
    }
    return result;
}

static void Zygote_finishScopedStandardStreams(JNIEnv*, jclass) {
    gesher::zygote::FinishScopedStandardStreams();
}

static jboolean Zygote_hasProtectedLaunchAuthority(JNIEnv*, jclass, jboolean scoped) {
    return gesher::zygote::HasProtectedLaunchAuthority(scoped == JNI_TRUE);
}

static jboolean Zygote_prepareProtectedOperator(JNIEnv*, jclass, jint pid, jlong birth) {
    return birth > 0 && gesher::zygote::PrepareProtectedOperator(pid, static_cast<uint64_t>(birth));
}

static std::array<uint8_t, 16> ZygoteInstallation(jlong high, jlong low) {
    std::array<uint8_t, 16> id{};
    for (unsigned shift = 0; shift < 64; shift += 8) {
        id[shift / 8] = static_cast<uint8_t>(static_cast<uint64_t>(high) >> shift);
        id[8 + shift / 8] = static_cast<uint8_t>(static_cast<uint64_t>(low) >> shift);
    }
    return id;
}

static jint Zygote_spawnProtectedApp(JNIEnv* env, jclass, jstring commandLine,
                                     jobjectArray set, jobjectArray unset, jstring logPath,
                                     jstring name, jstring displayName, jobjectArray capabilities,
                                     jint uid, jlong installHigh, jlong installLow, jlong startSeq,
                                     jlong pin) {
    gesher::zygote::AppContainerSpec spec{
        ZygoteString(env, name), ZygoteString(env, displayName), ZygoteStrings(env, capabilities)};
    return gesher::zygote::SpawnProtectedApp(ZygoteString(env, commandLine),
        ZygoteStrings(env, set), ZygoteStrings(env, unset), ZygoteString(env, logPath),
        spec, static_cast<uint32_t>(uid), ZygoteInstallation(installHigh, installLow),
        startSeq > 0 ? static_cast<uint64_t>(startSeq) : 0, static_cast<uint64_t>(pin));
}

static jboolean Zygote_changeProtectedInstallation(JNIEnv*, jclass, jlong high, jlong low,
                                                     jboolean revoke) {
    return gesher::zygote::ChangeProtectedInstallation(ZygoteInstallation(high, low),
                                                        revoke == JNI_TRUE);
}

static jboolean Zygote_stopProtectedInstallation(JNIEnv*, jclass, jlong high, jlong low,
                                                   jlong stopThrough) {
    return stopThrough >= 0 && gesher::zygote::StopProtectedInstallation(
        ZygoteInstallation(high, low), static_cast<uint64_t>(stopThrough));
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

static jint Zygote_grantPropertyAreaRead(JNIEnv* env, jclass, jstring name) {
    // Package launches can overlap. Grant performs a DACL read/modify/write on
    // this shared file, so serializing the grants preserves every package ACE.
    static std::mutex grants;
    std::lock_guard lock(grants);
    std::wstring path = gesher::properties::ReadOnlyAreaPath();
    if (path.empty()) return 2;  // ERROR_FILE_NOT_FOUND
    constexpr uint32_t kRead = 0x00120089;  // FILE_GENERIC_READ
    return static_cast<jint>(gesher::zygote::GrantToAppContainer(
        std::u16string(path.begin(), path.end()), ZygoteString(env, name), kRead, false));
}

static jint Zygote_deleteAppContainer(JNIEnv* env, jclass, jstring name) {
    return static_cast<jint>(gesher::zygote::DeleteAppContainer(ZygoteString(env, name)));
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

static jlong Zygote_pinStandaloneInput(JNIEnv* env, jclass, jstring code) {
    uint64_t pin = 0;
    const uint32_t failure = gesher::zygote::PinStandaloneInput(ZygoteString(env, code), &pin);
    if (failure != 0) {
        char message[128];
        std::snprintf(message, sizeof(message), "Cannot pin standalone code: Win32 error %u", failure);
        jniThrowException(env, "java/lang/IllegalStateException", message);
    }
    return static_cast<jlong>(pin);
}

static void Zygote_releaseStandaloneInput(JNIEnv*, jclass, jlong pin) {
    if (pin > 0) gesher::zygote::ReleaseStandaloneInput(static_cast<uint64_t>(pin));
}

static jint Zygote_readStandaloneInput(JNIEnv* env, jclass, jlong pin, jlong offset,
                                       jbyteArray target) {
    const jsize requested = target == nullptr ? 0 : env->GetArrayLength(target);
    if (pin <= 0 || offset < 0 || requested <= 0 || requested > 128 * 1024) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "Invalid pinned code read");
        return 0;
    }
    std::vector<jbyte> bytes(static_cast<size_t>(requested));
    uint32_t copied = 0;
    const uint32_t failure = gesher::zygote::ReadStandaloneInput(
        static_cast<uint64_t>(pin), offset, bytes.data(), static_cast<uint32_t>(requested), &copied);
    if (failure != 0) {
        char message[128];
        std::snprintf(message, sizeof(message), "Cannot read pinned code: Win32 error %u", failure);
        jniThrowException(env, "java/io/IOException", message);
        return 0;
    }
    if (copied != 0) env->SetByteArrayRegion(target, 0, static_cast<jsize>(copied), bytes.data());
    return static_cast<jint>(copied);
}

static jint Zygote_runContainedStandalone(JNIEnv* env, jclass, jstring command, jstring name,
                                           jstring display, jobjectArray capabilities,
                                           jlong pin, jint uid) {
    gesher::zygote::AppContainerSpec spec{ZygoteString(env, name), ZygoteString(env, display),
                                        ZygoteStrings(env, capabilities)};
    uint32_t exit = 0;
    if (pin <= 0) {
        jniThrowException(env, "java/lang/IllegalStateException", "Contained code is not pinned");
        return 0;
    }
    const uint32_t failure = gesher::zygote::RunPinnedContainedStandalone(
        ZygoteString(env, command), spec, static_cast<uint64_t>(pin), static_cast<uint32_t>(uid), &exit);
    if (failure != 0) {
        char message[128];
        std::snprintf(message, sizeof(message), "Contained program launch failed: Win32 error %lu",
                      static_cast<unsigned long>(failure));
        jniThrowException(env, "java/lang/IllegalStateException", message);
    }
    return static_cast<jint>(exit);
}

namespace {
namespace cohost = gesher::cohost_permissions;
using gesher::binder_peer::OwnedHandle;
std::mutex gCohostLeasesLock;
std::map<jlong, OwnedHandle> gCohostLeases;
uint64_t gNextCohostLease = 1;

class CohostLease {
public:
    ~CohostLease() {
        std::lock_guard<std::mutex> lock(gCohostLeasesLock);
        gCohostLeases.erase(id_);
    }

    jlong attach(HANDLE process) {
        HANDLE retained = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &retained,
                             SYNCHRONIZE, FALSE, 0)) return 0;
        OwnedHandle owner(retained);
        std::lock_guard<std::mutex> lock(gCohostLeasesLock);
        if (id_ != 0 || gNextCohostLease > INT64_MAX) return 0;
        id_ = static_cast<jlong>(gNextCohostLease++);
        gCohostLeases.emplace(id_, std::move(owner));
        return id_;
    }

private:
    jlong id_ = 0;
};

DWORD DispatchCohostPermission(JavaVM* vm, jobject host, jmethodID dispatch,
        cohost::Operation operation, const std::vector<std::u16string>& names,
        std::vector<int32_t>* values) {
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return ERROR_GEN_FAILURE;
    DWORD error = ERROR_NO_SYSTEM_RESOURCES;
    jclass string = env->FindClass("java/lang/String");
    jobjectArray arguments = string == nullptr ? nullptr
        : env->NewObjectArray(static_cast<jsize>(names.size()), string, nullptr);
    if (arguments != nullptr) {
        for (jsize i = 0; i < static_cast<jsize>(names.size()) && !env->ExceptionCheck(); ++i) {
            const auto& name = names[i];
            if (name.empty()) continue;
            jstring argument = env->NewString(reinterpret_cast<const jchar*>(name.data()),
                                               static_cast<jsize>(name.size()));
            if (argument == nullptr) break;
            env->SetObjectArrayElement(arguments, i, argument);
            env->DeleteLocalRef(argument);
        }
        if (!env->ExceptionCheck()) {
            auto result = static_cast<jintArray>(
                env->CallObjectMethod(host, dispatch, static_cast<jint>(operation), arguments));
            if (!env->ExceptionCheck() && result != nullptr &&
                env->GetArrayLength(result) == static_cast<jsize>(names.size())) {
                values->resize(names.size());
                if (!values->empty()) env->GetIntArrayRegion(result, 0,
                    static_cast<jsize>(values->size()), values->data());
                if (!env->ExceptionCheck()) error = ERROR_SUCCESS;
            }
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        error = ERROR_ACCESS_DENIED;
    }
    vm->DetachCurrentThread();
    return error;
}
}  // namespace

static jboolean Zygote_cohostPermissionLive(JNIEnv*, jclass, jlong lease) {
    if (lease <= 0) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(gCohostLeasesLock);
    auto entry = gCohostLeases.find(lease);
    return entry != gCohostLeases.end() &&
        WaitForSingleObject(entry->second.get(), 0) == WAIT_TIMEOUT ? JNI_TRUE : JNI_FALSE;
}

static jboolean Zygote_hasCohostPermissionScope(JNIEnv*, jclass) {
    const auto& boot = gesher::cohost_bootstrap::Current();
    return boot && !boot.permission_pipe.empty() &&
        WaitForSingleObject(boot.storage_host, 0) == WAIT_TIMEOUT ? JNI_TRUE : JNI_FALSE;
}

static jboolean Zygote_cohostOwnsApk(JNIEnv* env, jclass type, jstring path) {
    if (Zygote_hasCohostPermissionScope(env, type) != JNI_TRUE) return JNI_FALSE;
    const auto value = ZygoteString(env, path);
    const std::wstring native(value.begin(), value.end());
    return gesher::standalone_input::Current().Matches(native) ? JNI_TRUE : JNI_FALSE;
}

static jintArray Zygote_cohostPermissionExchange(JNIEnv* env, jclass, jint operation,
                                                  jobjectArray names) {
    if (names == nullptr || env->GetArrayLength(names) > static_cast<jsize>(cohost::kMaxNames)) return nullptr;
    std::vector<int32_t> values;
    if (cohost::Exchange(static_cast<cohost::Operation>(operation), ZygoteStrings(env, names),
                         &values) != ERROR_SUCCESS) return nullptr;
    jintArray result = env->NewIntArray(static_cast<jsize>(values.size()));
    if (result != nullptr && !values.empty()) {
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    }
    return result;
}

static jint Zygote_runPinnedCohost(JNIEnv* env, jclass, jstring command, jstring name,
        jstring display, jobjectArray capabilities, jlong pin, jint uid, jobject permissions,
        jstring credential, jstring device, jstring storagePipe) {
    if (pin <= 0 || permissions == nullptr || !gesher::zygote::HasProtectedLaunchAuthority()) {
        jniThrowException(env, "java/lang/SecurityException", "Protected cohost cannot launch");
        return 0;
    }
    jclass type = env->GetObjectClass(permissions);
    if (type == nullptr) return 0;
    jmethodID bind = env->GetMethodID(type, "bindNativeLease", "(J)V");
    if (bind == nullptr) return 0;
    jmethodID dispatch = env->GetMethodID(type, "dispatch", "(I[Ljava/lang/String;)[I");
    if (dispatch == nullptr) return 0;
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) {
        jniThrowException(env, "java/lang/IllegalStateException", "Cannot attach host consent");
        return 0;
    }
    jobject host = env->NewGlobalRef(permissions);
    if (host == nullptr) return 0;
    uint32_t exit = 0;
    uint32_t failure = ERROR_GEN_FAILURE;
    {
        CohostLease lease;
        cohost::Server authority([&](cohost::Operation operation,
                const std::vector<std::u16string>& names, std::vector<int32_t>* values) {
            return DispatchCohostPermission(vm, host, dispatch, operation, names, values);
        });
        gesher::zygote::CohostSpec services{ZygoteString(env, credential), ZygoteString(env, device),
            ZygoteString(env, storagePipe), std::u16string(authority.name().begin(), authority.name().end()),
            [&](void* process, void* job) -> uint32_t {
                const jlong leaseId = lease.attach(static_cast<HANDLE>(process));
                if (leaseId == 0) return ERROR_ACCESS_DENIED;
                env->CallVoidMethod(host, bind, leaseId);
                if (env->ExceptionCheck()) return ERROR_ACCESS_DENIED;
                return authority.Start(static_cast<HANDLE>(process), static_cast<HANDLE>(job));
            }};
        gesher::zygote::AppContainerSpec spec{ZygoteString(env, name), ZygoteString(env, display),
                                             ZygoteStrings(env, capabilities)};
        failure = gesher::zygote::RunPinnedCohost(ZygoteString(env, command), spec,
            static_cast<uint64_t>(pin), static_cast<uint32_t>(uid), services, &exit);
    }
    env->DeleteGlobalRef(host);
    if (failure != 0 && !env->ExceptionCheck()) {
        char message[128];
        std::snprintf(message, sizeof(message), "Standalone application launch failed: Win32 error %u",
                      failure);
        jniThrowException(env, "java/lang/IllegalStateException", message);
    }
    return static_cast<jint>(exit);
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
    {"nativeSpawn",
     "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Z)I",
     GESHER_JNI(Zygote_spawn)},
    {"nativeSpawnInAppContainer",
     "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;"
     "Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)I",
     GESHER_JNI(Zygote_spawnInAppContainer)},
    {"nativeStartProtectedDevice",
     "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Z)[I",
     GESHER_JNI(Zygote_startProtectedDevice)},
    {"finishScopedStandardStreams", "()V", GESHER_JNI(Zygote_finishScopedStandardStreams)},
    {"nativeHasProtectedLaunchAuthority", "(Z)Z", GESHER_JNI(Zygote_hasProtectedLaunchAuthority)},
    {"nativePrepareProtectedOperator", "(IJ)Z", GESHER_JNI(Zygote_prepareProtectedOperator)},
    {"nativeSpawnProtectedApp",
     "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;"
     "Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;IJJJJ)I",
     GESHER_JNI(Zygote_spawnProtectedApp)},
    {"nativeChangeProtectedInstallation", "(JJZ)Z", GESHER_JNI(Zygote_changeProtectedInstallation)},
    {"nativeStopProtectedInstallation", "(JJJ)Z", GESHER_JNI(Zygote_stopProtectedInstallation)},
    {"nativeEnsureAppContainer", "(Ljava/lang/String;Ljava/lang/String;)I",
     GESHER_JNI(Zygote_ensureAppContainer)},
    {"nativeGrantToAppContainer", "(Ljava/lang/String;Ljava/lang/String;IZ)I",
     GESHER_JNI(Zygote_grantToAppContainer)},
    {"nativeGrantToCapability", "(Ljava/lang/String;Ljava/lang/String;IZ)I",
     GESHER_JNI(Zygote_grantToCapability)},
    {"nativeGrantPropertyAreaRead", "(Ljava/lang/String;)I", GESHER_JNI(Zygote_grantPropertyAreaRead)},
    {"nativeDeleteAppContainer", "(Ljava/lang/String;)I", GESHER_JNI(Zygote_deleteAppContainer)},
    {"nativeRestartWithUid", "(I)I", GESHER_JNI(Zygote_restartWithUid)},
    {"nativePinStandaloneInput", "(Ljava/lang/String;)J", GESHER_JNI(Zygote_pinStandaloneInput)},
    {"nativeReleaseStandaloneInput", "(J)V", GESHER_JNI(Zygote_releaseStandaloneInput)},
    {"nativeReadStandaloneInput", "(JJ[B)I", GESHER_JNI(Zygote_readStandaloneInput)},
    {"nativeRunContainedStandalone",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;JI)I",
     GESHER_JNI(Zygote_runContainedStandalone)},
    {"nativeRunPinnedCohost",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;JI"
     "Lcom/gesher/server/CohostPermissionHost;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
     GESHER_JNI(Zygote_runPinnedCohost)},
    {"nativeCohostPermissionLive", "(J)Z", GESHER_JNI(Zygote_cohostPermissionLive)},
    {"nativeHasCohostPermissionScope", "()Z", GESHER_JNI(Zygote_hasCohostPermissionScope)},
    {"nativeCohostOwnsApk", "(Ljava/lang/String;)Z", GESHER_JNI(Zygote_cohostOwnsApk)},
    {"nativeCohostPermissionExchange", "(I[Ljava/lang/String;)[I", GESHER_JNI(Zygote_cohostPermissionExchange)},
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

int register_gesher_Zygote(JNIEnv* env) {
    return RegisterGesherNatives(env, "com/gesher/server/Zygote", gZygoteMethods,
                                NELEM(gZygoteMethods));
}

}  // namespace android
