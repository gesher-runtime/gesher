// Gesher has neither Linux IncFS nor kernel-enforced fs-verity. These are
// capability/metadata queries, not APK verification: returning no IncFS
// signature leaves AOSP's v4/.idsig and v3/v2/v1 verification in charge.
// In particular, a sidecar must never be reported as kernel-verified metadata.

#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedUtfChars.h>

namespace {

jboolean JNICALL Incremental_disabled(JNIEnv*, jclass) { return JNI_FALSE; }
jboolean JNICALL Incremental_path(JNIEnv*, jclass, jstring) { return JNI_FALSE; }
jboolean JNICALL Incremental_fd(JNIEnv*, jclass, jint) { return JNI_FALSE; }
jbyteArray JNICALL Incremental_signature(JNIEnv*, jclass, jstring) { return nullptr; }

// Preserve real lookup failures. Android's enable methods return positive
// errno; its stat/measure methods return negative errno. Never write a digest
// buffer or report that enabling integrity protection succeeded.
int OpenIntegrityFile(JNIEnv* env, jstring filePath) {
  ScopedUtfChars path(env, filePath);
  if (path.c_str() == nullptr) {
    errno = EINVAL;
    return -1;
  }
  // A capability probe must not wait for a writer if passed a FIFO.
  return open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
}

jint JNICALL Verity_enableFd(JNIEnv*, jclass, jint fd) {
  struct stat st {};
  if (fstat(fd, &st) != 0) return errno;
  return EOPNOTSUPP;
}

jint JNICALL Verity_enable(JNIEnv* env, jclass type, jstring filePath) {
  int fd = OpenIntegrityFile(env, filePath);
  if (fd < 0) return errno;
  int result = Verity_enableFd(env, type, fd);
  close(fd);
  return result;
}

jint JNICALL Verity_stat(JNIEnv* env, jclass, jstring filePath) {
  ScopedUtfChars path(env, filePath);
  if (path.c_str() == nullptr) return -EINVAL;
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return -errno;
  return 0;  // The file exists and has no fs-verity attribute.
}

jint JNICALL Verity_measure(JNIEnv* env, jclass, jstring filePath, jbyteArray) {
  int fd = OpenIntegrityFile(env, filePath);
  if (fd < 0) return -errno;
  close(fd);
  return -EOPNOTSUPP;
}

const JNINativeMethod kIncrementalMethods[] = {
    {"nativeIsEnabled", "()Z", reinterpret_cast<void*>(&Incremental_disabled)},
    {"nativeIsV2Available", "()Z", reinterpret_cast<void*>(&Incremental_disabled)},
    {"nativeIsIncrementalPath", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(&Incremental_path)},
    {"nativeIsIncrementalFd", "(I)Z", reinterpret_cast<void*>(&Incremental_fd)},
    {"nativeUnsafeGetFileSignature", "(Ljava/lang/String;)[B",
     reinterpret_cast<void*>(&Incremental_signature)},
};
const JNINativeMethod kVerityMethods[] = {
    {"enableFsverityNative", "(Ljava/lang/String;)I", reinterpret_cast<void*>(&Verity_enable)},
    {"enableFsverityForFdNative", "(I)I", reinterpret_cast<void*>(&Verity_enableFd)},
    {"statxForFsverityNative", "(Ljava/lang/String;)I", reinterpret_cast<void*>(&Verity_stat)},
    {"measureFsverityNative", "(Ljava/lang/String;[B)I", reinterpret_cast<void*>(&Verity_measure)},
};

}  // namespace

int register_gesher_incremental(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "android/os/incremental/IncrementalManager",
                                  kIncrementalMethods,
                                  sizeof(kIncrementalMethods) / sizeof(kIncrementalMethods[0]));
}

int register_gesher_verity(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "com/android/internal/security/VerityUtils", kVerityMethods,
                                  sizeof(kVerityMethods) / sizeof(kVerityMethods[0]));
}
