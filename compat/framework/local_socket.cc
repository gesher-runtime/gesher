// LocalSocketImpl over Gesher's descriptor-based AF_UNIX stream backend.
// libcutils' Windows local-socket helpers and libbase's ancillary helpers are
// not built with working backends. Use the POSIX operations directly, reject
// unsupported namespaces/SCM_RIGHTS, and preserve actual peer-credential errors.
#include <errno.h>
#include <jni.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nativehelper/JNIHelp.h>
#include <nativehelper/JNIPlatformHelp.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include <nativehelper/ScopedUtfChars.h>

namespace {
jfieldID outboundDescriptors;
jclass credentialsClass;
jmethodID credentialsConstructor;

bool Address(JNIEnv* env, jstring name, jint space, sockaddr_un* address, socklen_t* length) {
    if (name == nullptr) {
        jniThrowNullPointerException(env, "name");
        return false;
    }
    // FILESYSTEM=2 is backed by Winsock AF_UNIX. Abstract/reserved Android
    // namespaces do not have their lifetime/authority semantics on this host.
    if (space != 2) {
        jniThrowIOException(env, (space == 0 || space == 1) ? EOPNOTSUPP : EINVAL);
        return false;
    }
    ScopedUtfChars chars(env, name);
    if (chars.c_str() == nullptr) return false;
    size_t size = strlen(chars.c_str());
    if (size == 0 || size >= sizeof(address->sun_path)) {
        jniThrowIOException(env, size == 0 ? EINVAL : ENAMETOOLONG);
        return false;
    }
    *address = {};
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, chars.c_str(), size + 1);
    *length = offsetof(sockaddr_un, sun_path) + size + 1;
    return true;
}

void JNICALL Connect(JNIEnv* env, jobject, jobject descriptor, jstring name, jint space) {
    sockaddr_un address;
    socklen_t length;
    if (!Address(env, name, space, &address, &length)) return;
    int fd = jniGetFDFromFileDescriptor(env, descriptor);
    if (env->ExceptionCheck()) return;
    if (TEMP_FAILURE_RETRY(connect(fd, reinterpret_cast<sockaddr*>(&address), length)) < 0)
        jniThrowIOException(env, errno);
}

void JNICALL Bind(JNIEnv* env, jobject, jobject descriptor, jstring name, jint space) {
    sockaddr_un address;
    socklen_t length;
    if (!Address(env, name, space, &address, &length)) return;
    int fd = jniGetFDFromFileDescriptor(env, descriptor);
    if (env->ExceptionCheck()) return;
    // Match AOSP's filesystem namespace: remove a stale endpoint before bind.
    unlink(address.sun_path);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), length) < 0)
        jniThrowIOException(env, errno);
}

bool Bounds(JNIEnv* env, jbyteArray bytes, jint offset, jint length) {
    if (bytes == nullptr) {
        jniThrowNullPointerException(env, "buffer");
        return false;
    }
    if (offset < 0 || length < 0 || offset > env->GetArrayLength(bytes) - length) {
        jniThrowException(env, "java/lang/ArrayIndexOutOfBoundsException", nullptr);
        return false;
    }
    return true;
}

bool Writable(JNIEnv* env, jobject self) {
    auto fds = static_cast<jobjectArray>(env->GetObjectField(self, outboundDescriptors));
    if (env->ExceptionCheck()) return false;
    bool hasDescriptors = fds != nullptr && env->GetArrayLength(fds) != 0;
    if (fds != nullptr) env->DeleteLocalRef(fds);
    if (hasDescriptors) jniThrowIOException(env, EOPNOTSUPP);
    return !hasDescriptors;
}

jint JNICALL ReadByte(JNIEnv* env, jobject, jobject descriptor) {
    int fd = jniGetFDFromFileDescriptor(env, descriptor);
    if (env->ExceptionCheck()) return -1;
    unsigned char byte;
    ssize_t count = TEMP_FAILURE_RETRY(recv(fd, &byte, 1, 0));
    if (count < 0) jniThrowIOException(env, errno);
    return count > 0 ? byte : -1;
}

jint JNICALL ReadBytes(JNIEnv* env, jobject, jbyteArray bytes, jint offset, jint length,
                       jobject descriptor) {
    if (!Bounds(env, bytes, offset, length)) return -1;
    int fd = jniGetFDFromFileDescriptor(env, descriptor);
    if (env->ExceptionCheck()) return -1;
    if (length == 0) return 0;
    ScopedByteArrayRW buffer(env, bytes);
    if (buffer.get() == nullptr) return -1;
    ssize_t count = TEMP_FAILURE_RETRY(recv(fd, buffer.get() + offset, length, 0));
    if (count < 0) jniThrowIOException(env, errno);
    return count > 0 ? static_cast<jint>(count) : -1;
}

void WriteAll(JNIEnv* env, int fd, const void* data, size_t length) {
    auto bytes = static_cast<const unsigned char*>(data);
    while (length != 0) {
        ssize_t sent = TEMP_FAILURE_RETRY(send(fd, bytes, length, MSG_NOSIGNAL));
        if (sent <= 0) {
            jniThrowIOException(env, sent < 0 ? errno : EIO);
            return;
        }
        bytes += sent;
        length -= sent;
    }
}

void JNICALL WriteByte(JNIEnv* env, jobject self, jint value, jobject descriptor) {
    int fd = jniGetFDFromFileDescriptor(env, descriptor);
    if (env->ExceptionCheck() || !Writable(env, self)) return;
    unsigned char byte = static_cast<unsigned char>(value);
    WriteAll(env, fd, &byte, 1);
}

void JNICALL WriteBytes(JNIEnv* env, jobject self, jbyteArray bytes, jint offset, jint length,
                        jobject descriptor) {
    if (!Bounds(env, bytes, offset, length)) return;
    int fd = jniGetFDFromFileDescriptor(env, descriptor);
    if (env->ExceptionCheck() || !Writable(env, self) || length == 0) return;
    ScopedByteArrayRO buffer(env, bytes);
    if (buffer.get() != nullptr) WriteAll(env, fd, buffer.get() + offset, length);
}

jobject JNICALL PeerCredentials(JNIEnv* env, jobject, jobject descriptor) {
    int fd = jniGetFDFromFileDescriptor(env, descriptor);
    if (env->ExceptionCheck()) return nullptr;
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0) {
        jniThrowIOException(env, errno);
        return nullptr;
    }
    if (size != sizeof(credentials)) {
        jniThrowIOException(env, EIO);
        return nullptr;
    }
    return env->NewObject(credentialsClass, credentialsConstructor,
                          credentials.pid, credentials.uid, credentials.gid);
}
}  // namespace

namespace android {
int register_gesher_LocalSocketImpl(JNIEnv* env) {
    jclass cls = env->FindClass("android/net/LocalSocketImpl");
    if (cls == nullptr) return -1;
    outboundDescriptors = env->GetFieldID(cls, "outboundFileDescriptors", "[Ljava/io/FileDescriptor;");
    env->DeleteLocalRef(cls);
    if (outboundDescriptors == nullptr) return -1;
    jclass credentials = env->FindClass("android/net/Credentials");
    if (credentials == nullptr) return -1;
    credentialsConstructor = env->GetMethodID(credentials, "<init>", "(III)V");
    credentialsClass = static_cast<jclass>(env->NewGlobalRef(credentials));
    env->DeleteLocalRef(credentials);
    if (credentialsConstructor == nullptr || credentialsClass == nullptr) return -1;
    static const JNINativeMethod kMethods[] = {
        {"connectLocal", "(Ljava/io/FileDescriptor;Ljava/lang/String;I)V", reinterpret_cast<void*>(&Connect)},
        {"bindLocal", "(Ljava/io/FileDescriptor;Ljava/lang/String;I)V", reinterpret_cast<void*>(&Bind)},
        {"read_native", "(Ljava/io/FileDescriptor;)I", reinterpret_cast<void*>(&ReadByte)},
        {"readba_native", "([BIILjava/io/FileDescriptor;)I", reinterpret_cast<void*>(&ReadBytes)},
        {"write_native", "(ILjava/io/FileDescriptor;)V", reinterpret_cast<void*>(&WriteByte)},
        {"writeba_native", "([BIILjava/io/FileDescriptor;)V", reinterpret_cast<void*>(&WriteBytes)},
        {"getPeerCredentials_native", "(Ljava/io/FileDescriptor;)Landroid/net/Credentials;",
         reinterpret_cast<void*>(&PeerCredentials)},
    };
    return jniRegisterNativeMethods(env, "android/net/LocalSocketImpl", kMethods,
                                    sizeof(kMethods) / sizeof(kMethods[0]));
}
}  // namespace android
