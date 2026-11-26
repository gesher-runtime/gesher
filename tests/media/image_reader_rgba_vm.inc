// Isolated JNI attachment adapter; production QueueRgba is compiled unchanged.
constexpr int JNI_VERSION_1_6 = 0x10006;
constexpr int JNI_EDETACHED = -2;
constexpr int JNI_OK = 0;
struct JavaVM {
  bool detached = false, fail_attach = false, empty_environment = false;
  int lookups = 0, attaches = 0, detaches = 0;
  int GetEnv(void** out, int version) {
    Check(version == JNI_VERSION_1_6, "RGBA JNI version retained");
    ++lookups;
    *out = detached || empty_environment ? nullptr : current_env;
    return detached ? JNI_EDETACHED : JNI_OK;
  }
  int AttachCurrentThreadAsDaemon(JNIEnv** out, void*) {
    ++attaches;
    if (fail_attach) return -1;
    detached = false;
    *out = empty_environment ? nullptr : current_env;
    return JNI_OK;
  }
  int DetachCurrentThread() {
    ++detaches;
    detached = true;
    return JNI_OK;
  }
} reader_vm;
