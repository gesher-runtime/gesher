// Gesher (devices track, docs/bluetooth.md): com.gesher.server.WindowsBluetooth's
// natives (gesher-app.jar), over bluetooth.h's backend. Registered by
// register_gesher_devices (devices_jni.cc).
//
// Scans, GATT connections, sockets and listeners are handles: ids into
// registries of shared pointers, so a call racing with a close keeps its object
// alive until it returns, and a stale id is just "not found".

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>
#include <nativehelper/scoped_utf_chars.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bluetooth.h"
#include "core_jni_helpers.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

namespace bt = gesher::devices::bluetooth;

std::atomic<jlong> gNextHandle{1};

template <typename T>
class Registry {
 public:
  jlong Add(std::shared_ptr<T> object) {
    jlong id = gNextHandle.fetch_add(1);
    std::lock_guard<std::mutex> lock(mu_);
    objects_[id] = std::move(object);
    return id;
  }
  std::shared_ptr<T> Get(jlong id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : it->second;
  }
  std::shared_ptr<T> Remove(jlong id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = objects_.find(id);
    if (it == objects_.end()) return nullptr;
    std::shared_ptr<T> object = std::move(it->second);
    objects_.erase(it);
    return object;
  }

 private:
  std::mutex mu_;
  std::map<jlong, std::shared_ptr<T>> objects_;
};

Registry<bt::Scan>& Scans() {
  static auto* r = new Registry<bt::Scan>();
  return *r;
}
Registry<bt::Gatt>& Gatts() {
  static auto* r = new Registry<bt::Gatt>();
  return *r;
}
struct SocketHandle {
  std::shared_ptr<bt::Socket> socket;
  std::atomic<uint64_t> bytesRead{0};
  std::atomic<uint64_t> bytesWritten{0};

  explicit SocketHandle(std::shared_ptr<bt::Socket> s) : socket(std::move(s)) {}
};

Registry<SocketHandle>& Sockets() {
  static auto* r = new Registry<SocketHandle>();
  return *r;
}
Registry<bt::Server>& Servers() {
  static auto* r = new Registry<bt::Server>();
  return *r;
}

std::atomic<bool> gInquiryCancel{false};

void SetInt(JNIEnv* env, jintArray array, int index, jint value) {
  if (array != nullptr && env->GetArrayLength(array) > index) {
    env->SetIntArrayRegion(array, index, 1, &value);
  }
}

void SetLong(JNIEnv* env, jlongArray array, int index, jlong value) {
  if (array != nullptr && env->GetArrayLength(array) > index) {
    env->SetLongArrayRegion(array, index, 1, &value);
  }
}

// Text fields are tab-separated: tabs and line breaks in names become spaces.
std::string Field(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return out;
}

jstring NewString(JNIEnv* env, const std::string& utf8) {
  // Modified UTF-8 differs from UTF-8 only for NUL and supplementary characters;
  // go through UTF-16 to be exact.
  std::wstring wide = bt::Wide(utf8);
  return env->NewString(reinterpret_cast<const jchar*>(wide.data()),
                        static_cast<jsize>(wide.size()));
}

jobjectArray Strings(JNIEnv* env, const std::vector<std::string>& lines) {
  jclass string = env->FindClass("java/lang/String");
  jobjectArray result = env->NewObjectArray(static_cast<jsize>(lines.size()), string, nullptr);
  for (size_t i = 0; i < lines.size(); ++i) {
    ScopedLocalRef<jstring> s(env, NewString(env, lines[i]));
    env->SetObjectArrayElement(result, static_cast<jsize>(i), s.get());
  }
  return result;
}

jbyteArray Bytes(JNIEnv* env, const std::vector<uint8_t>& bytes) {
  jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
  if (array != nullptr && !bytes.empty()) {
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte*>(bytes.data()));
  }
  return array;
}

std::string Uuids(const std::vector<GUID>& uuids) {
  std::string out;
  for (const GUID& g : uuids) {
    if (!out.empty()) out += ",";
    out += bt::GuidText(g);
  }
  return out;
}

// "address\taddressType\tkind\tbonded\tconnected\tclass\tname\tuuid,uuid..."
std::string DeviceLine(const bt::Device& d) {
  return std::to_string(static_cast<long long>(d.address)) + "\t" +
         std::to_string(d.addressType) + "\t" + std::to_string(d.kind) + "\t" +
         (d.bonded ? "1" : "0") + "\t" + (d.connected ? "1" : "0") + "\t" +
         std::to_string(d.classOfDevice) + "\t" + Field(d.name) + "\t" + Uuids(d.uuids);
}

// --- adapter and devices ----------------------------------------------------------------------

// "present\ton\taddress\tname\tflags\tmaxAdvertisingLength\tvia\thresult"; flags: 1
// classic, 2 LE, 4 central, 8 peripheral, 16 extended advertising, 32 LE 2M, 64 LE coded.
jstring WindowsBluetooth_adapter(JNIEnv* env, jclass) {
  bt::Adapter a = bt::Get().GetAdapter();
  int flags = (a.classic ? 1 : 0) | (a.le ? 2 : 0) | (a.central ? 4 : 0) |
              (a.peripheral ? 8 : 0) | (a.extendedAdvertising ? 16 : 0) | (a.le2mPhy ? 32 : 0) |
              (a.leCodedPhy ? 64 : 0);
  std::string line = std::string(a.present ? "1" : "0") + "\t" + (a.on ? "1" : "0") + "\t" +
                     std::to_string(static_cast<long long>(a.address)) + "\t" + Field(a.name) +
                     "\t" + std::to_string(flags) + "\t" +
                     std::to_string(a.maxAdvertisementDataLength) + "\t" + Field(a.via) + "\t" +
                     std::to_string(static_cast<long long>(static_cast<uint32_t>(a.hr)));
  return NewString(env, line);
}

jint WindowsBluetooth_setEnabled(JNIEnv*, jclass, jboolean on) {
  HRESULT hr = bt::Get().SetEnabled(on == JNI_TRUE);
  ALOGI("bluetooth: turning the radio %s: HRESULT 0x%08lX", on ? "on" : "off",
        static_cast<unsigned long>(hr));
  return static_cast<jint>(hr);
}

jobjectArray WindowsBluetooth_bonded(JNIEnv* env, jclass, jintArray hrOut) {
  std::vector<bt::Device> devices;
  HRESULT hr = bt::Get().Bonded(&devices);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  std::vector<std::string> lines;
  for (const bt::Device& d : devices) lines.push_back(DeviceLine(d));
  return Strings(env, lines);
}

jobjectArray WindowsBluetooth_inquiry(JNIEnv* env, jclass, jint seconds, jintArray hrOut) {
  gInquiryCancel.store(false);
  std::vector<std::string> lines;
  HRESULT hr = bt::Get().Inquiry(seconds, &gInquiryCancel, [&lines](const bt::Device& d) {
    lines.push_back(DeviceLine(d));
  });
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  return Strings(env, lines);
}

void WindowsBluetooth_inquiryCancel(JNIEnv*, jclass) { gInquiryCancel.store(true); }

jobjectArray WindowsBluetooth_remoteUuids(JNIEnv* env, jclass, jlong address, jboolean refresh,
                                          jintArray hrOut) {
  std::vector<GUID> uuids;
  HRESULT hr = bt::Get().RemoteUuids(static_cast<uint64_t>(address), refresh == JNI_TRUE, &uuids);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  std::vector<std::string> lines;
  for (const GUID& g : uuids) lines.push_back(bt::GuidText(g));
  return Strings(env, lines);
}

// Pairing through Windows' UI (blocks); result: 0 paired, 1 already, 2 not paired.
jint WindowsBluetooth_pair(JNIEnv* env, jclass, jlong address, jboolean le, jintArray resultOut) {
  int result = 2;
  HRESULT hr = bt::Get().Pair(static_cast<uint64_t>(address), le == JNI_TRUE, &result);
  ALOGI("bluetooth: pairing %012llX: HRESULT 0x%08lX, result %d",
        static_cast<unsigned long long>(address), static_cast<unsigned long>(hr), result);
  SetInt(env, resultOut, 0, result);
  return static_cast<jint>(hr);
}

// result: 0 unpaired, 1 was not paired, 2 failed.
jint WindowsBluetooth_unpair(JNIEnv* env, jclass, jlong address, jintArray resultOut) {
  int result = 2;
  HRESULT hr = bt::Get().Unpair(static_cast<uint64_t>(address), &result);
  SetInt(env, resultOut, 0, result);
  return static_cast<jint>(hr);
}

jboolean WindowsBluetooth_waitForChange(JNIEnv*, jclass, jint timeoutMs) {
  return bt::Get().WaitForChange(timeoutMs) ? JNI_TRUE : JNI_FALSE;
}

// --- LE scan ---------------------------------------------------------------------------------

jlong WindowsBluetooth_scanStart(JNIEnv* env, jclass, jboolean active, jintArray hrOut) {
  HRESULT hr = S_OK;
  std::shared_ptr<bt::Scan> scan = bt::Get().StartScan(active == JNI_TRUE, &hr);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  ALOGI("bluetooth: LE scan (%s) started: HRESULT 0x%08lX", active ? "active" : "passive",
        static_cast<unsigned long>(hr));
  return scan ? Scans().Add(std::move(scan)) : 0;
}

// The next report's AD structures, or null on time-out / end. meta: address,
// address type, RSSI, TX power, flags (1 connectable, 2 scan response, 4 legacy),
// and the scan's error (non-zero once Windows ended it).
jbyteArray WindowsBluetooth_scanNext(JNIEnv* env, jclass, jlong handle, jint timeoutMs,
                                     jlongArray meta) {
  std::shared_ptr<bt::Scan> scan = Scans().Get(handle);
  if (!scan) {
    SetLong(env, meta, 5, E_HANDLE);
    return nullptr;
  }
  bt::Advertisement a;
  if (!scan->Next(&a, timeoutMs)) {
    SetLong(env, meta, 5, static_cast<jlong>(scan->Error()));
    return nullptr;
  }
  jlong values[6] = {static_cast<jlong>(a.address), a.addressType, a.rssi, a.txPower,
                     (a.connectable ? 1 : 0) | (a.scanResponse ? 2 : 0) | (a.legacy ? 4 : 0), 0};
  if (meta != nullptr && env->GetArrayLength(meta) >= 6) {
    env->SetLongArrayRegion(meta, 0, 6, values);
  }
  return Bytes(env, a.data);
}

void WindowsBluetooth_scanStop(JNIEnv*, jclass, jlong handle) {
  std::shared_ptr<bt::Scan> scan = Scans().Remove(handle);
  if (scan) scan->Stop();
}

// --- GATT ----------------------------------------------------------------------------------

jlong WindowsBluetooth_gattOpen(JNIEnv* env, jclass, jlong address, jint addressType,
                                jintArray hrOut) {
  HRESULT hr = S_OK;
  std::shared_ptr<bt::Gatt> gatt = bt::Get().OpenGatt(static_cast<uint64_t>(address),
                                                      addressType, &hr);
  SetInt(env, hrOut, 0, static_cast<jint>(hr));
  return gatt ? Gatts().Add(std::move(gatt)) : 0;
}

jint WindowsBluetooth_gattConnect(JNIEnv* env, jclass, jlong handle, jint timeoutMs,
                                  jintArray mtuOut) {
  std::shared_ptr<bt::Gatt> gatt = Gatts().Get(handle);
  if (!gatt) return E_HANDLE;
  int mtu = 23;
  HRESULT hr = gatt->Connect(timeoutMs, &mtu);
  SetInt(env, mtuOut, 0, mtu);
  return static_cast<jint>(hr);
}

// "kind\tuuid\thandle\tproperties" per attribute, in discovery order.
jobjectArray WindowsBluetooth_gattDiscover(JNIEnv* env, jclass, jlong handle,
                                           jintArray statusOut) {
  std::shared_ptr<bt::Gatt> gatt = Gatts().Get(handle);
  std::vector<std::string> lines;
  int status = bt::kGattFailure;
  if (gatt) {
    std::vector<bt::Attribute> attributes;
    status = gatt->Discover(&attributes);
    for (const bt::Attribute& a : attributes) {
      lines.push_back(std::to_string(a.kind) + "\t" + bt::GuidText(a.uuid) + "\t" +
                      std::to_string(a.handle) + "\t" + std::to_string(a.properties));
    }
  }
  SetInt(env, statusOut, 0, status);
  return Strings(env, lines);
}

jbyteArray WindowsBluetooth_gattRead(JNIEnv* env, jclass, jlong handle, jint kind,
                                     jint attribute, jintArray statusOut) {
  std::shared_ptr<bt::Gatt> gatt = Gatts().Get(handle);
  std::vector<uint8_t> value;
  int status = gatt ? gatt->Read(kind, static_cast<uint16_t>(attribute), &value)
                    : bt::kGattFailure;
  SetInt(env, statusOut, 0, status);
  return status == bt::kGattSuccess ? Bytes(env, value) : nullptr;
}

jint WindowsBluetooth_gattWrite(JNIEnv* env, jclass, jlong handle, jint kind, jint attribute,
                                jbyteArray value, jboolean withResponse) {
  std::shared_ptr<bt::Gatt> gatt = Gatts().Get(handle);
  if (!gatt) return bt::kGattFailure;
  std::vector<uint8_t> bytes;
  if (value != nullptr) {
    bytes.resize(env->GetArrayLength(value));
    if (!bytes.empty()) {
      env->GetByteArrayRegion(value, 0, static_cast<jsize>(bytes.size()),
                              reinterpret_cast<jbyte*>(bytes.data()));
    }
  }
  return gatt->Write(kind, static_cast<uint16_t>(attribute), bytes, withResponse == JNI_TRUE);
}

jint WindowsBluetooth_gattNotify(JNIEnv*, jclass, jlong handle, jint attribute, jint mode) {
  std::shared_ptr<bt::Gatt> gatt = Gatts().Get(handle);
  return gatt ? gatt->SetNotify(static_cast<uint16_t>(attribute), mode) : bt::kGattFailure;
}

jint WindowsBluetooth_gattMtu(JNIEnv*, jclass, jlong handle) {
  std::shared_ptr<bt::Gatt> gatt = Gatts().Get(handle);
  return gatt ? gatt->Mtu() : 23;
}

// The next event: its data (possibly empty), or null on time-out. meta: type,
// handle, value. A closed (or unknown) connection answers type 5 (closed).
jbyteArray WindowsBluetooth_gattNext(JNIEnv* env, jclass, jlong handle, jint timeoutMs,
                                     jintArray meta) {
  std::shared_ptr<bt::Gatt> gatt = Gatts().Get(handle);
  bt::GattEvent e;
  if (!gatt) {
    e.type = bt::kClosed;
  } else if (!gatt->NextEvent(&e, timeoutMs)) {
    SetInt(env, meta, 0, 0);
    return nullptr;
  }
  SetInt(env, meta, 0, e.type);
  SetInt(env, meta, 1, e.handle);
  SetInt(env, meta, 2, e.value);
  return Bytes(env, e.data);
}

// kGattSuccess with out = {tx, rx} (1 1M, 2 2M, 3 coded), or why not.
jint WindowsBluetooth_gattPhy(JNIEnv* env, jclass, jlong handle, jintArray out) {
  std::shared_ptr<bt::Gatt> gatt = Gatts().Get(handle);
  if (!gatt) return bt::kGattFailure;
  int tx = 0, rx = 0;
  int status = gatt->Phy(&tx, &rx);
  SetInt(env, out, 0, tx);
  SetInt(env, out, 1, rx);
  return status;
}

void WindowsBluetooth_gattClose(JNIEnv*, jclass, jlong handle) {
  std::shared_ptr<bt::Gatt> gatt = Gatts().Remove(handle);
  if (gatt) gatt->Close();
}

// --- RFCOMM -------------------------------------------------------------------------------

// An RFCOMM socket, not connected yet (nativeRfcommConnect connects it); out: HRESULT.
jlong WindowsBluetooth_rfcommOpen(JNIEnv* env, jclass, jlong address, jstring uuid, jint channel,
                                  jboolean secure, jintArray out) {
  GUID g = {};
  std::string uuidText = "(none)";
  if (uuid != nullptr) {
    ScopedUtfChars chars(env, uuid);
    if (chars.c_str() == nullptr || !bt::ParseGuid(chars.c_str(), &g)) {
      SetInt(env, out, 0, E_INVALIDARG);
      return 0;
    }
    uuidText = chars.c_str();
  }
  HRESULT hr = S_OK;
  std::shared_ptr<bt::Socket> socket = bt::Get().OpenRfcomm(
      static_cast<uint64_t>(address), g, channel, secure == JNI_TRUE, &hr);
  SetInt(env, out, 0, static_cast<jint>(hr));
  jlong handle = socket ? Sockets().Add(std::make_shared<SocketHandle>(std::move(socket))) : 0;
  ALOGI("bluetooth: RFCOMM open #%lld to %012llX (uuid %s, ch %d, %s): HRESULT 0x%08lX",
        static_cast<long long>(handle), static_cast<unsigned long long>(address),
        uuidText.c_str(), static_cast<int>(channel),
        secure == JNI_TRUE ? "secure" : "insecure", static_cast<unsigned long>(hr));
  return handle;
}

// Connects (blocks; nativeRfcommClose from another thread ends it). out: channel.
jint WindowsBluetooth_rfcommConnect(JNIEnv* env, jclass, jlong handle, jintArray out) {
  std::shared_ptr<SocketHandle> entry = Sockets().Get(handle);
  if (!entry) return E_ABORT;
  HRESULT hr = entry->socket->Connect();
  ALOGI("bluetooth: RFCOMM connect #%lld to %012llX (ch %d): HRESULT 0x%08lX",
        static_cast<long long>(handle),
        static_cast<unsigned long long>(entry->socket->Remote()),
        entry->socket->Channel(), static_cast<unsigned long>(hr));
  if (SUCCEEDED(hr)) SetInt(env, out, 0, entry->socket->Channel());
  return static_cast<jint>(hr);
}

constexpr jint kChunk = 64 * 1024;

jint WindowsBluetooth_rfcommRead(JNIEnv* env, jclass, jlong handle, jbyteArray buffer,
                                 jint offset, jint length) {
  std::shared_ptr<SocketHandle> entry = Sockets().Get(handle);
  if (!entry) return 0;
  if (length > kChunk) length = kChunk;
  if (length <= 0) return 0;
  std::vector<uint8_t> chunk(length);
  int n = entry->socket->Read(chunk.data(), length);
  if (n > 0) {
    entry->bytesRead.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    env->SetByteArrayRegion(buffer, offset, n, reinterpret_cast<const jbyte*>(chunk.data()));
  } else {
    ALOGI("bluetooth: RFCOMM read #%lld ended: %d", static_cast<long long>(handle), n);
  }
  return n;
}

jint WindowsBluetooth_rfcommWrite(JNIEnv* env, jclass, jlong handle, jbyteArray buffer,
                                  jint offset, jint length) {
  std::shared_ptr<SocketHandle> entry = Sockets().Get(handle);
  if (!entry) return -10057;  // WSAENOTCONN
  jint done = 0;
  std::vector<uint8_t> chunk;
  while (done < length) {
    jint n = length - done > kChunk ? kChunk : length - done;
    chunk.resize(n);
    env->GetByteArrayRegion(buffer, offset + done, n, reinterpret_cast<jbyte*>(chunk.data()));
    int written = entry->socket->Write(chunk.data(), n);
    if (written < 0) {
      ALOGI("bluetooth: RFCOMM write #%lld failed: %d (after tx %llu B)",
            static_cast<long long>(handle), written,
            static_cast<unsigned long long>(entry->bytesWritten.load(std::memory_order_relaxed)));
      return written;
    }
    entry->bytesWritten.fetch_add(static_cast<uint64_t>(written), std::memory_order_relaxed);
    done += written;
  }
  return done;
}

jint WindowsBluetooth_rfcommAvailable(JNIEnv*, jclass, jlong handle) {
  std::shared_ptr<SocketHandle> entry = Sockets().Get(handle);
  return entry ? entry->socket->Available() : 0;
}

void WindowsBluetooth_rfcommClose(JNIEnv*, jclass, jlong handle) {
  std::shared_ptr<SocketHandle> entry = Sockets().Remove(handle);
  if (entry) {
    ALOGI("bluetooth: RFCOMM close #%lld to %012llX (ch %d, tx %llu B, rx %llu B)",
          static_cast<long long>(handle),
          static_cast<unsigned long long>(entry->socket->Remote()),
          entry->socket->Channel(),
          static_cast<unsigned long long>(entry->bytesWritten.load(std::memory_order_relaxed)),
          static_cast<unsigned long long>(entry->bytesRead.load(std::memory_order_relaxed)));
    entry->socket->Close();
  }
}

// out: HRESULT, channel.
jlong WindowsBluetooth_rfcommListen(JNIEnv* env, jclass, jstring name, jstring uuid,
                                    jboolean secure, jintArray out) {
  GUID g = {};
  std::wstring serviceName;
  {
    ScopedUtfChars chars(env, uuid);
    if (chars.c_str() == nullptr || !bt::ParseGuid(chars.c_str(), &g)) {
      SetInt(env, out, 0, E_INVALIDARG);
      return 0;
    }
  }
  if (name != nullptr) {
    ScopedUtfChars chars(env, name);
    if (chars.c_str() != nullptr) serviceName = bt::Wide(chars.c_str());
  }
  HRESULT hr = S_OK;
  std::shared_ptr<bt::Server> server =
      bt::Get().ListenRfcomm(serviceName, g, secure == JNI_TRUE, &hr);
  SetInt(env, out, 0, static_cast<jint>(hr));
  if (!server) return 0;
  SetInt(env, out, 1, server->Channel());
  return Servers().Add(std::move(server));
}

// A connection's handle, or 0; out: HRESULT, remote address, channel.
jlong WindowsBluetooth_rfcommAccept(JNIEnv* env, jclass, jlong handle, jint timeoutMs,
                                    jlongArray out) {
  std::shared_ptr<bt::Server> server = Servers().Get(handle);
  if (!server) {
    SetLong(env, out, 0, E_ABORT);
    return 0;
  }
  HRESULT hr = S_OK;
  std::shared_ptr<bt::Socket> socket = server->Accept(timeoutMs, &hr);
  SetLong(env, out, 0, hr);
  if (!socket) return 0;
  SetLong(env, out, 1, static_cast<jlong>(socket->Remote()));
  SetLong(env, out, 2, socket->Channel());
  return Sockets().Add(std::make_shared<SocketHandle>(std::move(socket)));
}

void WindowsBluetooth_rfcommServerClose(JNIEnv*, jclass, jlong handle) {
  std::shared_ptr<bt::Server> server = Servers().Remove(handle);
  if (server) server->Close();
}

const JNINativeMethod gMethods[] = {
    {"nativeAdapter", "()Ljava/lang/String;", GESHER_JNI(WindowsBluetooth_adapter)},
    {"nativeSetEnabled", "(Z)I", GESHER_JNI(WindowsBluetooth_setEnabled)},
    {"nativeBonded", "([I)[Ljava/lang/String;", GESHER_JNI(WindowsBluetooth_bonded)},
    {"nativeInquiry", "(I[I)[Ljava/lang/String;", GESHER_JNI(WindowsBluetooth_inquiry)},
    {"nativeInquiryCancel", "()V", GESHER_JNI(WindowsBluetooth_inquiryCancel)},
    {"nativeRemoteUuids", "(JZ[I)[Ljava/lang/String;", GESHER_JNI(WindowsBluetooth_remoteUuids)},
    {"nativePair", "(JZ[I)I", GESHER_JNI(WindowsBluetooth_pair)},
    {"nativeUnpair", "(J[I)I", GESHER_JNI(WindowsBluetooth_unpair)},
    {"nativeWaitForChange", "(I)Z", GESHER_JNI(WindowsBluetooth_waitForChange)},
    {"nativeScanStart", "(Z[I)J", GESHER_JNI(WindowsBluetooth_scanStart)},
    {"nativeScanNext", "(JI[J)[B", GESHER_JNI(WindowsBluetooth_scanNext)},
    {"nativeScanStop", "(J)V", GESHER_JNI(WindowsBluetooth_scanStop)},
    {"nativeGattOpen", "(JI[I)J", GESHER_JNI(WindowsBluetooth_gattOpen)},
    {"nativeGattConnect", "(JI[I)I", GESHER_JNI(WindowsBluetooth_gattConnect)},
    {"nativeGattDiscover", "(J[I)[Ljava/lang/String;", GESHER_JNI(WindowsBluetooth_gattDiscover)},
    {"nativeGattRead", "(JII[I)[B", GESHER_JNI(WindowsBluetooth_gattRead)},
    {"nativeGattWrite", "(JII[BZ)I", GESHER_JNI(WindowsBluetooth_gattWrite)},
    {"nativeGattNotify", "(JII)I", GESHER_JNI(WindowsBluetooth_gattNotify)},
    {"nativeGattMtu", "(J)I", GESHER_JNI(WindowsBluetooth_gattMtu)},
    {"nativeGattNext", "(JI[I)[B", GESHER_JNI(WindowsBluetooth_gattNext)},
    {"nativeGattPhy", "(J[I)I", GESHER_JNI(WindowsBluetooth_gattPhy)},
    {"nativeGattClose", "(J)V", GESHER_JNI(WindowsBluetooth_gattClose)},
    {"nativeRfcommOpen", "(JLjava/lang/String;IZ[I)J", GESHER_JNI(WindowsBluetooth_rfcommOpen)},
    {"nativeRfcommConnect", "(J[I)I", GESHER_JNI(WindowsBluetooth_rfcommConnect)},
    {"nativeRfcommRead", "(J[BII)I", GESHER_JNI(WindowsBluetooth_rfcommRead)},
    {"nativeRfcommWrite", "(J[BII)I", GESHER_JNI(WindowsBluetooth_rfcommWrite)},
    {"nativeRfcommAvailable", "(J)I", GESHER_JNI(WindowsBluetooth_rfcommAvailable)},
    {"nativeRfcommClose", "(J)V", GESHER_JNI(WindowsBluetooth_rfcommClose)},
    {"nativeRfcommListen", "(Ljava/lang/String;Ljava/lang/String;Z[I)J",
     GESHER_JNI(WindowsBluetooth_rfcommListen)},
    {"nativeRfcommAccept", "(JI[J)J", GESHER_JNI(WindowsBluetooth_rfcommAccept)},
    {"nativeRfcommServerClose", "(J)V", GESHER_JNI(WindowsBluetooth_rfcommServerClose)},
};

}  // namespace

int register_gesher_bluetooth(JNIEnv* env) {
  return RegisterGesherNatives(env, "com/gesher/server/WindowsBluetooth", gMethods,
                               NELEM(gMethods));
}

}  // namespace android
