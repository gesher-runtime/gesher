// gesher: libandroid.so for an app's ELF libraries: the NDK's non-graphics
// platform APIs as System V entry points in the bionic shim's symbol table
// (an app's `DT_NEEDED libandroid.so` is satisfied symbol by symbol, like
// libc.so), implemented over what Gesher already runs: libandroidfw and the
// framework's AssetManager natives (android_util_AssetManager.cpp), libutils'
// Looper (over gesher-base's epoll and eventfd), and androidfw's ObbFile.
// Linked into dalvikvm (compat/bionic/jni.cmake), where the framework's
// natives are.
//
// What is here, after frameworks/base/native/android:
//  - AAssetManager_fromJava / AAssetManager_open/openDir, AAsset_*,
//    AAssetDir_*: assets of the APK through the Java AssetManager's native
//    AssetManager2 (the .so's JNIEnv is the bridge's proxy; the real one is
//    behind it).
//  - AConfiguration_*: a ResTable_config, as on Android; fromAssetManager
//    reads the AssetManager2's configuration.
//  - ALooper_*: libutils' Looper itself (an ALooper* is a Looper*, as on
//    Android); ALooper_addFd's System V callback is wrapped in a
//    LooperCallback that sets up the thread for loaded code first.
//  - ASensorManager: a device without sensors (an empty list; queues that
//    never deliver events).
//  - ATrace_*: tracing is off (no-ops; isEnabled is false).
//  - AStorageManager / AObbScanner: OBB files are scanned with androidfw's
//    ObbFile; mounting is not supported (the callback reports
//    AOBB_STATE_ERROR_INTERNAL from another thread, as the platform reports
//    asynchronously).
//  - ASharedMemory_create/getSize/setProt over gesher-base's memfd_create.
//  - ANativeWindow_* and ANativeActivity_*: stand-ins for the graphics track,
//    which errors or no-ops that say so once (see docs/native-libs.md for the
//    interface they need).

#include "jni_bridge.h"
#include "shim_internal.h"

#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <android/asset_manager.h>
#include <android_runtime/android_util_AssetManager.h>
#include <androidfw/Asset.h>
#include <androidfw/AssetDir.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/MutexGuard.h>
#include <androidfw/ObbFile.h>
#include <androidfw/ResourceTypes.h>
#include <utils/Looper.h>

#include <errno.h>
#include <io.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace gesher::bionic {

namespace {

using android::Asset;
using android::AssetDir;
using android::AssetManager2;
using android::Looper;
using android::ResTable_config;
using android::ScopedLock;

void LogOnce(std::atomic<bool>* said, const char* what) {
  if (!said->exchange(true)) {
    std::fprintf(stderr, "gesher libandroid: %s\n", what);
    std::fflush(stderr);
  }
}

// --- AAssetManager, AAsset, AAssetDir ---------------------------------------------

struct NdkAsset {
  std::unique_ptr<Asset> asset;
};
struct NdkAssetDir {
  std::unique_ptr<AssetDir> dir;
  size_t index = 0;
  android::String8 cached_name;
};

android::Guarded<AssetManager2>* GuardedOf(AAssetManager* am) {
  return android::AssetManagerForNdkAssetManager(am);
}

AAssetManager* BIONIC_SYSV AssetManagerFromJava(JNIEnv* env, jobject am) {
  return android::NdkAssetManagerForJavaObject(RealJniEnv(env), am);
}

NdkAsset* BIONIC_SYSV AssetManagerOpen(AAssetManager* am, const char* name, int mode) {
  Asset::AccessMode access;
  switch (mode) {
    case 0: access = Asset::ACCESS_UNKNOWN; break;    // AASSET_MODE_UNKNOWN
    case 1: access = Asset::ACCESS_RANDOM; break;     // AASSET_MODE_RANDOM
    case 2: access = Asset::ACCESS_STREAMING; break;  // AASSET_MODE_STREAMING
    case 3: access = Asset::ACCESS_BUFFER; break;     // AASSET_MODE_BUFFER
    default: return nullptr;
  }
  if (am == nullptr || name == nullptr) return nullptr;
  ScopedLock<AssetManager2> locked(*GuardedOf(am));
  std::unique_ptr<Asset> asset = locked->Open(name, access);
  if (asset == nullptr) return nullptr;
  return new NdkAsset{std::move(asset)};
}

NdkAssetDir* BIONIC_SYSV AssetManagerOpenDir(AAssetManager* am, const char* name) {
  if (am == nullptr || name == nullptr) return nullptr;
  ScopedLock<AssetManager2> locked(*GuardedOf(am));
  std::unique_ptr<AssetDir> dir = locked->OpenDir(name);
  if (dir == nullptr) return nullptr;
  auto* d = new NdkAssetDir();
  d->dir = std::move(dir);
  return d;
}

// Regular files only, as AOSP's: directories are never reported.
const char* BIONIC_SYSV AssetDirGetNextFileName(NdkAssetDir* d) {
  size_t max = d->dir->getFileCount();
  while (d->index < max && d->dir->getFileType(d->index) != android::kFileTypeRegular) ++d->index;
  if (d->index >= max) return nullptr;
  d->cached_name = d->dir->getFileName(d->index++);
  return d->cached_name.c_str();
}
void BIONIC_SYSV AssetDirRewind(NdkAssetDir* d) { d->index = 0; }
void BIONIC_SYSV AssetDirClose(NdkAssetDir* d) { delete d; }

int BIONIC_SYSV AssetRead(NdkAsset* a, void* buf, size_t n) {
  return static_cast<int>(a->asset->read(buf, n));
}
int64_t BIONIC_SYSV AssetSeek(NdkAsset* a, int64_t offset, int whence) {
  return a->asset->seek(offset, whence);
}
void BIONIC_SYSV AssetClose(NdkAsset* a) {
  a->asset->close();
  delete a;
}
const void* BIONIC_SYSV AssetGetBuffer(NdkAsset* a) { return a->asset->getBuffer(false); }
int64_t BIONIC_SYSV AssetGetLength(NdkAsset* a) { return a->asset->getLength(); }
int64_t BIONIC_SYSV AssetGetRemainingLength(NdkAsset* a) {
  return a->asset->getRemainingLength();
}
// An fd on the APK at the (stored) asset's offset; -1 for a compressed one.
int BIONIC_SYSV AssetOpenFileDescriptor(NdkAsset* a, int64_t* start, int64_t* length) {
  off64_t s = 0, l = 0;
  int fd = a->asset->openFileDescriptor(&s, &l);
  *start = s;
  *length = l;
  return fd;
}
int BIONIC_SYSV AssetIsAllocated(NdkAsset* a) { return a->asset->isAllocated() ? 1 : 0; }

// --- AConfiguration ------------------------------------------------------------------

ResTable_config* BIONIC_SYSV ConfigurationNew() {
  auto* c = new ResTable_config();
  std::memset(c, 0, sizeof(*c));
  return c;
}
void BIONIC_SYSV ConfigurationDelete(ResTable_config* c) { delete c; }
void BIONIC_SYSV ConfigurationFromAssetManager(ResTable_config* out, AAssetManager* am) {
  std::memset(out, 0, sizeof(*out));
  if (am == nullptr) return;
  ScopedLock<AssetManager2> locked(*GuardedOf(am));
  const auto& configs = locked->GetConfigurations();
  if (!configs.empty()) std::memcpy(out, &configs[0], sizeof(*out));
}
void BIONIC_SYSV ConfigurationCopy(ResTable_config* dst, const ResTable_config* src) { *dst = *src; }
int32_t BIONIC_SYSV ConfigurationDiff(const ResTable_config* a, const ResTable_config* b) {
  return static_cast<int32_t>(a->diff(*b));
}
int32_t BIONIC_SYSV ConfigurationMatch(const ResTable_config* base, const ResTable_config* req) {
  return base->match(*req) ? 1 : 0;
}
int32_t BIONIC_SYSV ConfigurationIsBetterThan(const ResTable_config* base,
                                              const ResTable_config* test,
                                              const ResTable_config* req) {
  return base->isBetterThan(*test, req) ? 1 : 0;
}

#define CONFIG_FIELD(Name, field)                                                          \
  int32_t BIONIC_SYSV ConfigGet##Name(const ResTable_config* c) { return c->field; }     \
  void BIONIC_SYSV ConfigSet##Name(ResTable_config* c, int32_t v) {                        \
    c->field = static_cast<decltype(c->field)>(v);                                         \
  }
CONFIG_FIELD(Mcc, mcc)
CONFIG_FIELD(Mnc, mnc)
CONFIG_FIELD(Orientation, orientation)
CONFIG_FIELD(Touchscreen, touchscreen)
CONFIG_FIELD(Density, density)
CONFIG_FIELD(Keyboard, keyboard)
CONFIG_FIELD(Navigation, navigation)
CONFIG_FIELD(SdkVersion, sdkVersion)
CONFIG_FIELD(ScreenWidthDp, screenWidthDp)
CONFIG_FIELD(ScreenHeightDp, screenHeightDp)
CONFIG_FIELD(SmallestScreenWidthDp, smallestScreenWidthDp)
#undef CONFIG_FIELD

// Fields packed with others: (mask, shift) within a byte.
#define CONFIG_BITS(Name, field, mask, shift)                                                \
  int32_t BIONIC_SYSV ConfigGet##Name(const ResTable_config* c) {                            \
    return (c->field & (mask)) >> (shift);                                                   \
  }                                                                                          \
  void BIONIC_SYSV ConfigSet##Name(ResTable_config* c, int32_t v) {                          \
    c->field = static_cast<decltype(c->field)>((c->field & ~(mask)) | ((v << (shift)) & (mask))); \
  }
CONFIG_BITS(KeysHidden, inputFlags, ResTable_config::MASK_KEYSHIDDEN, 0)
CONFIG_BITS(NavHidden, inputFlags, ResTable_config::MASK_NAVHIDDEN, ResTable_config::SHIFT_NAVHIDDEN)
CONFIG_BITS(ScreenSize, screenLayout, ResTable_config::MASK_SCREENSIZE, 0)
CONFIG_BITS(ScreenLong, screenLayout, ResTable_config::MASK_SCREENLONG, ResTable_config::SHIFT_SCREENLONG)
CONFIG_BITS(LayoutDirection, screenLayout, ResTable_config::MASK_LAYOUTDIR, ResTable_config::SHIFT_LAYOUTDIR)
CONFIG_BITS(ScreenRound, screenLayout2, ResTable_config::MASK_SCREENROUND, 0)
CONFIG_BITS(UiModeType, uiMode, ResTable_config::MASK_UI_MODE_TYPE, 0)
CONFIG_BITS(UiModeNight, uiMode, ResTable_config::MASK_UI_MODE_NIGHT, ResTable_config::SHIFT_UI_MODE_NIGHT)
#undef CONFIG_BITS

void BIONIC_SYSV ConfigGetLanguage(const ResTable_config* c, char* out) {
  out[0] = c->language[0];
  out[1] = c->language[1];
}
void BIONIC_SYSV ConfigSetLanguage(ResTable_config* c, const char* in) {
  c->language[0] = in != nullptr ? in[0] : 0;
  c->language[1] = in != nullptr ? in[1] : 0;
}
void BIONIC_SYSV ConfigGetCountry(const ResTable_config* c, char* out) {
  out[0] = c->country[0];
  out[1] = c->country[1];
}
void BIONIC_SYSV ConfigSetCountry(ResTable_config* c, const char* in) {
  c->country[0] = in != nullptr ? in[0] : 0;
  c->country[1] = in != nullptr ? in[1] : 0;
}

// --- ALooper ----------------------------------------------------------------------------

using SysvLooperFunc = int(BIONIC_SYSV*)(int fd, int events, void* data);

class SysvLooperCallback : public android::LooperCallback {
 public:
  explicit SysvLooperCallback(SysvLooperFunc fn) : fn_(fn) {}
  int handleEvent(int fd, int events, void* data) override {
    gesher::elf::SetupThreadTls();  // the callback is loaded code
    return fn_(fd, events, data);
  }

 private:
  SysvLooperFunc fn_;
};

Looper* L(void* looper) { return static_cast<Looper*>(looper); }

void* BIONIC_SYSV LooperForThread() { return Looper::getForThread().get(); }
void* BIONIC_SYSV LooperPrepare(int opts) { return Looper::prepare(opts).get(); }
void BIONIC_SYSV LooperAcquire(void* looper) {
  L(looper)->incStrong(reinterpret_cast<void*>(&LooperAcquire));
}
void BIONIC_SYSV LooperRelease(void* looper) {
  L(looper)->decStrong(reinterpret_cast<void*>(&LooperAcquire));
}
int BIONIC_SYSV LooperPollOnce(int timeout, int* fd, int* events, void** data) {
  android::sp<Looper> looper = Looper::getForThread();
  if (looper == nullptr) return -4;  // ALOOPER_POLL_ERROR
  DeliverPendingSignals();
  return looper->pollOnce(timeout, fd, events, data);
}
int BIONIC_SYSV LooperPollAll(int timeout, int* fd, int* events, void** data) {
  android::sp<Looper> looper = Looper::getForThread();
  if (looper == nullptr) return -4;
  DeliverPendingSignals();
  return looper->pollAll(timeout, fd, events, data);
}
void BIONIC_SYSV LooperWake(void* looper) { L(looper)->wake(); }
int BIONIC_SYSV LooperAddFd(void* looper, int fd, int ident, int events, SysvLooperFunc callback,
                            void* data) {
  android::sp<android::LooperCallback> cb;
  if (callback != nullptr) cb = android::sp<SysvLooperCallback>::make(callback);
  return L(looper)->addFd(fd, ident, events, cb, data);
}
int BIONIC_SYSV LooperRemoveFd(void* looper, int fd) { return L(looper)->removeFd(fd); }

// --- ASensorManager: no sensors ---------------------------------------------------------------

struct SensorManager {
  int unused;
};
SensorManager g_sensor_manager;
struct SensorQueue {
  int unused;
};
const void* const g_empty_sensor_list[1] = {nullptr};
constexpr int kEinval = -22;

SensorManager* BIONIC_SYSV SensorManagerGetInstance() { return &g_sensor_manager; }
SensorManager* BIONIC_SYSV SensorManagerGetInstanceForPackage(const char*) {
  return &g_sensor_manager;
}
int BIONIC_SYSV SensorManagerGetSensorList(SensorManager*, const void* const** list) {
  if (list != nullptr) *list = g_empty_sensor_list;
  return 0;
}
int64_t BIONIC_SYSV SensorManagerGetDynamicSensorList(SensorManager*, const void* const** list) {
  if (list != nullptr) *list = g_empty_sensor_list;
  return 0;
}
const void* BIONIC_SYSV SensorManagerGetDefaultSensor(SensorManager*, int) { return nullptr; }
const void* BIONIC_SYSV SensorManagerGetDefaultSensorEx(SensorManager*, int, bool) {
  return nullptr;
}
SensorQueue* BIONIC_SYSV SensorManagerCreateEventQueue(SensorManager*, void*, int, void*, void*) {
  return new SensorQueue();
}
int BIONIC_SYSV SensorManagerDestroyEventQueue(SensorManager*, SensorQueue* q) {
  delete q;
  return 0;
}
int BIONIC_SYSV SensorManagerCreateDirectChannel(SensorManager*, int, size_t) { return kEinval; }
void BIONIC_SYSV SensorManagerDestroyDirectChannel(SensorManager*, int) {}
int BIONIC_SYSV SensorManagerConfigureDirectReport(SensorManager*, const void*, int, int) {
  return kEinval;
}
int BIONIC_SYSV SensorQueueRegisterSensor(SensorQueue*, const void*, int32_t, int64_t) {
  return kEinval;
}
int BIONIC_SYSV SensorQueueEnableSensor(SensorQueue*, const void*) { return kEinval; }
int BIONIC_SYSV SensorQueueDisableSensor(SensorQueue*, const void*) { return kEinval; }
int BIONIC_SYSV SensorQueueSetEventRate(SensorQueue*, const void*, int32_t) { return kEinval; }
int BIONIC_SYSV SensorQueueHasEvents(SensorQueue*) { return 0; }
int64_t BIONIC_SYSV SensorQueueGetEvents(SensorQueue*, void*, size_t) { return 0; }
int BIONIC_SYSV SensorQueueRequestAdditionalInfo(SensorQueue*, bool) { return kEinval; }
// ASensor_* would only see sensors the list never contains.
const char* BIONIC_SYSV SensorGetString(const void*) { return ""; }
int BIONIC_SYSV SensorGetInt(const void*) { return 0; }
float BIONIC_SYSV SensorGetFloat(const void*) { return 0; }
bool BIONIC_SYSV SensorGetBool(const void*) { return false; }
bool BIONIC_SYSV SensorChannelSupported(const void*, int) { return false; }

// --- ATrace: tracing is off ------------------------------------------------------------------------

bool BIONIC_SYSV TraceIsEnabled() { return false; }
void BIONIC_SYSV TraceBeginSection(const char*) {}
void BIONIC_SYSV TraceEndSection() {}
void BIONIC_SYSV TraceAsyncSection(const char*, int32_t) {}
void BIONIC_SYSV TraceSetCounter(const char*, int64_t) {}

// --- AStorageManager, AObbScanner ---------------------------------------------------------------

struct StorageManager {
  int unused;
};
using SysvObbCallback = void(BIONIC_SYSV*)(const char* filename, int32_t state, void* data);
constexpr int32_t kObbErrorInternal = 20;    // AOBB_STATE_ERROR_INTERNAL
constexpr int32_t kObbErrorNotMounted = 23;  // AOBB_STATE_ERROR_NOT_MOUNTED
std::atomic<bool> g_said_obb{false};

// The platform reports from a binder thread; so does this, from its own.
void ReportObb(const char* filename, int32_t state, SysvObbCallback cb, void* data) {
  if (cb == nullptr) return;
  std::string name = filename != nullptr ? filename : "";
  std::thread([name, state, cb, data] {
    gesher::elf::SetupThreadTls();
    cb(name.c_str(), state, data);
  }).detach();
}

StorageManager* BIONIC_SYSV StorageManagerNew() { return new StorageManager(); }
void BIONIC_SYSV StorageManagerDelete(StorageManager* m) { delete m; }
void BIONIC_SYSV StorageManagerMountObb(StorageManager*, const char* filename, const char*,
                                        SysvObbCallback cb, void* data) {
  LogOnce(&g_said_obb, "AStorageManager_mountObb: OBB mounting is not supported");
  ReportObb(filename, kObbErrorInternal, cb, data);
}
void BIONIC_SYSV StorageManagerUnmountObb(StorageManager*, const char* filename, int,
                                          SysvObbCallback cb, void* data) {
  ReportObb(filename, kObbErrorNotMounted, cb, data);
}
int BIONIC_SYSV StorageManagerIsObbMounted(StorageManager*, const char*) { return 0; }
const char* BIONIC_SYSV StorageManagerGetMountedObbPath(StorageManager*, const char*) {
  return nullptr;
}

struct ObbInfo : public android::ObbFile {};

ObbInfo* BIONIC_SYSV ObbScannerGetObbInfo(const char* filename) {
  auto* info = new ObbInfo();
  if (!info->readFrom(filename)) {
    delete info;
    return nullptr;
  }
  info->incStrong(reinterpret_cast<void*>(&ObbScannerGetObbInfo));
  return info;
}
void BIONIC_SYSV ObbInfoDelete(ObbInfo* info) {
  if (info != nullptr) info->decStrong(reinterpret_cast<void*>(&ObbScannerGetObbInfo));
}
const char* BIONIC_SYSV ObbInfoGetPackageName(ObbInfo* info) {
  return info->getPackageName().c_str();
}
int32_t BIONIC_SYSV ObbInfoGetVersion(ObbInfo* info) { return info->getVersion(); }
int32_t BIONIC_SYSV ObbInfoGetFlags(ObbInfo* info) { return info->getFlags(); }

// --- ASharedMemory ---------------------------------------------------------------------------------

int BIONIC_SYSV SharedMemoryCreate(const char* name, size_t size) {
  if (size == 0) return errno = kEINVAL, -1;
  int fd = memfd_create(name != nullptr ? name : "ashmem", 0);
  if (fd < 0) return -1;
  if (_chsize_s(fd, static_cast<long long>(size)) != 0) {
    close(fd);
    return errno = kENOMEM, -1;
  }
  return fd;
}
size_t BIONIC_SYSV SharedMemoryGetSize(int fd) {
  struct stat st {};
  if (fstat(fd, &st) != 0) return 0;
  return static_cast<size_t>(st.st_size);
}
int BIONIC_SYSV SharedMemorySetProt(int, int) { return 0; }

// --- ANativeWindow, ANativeActivity: the graphics track's ---------------------------------------
// No Surface exists yet: ANativeWindow_fromSurface gives nullptr and the
// window functions fail with -EINVAL; ANativeActivity's requests are dropped.

std::atomic<bool> g_said_window{false};
std::atomic<bool> g_said_activity{false};
void SayWindow() {
  LogOnce(&g_said_window, "ANativeWindow: no native windows yet (the graphics track's)");
}
void SayActivity() {
  LogOnce(&g_said_activity, "ANativeActivity: requests are not handled yet (the graphics track's)");
}

void* BIONIC_SYSV WindowFromSurface(JNIEnv*, jobject) {
  SayWindow();
  return nullptr;
}
jobject BIONIC_SYSV WindowToSurface(JNIEnv*, void*) {
  SayWindow();
  return nullptr;
}
void BIONIC_SYSV WindowAcquireRelease(void*) {}
int32_t BIONIC_SYSV WindowQuery(void*) {
  SayWindow();
  return kEinval;
}
int32_t BIONIC_SYSV WindowSetBuffersGeometry(void*, int32_t, int32_t, int32_t) {
  SayWindow();
  return kEinval;
}
int32_t BIONIC_SYSV WindowLock(void*, void*, void*) {
  SayWindow();
  return kEinval;
}
int32_t BIONIC_SYSV WindowSetInt(void*, int32_t) {
  SayWindow();
  return kEinval;
}
void BIONIC_SYSV ActivityRequest(void*) { SayActivity(); }
void BIONIC_SYSV ActivityRequestFlags(void*, uint32_t, uint32_t) { SayActivity(); }
void BIONIC_SYSV ActivityRequestOne(void*, uint32_t) { SayActivity(); }

// --- the table ---------------------------------------------------------------------------------------

void Register() {
  struct Entry {
    const char* name;
    void* fn;
  };
#define E(name, fn) {name, reinterpret_cast<void*>(&fn)}
#define CFG(Name) E("AConfiguration_get" #Name, ConfigGet##Name), E("AConfiguration_set" #Name, ConfigSet##Name)
  static const Entry kEntries[] = {
      // Assets.
      E("AAssetManager_fromJava", AssetManagerFromJava),
      E("AAssetManager_open", AssetManagerOpen),
      E("AAssetManager_openDir", AssetManagerOpenDir),
      E("AAssetDir_getNextFileName", AssetDirGetNextFileName),
      E("AAssetDir_rewind", AssetDirRewind),
      E("AAssetDir_close", AssetDirClose),
      E("AAsset_read", AssetRead),
      E("AAsset_seek", AssetSeek),
      E("AAsset_seek64", AssetSeek),
      E("AAsset_close", AssetClose),
      E("AAsset_getBuffer", AssetGetBuffer),
      E("AAsset_getLength", AssetGetLength),
      E("AAsset_getLength64", AssetGetLength),
      E("AAsset_getRemainingLength", AssetGetRemainingLength),
      E("AAsset_getRemainingLength64", AssetGetRemainingLength),
      E("AAsset_openFileDescriptor", AssetOpenFileDescriptor),
      E("AAsset_openFileDescriptor64", AssetOpenFileDescriptor),
      E("AAsset_isAllocated", AssetIsAllocated),
      // Configuration.
      E("AConfiguration_new", ConfigurationNew),
      E("AConfiguration_delete", ConfigurationDelete),
      E("AConfiguration_fromAssetManager", ConfigurationFromAssetManager),
      E("AConfiguration_copy", ConfigurationCopy),
      E("AConfiguration_diff", ConfigurationDiff),
      E("AConfiguration_match", ConfigurationMatch),
      E("AConfiguration_isBetterThan", ConfigurationIsBetterThan),
      CFG(Mcc), CFG(Mnc), CFG(Orientation), CFG(Touchscreen), CFG(Density), CFG(Keyboard),
      CFG(Navigation), CFG(SdkVersion), CFG(ScreenWidthDp), CFG(ScreenHeightDp),
      CFG(SmallestScreenWidthDp), CFG(KeysHidden), CFG(NavHidden), CFG(ScreenSize),
      CFG(ScreenLong), CFG(LayoutDirection), CFG(ScreenRound), CFG(UiModeType),
      CFG(UiModeNight), CFG(Language), CFG(Country),
      // Looper.
      E("ALooper_forThread", LooperForThread),
      E("ALooper_prepare", LooperPrepare),
      E("ALooper_acquire", LooperAcquire),
      E("ALooper_release", LooperRelease),
      E("ALooper_pollOnce", LooperPollOnce),
      E("ALooper_pollAll", LooperPollAll),
      E("ALooper_wake", LooperWake),
      E("ALooper_addFd", LooperAddFd),
      E("ALooper_removeFd", LooperRemoveFd),
      // Sensors.
      E("ASensorManager_getInstance", SensorManagerGetInstance),
      E("ASensorManager_getInstanceForPackage", SensorManagerGetInstanceForPackage),
      E("ASensorManager_getSensorList", SensorManagerGetSensorList),
      E("ASensorManager_getDynamicSensorList", SensorManagerGetDynamicSensorList),
      E("ASensorManager_getDefaultSensor", SensorManagerGetDefaultSensor),
      E("ASensorManager_getDefaultSensorEx", SensorManagerGetDefaultSensorEx),
      E("ASensorManager_createEventQueue", SensorManagerCreateEventQueue),
      E("ASensorManager_destroyEventQueue", SensorManagerDestroyEventQueue),
      E("ASensorManager_createSharedMemoryDirectChannel", SensorManagerCreateDirectChannel),
      E("ASensorManager_createHardwareBufferDirectChannel", SensorManagerCreateDirectChannel),
      E("ASensorManager_destroyDirectChannel", SensorManagerDestroyDirectChannel),
      E("ASensorManager_configureDirectReport", SensorManagerConfigureDirectReport),
      E("ASensorEventQueue_registerSensor", SensorQueueRegisterSensor),
      E("ASensorEventQueue_enableSensor", SensorQueueEnableSensor),
      E("ASensorEventQueue_disableSensor", SensorQueueDisableSensor),
      E("ASensorEventQueue_setEventRate", SensorQueueSetEventRate),
      E("ASensorEventQueue_hasEvents", SensorQueueHasEvents),
      E("ASensorEventQueue_getEvents", SensorQueueGetEvents),
      E("ASensorEventQueue_requestAdditionalInfoEvents", SensorQueueRequestAdditionalInfo),
      E("ASensor_getName", SensorGetString),
      E("ASensor_getVendor", SensorGetString),
      E("ASensor_getStringType", SensorGetString),
      E("ASensor_getType", SensorGetInt),
      E("ASensor_getMinDelay", SensorGetInt),
      E("ASensor_getFifoMaxEventCount", SensorGetInt),
      E("ASensor_getFifoReservedEventCount", SensorGetInt),
      E("ASensor_getReportingMode", SensorGetInt),
      E("ASensor_getHighestDirectReportRateLevel", SensorGetInt),
      E("ASensor_getHandle", SensorGetInt),
      E("ASensor_getResolution", SensorGetFloat),
      E("ASensor_isWakeUpSensor", SensorGetBool),
      E("ASensor_isDirectChannelTypeSupported", SensorChannelSupported),
      // Tracing.
      E("ATrace_isEnabled", TraceIsEnabled),
      E("ATrace_beginSection", TraceBeginSection),
      E("ATrace_endSection", TraceEndSection),
      E("ATrace_beginAsyncSection", TraceAsyncSection),
      E("ATrace_endAsyncSection", TraceAsyncSection),
      E("ATrace_setCounter", TraceSetCounter),
      // Storage and OBB files.
      E("AStorageManager_new", StorageManagerNew),
      E("AStorageManager_delete", StorageManagerDelete),
      E("AStorageManager_mountObb", StorageManagerMountObb),
      E("AStorageManager_unmountObb", StorageManagerUnmountObb),
      E("AStorageManager_isObbMounted", StorageManagerIsObbMounted),
      E("AStorageManager_getMountedObbPath", StorageManagerGetMountedObbPath),
      E("AObbScanner_getObbInfo", ObbScannerGetObbInfo),
      E("AObbInfo_delete", ObbInfoDelete),
      E("AObbInfo_getPackageName", ObbInfoGetPackageName),
      E("AObbInfo_getVersion", ObbInfoGetVersion),
      E("AObbInfo_getFlags", ObbInfoGetFlags),
      // Shared memory.
      E("ASharedMemory_create", SharedMemoryCreate),
      E("ASharedMemory_getSize", SharedMemoryGetSize),
      E("ASharedMemory_setProt", SharedMemorySetProt),
      // Graphics-track stand-ins.
      E("ANativeWindow_fromSurface", WindowFromSurface),
      E("ANativeWindow_toSurface", WindowToSurface),
      E("ANativeWindow_acquire", WindowAcquireRelease),
      E("ANativeWindow_release", WindowAcquireRelease),
      E("ANativeWindow_getWidth", WindowQuery),
      E("ANativeWindow_getHeight", WindowQuery),
      E("ANativeWindow_getFormat", WindowQuery),
      E("ANativeWindow_setBuffersGeometry", WindowSetBuffersGeometry),
      E("ANativeWindow_lock", WindowLock),
      E("ANativeWindow_unlockAndPost", WindowQuery),
      E("ANativeWindow_setBuffersTransform", WindowSetInt),
      E("ANativeWindow_setBuffersDataSpace", WindowSetInt),
      E("ANativeWindow_getBuffersDataSpace", WindowQuery),
      E("ANativeActivity_finish", ActivityRequest),
      E("ANativeActivity_setWindowFormat", ActivityRequestOne),
      E("ANativeActivity_setWindowFlags", ActivityRequestFlags),
      E("ANativeActivity_showSoftInput", ActivityRequestOne),
      E("ANativeActivity_hideSoftInput", ActivityRequestOne),
  };
#undef CFG
#undef E
  for (const Entry& e : kEntries) AddBionicSymbol(e.name, e.fn);
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace

}  // namespace gesher::bionic
