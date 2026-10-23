// Gesher: android.os.SharedMemory's natives (frameworks/base's
// core/jni/android_os_SharedMemory.cpp does the same over ashmem).
//
// A region is a memfd from gesher-base (a delete-on-close temporary file,
// which mmap maps with a Windows file mapping), as the NDK's ASharedMemory is
// in compat/bionic. The framework sends the system font map to an app this
// way (Typeface.serializeFontMap / setSystemFontMap, docs/fonts.md).
//
// ashmem's protection mask (nSetProt) is not modelled: a later mmap may still
// ask for more than the mask allows. The functions are JNICALL, which is
// System V under GESHER, as ART calls natives
// (docs/decisions/0006-managed-abi.md).

#include <errno.h>
#include <io.h>
#include <jni.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nativehelper/JNIHelp.h>

namespace {

jobject JNICALL SharedMemory_nCreate(JNIEnv* env, jclass, jstring jname, jint size) {
  if (size <= 0) {
    jniThrowErrnoException(env, "SharedMemory_create", EINVAL);
    return nullptr;
  }
  const char* name = jname != nullptr ? env->GetStringUTFChars(jname, nullptr) : nullptr;
  int fd = memfd_create(name != nullptr ? name : "SharedMemory", MFD_CLOEXEC);
  int err = fd < 0 ? errno : 0;
  if (name != nullptr) env->ReleaseStringUTFChars(jname, name);
  if (fd >= 0 && _chsize_s(fd, static_cast<long long>(size)) != 0) {
    err = ENOMEM;
    close(fd);
    fd = -1;
  }
  if (fd < 0) {
    jniThrowErrnoException(env, "SharedMemory_create", err);
    return nullptr;
  }
  jobject descriptor = jniCreateFileDescriptor(env, fd);
  if (descriptor == nullptr) close(fd);
  return descriptor;
}

jint JNICALL SharedMemory_nGetSize(JNIEnv* env, jclass, jobject descriptor) {
  int fd = jniGetFDFromFileDescriptor(env, descriptor);
  struct stat st {};
  if (fd < 0 || fstat(fd, &st) != 0) return -1;
  return st.st_size > INT_MAX ? INT_MAX : static_cast<jint>(st.st_size);
}

jint JNICALL SharedMemory_nSetProt(JNIEnv* env, jclass, jobject descriptor, jint prot) {
  if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) return EINVAL;
  return jniGetFDFromFileDescriptor(env, descriptor) < 0 ? EBADF : 0;
}

const JNINativeMethod kMethods[] = {
    {"nCreate", "(Ljava/lang/String;I)Ljava/io/FileDescriptor;",
     reinterpret_cast<void*>(&SharedMemory_nCreate)},
    {"nGetSize", "(Ljava/io/FileDescriptor;)I", reinterpret_cast<void*>(&SharedMemory_nGetSize)},
    {"nSetProt", "(Ljava/io/FileDescriptor;I)I", reinterpret_cast<void*>(&SharedMemory_nSetProt)},
};

}  // namespace

int register_gesher_shared_memory(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "android/os/SharedMemory", kMethods,
                                  sizeof(kMethods) / sizeof(kMethods[0]));
}
