// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <jni.h>

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

#include "core_jni_helpers.h"
#include "usb_storage_observation.h"
#include "usb_winusb.h"
#include "../usb_storage/protected_runtime_client.h"
#include "../usb_storage/protected_usb_observation.h"

namespace android {
int RegisterGesherNatives(JNIEnv*, const char*, const JNINativeMethod*, int);

namespace {
using gesher::usb::Connection;
using gesher::usb::OpenDevice;
using gesher::usb_storage::UsbObservationData;
using gesher::usb_storage::UsbObservationKind;
jfieldID context;
jfieldID request_context;

jstring String(JNIEnv* env, const std::wstring& value) {
  static_assert(sizeof(wchar_t) == sizeof(jchar));
  return env->NewString(reinterpret_cast<const jchar*>(value.data()), value.size());
}

std::wstring Wide(JNIEnv* env, jstring text) {
  if (!text) return {};
  std::wstring result(env->GetStringLength(text), L'\0');
  env->GetStringRegion(text, 0, result.size(), reinterpret_cast<jchar*>(result.data()));
  return result;
}

jbyteArray Bytes(JNIEnv* env, const std::vector<unsigned char>& bytes) {
  if (bytes.empty()) return nullptr;
  jbyteArray out = env->NewByteArray(bytes.size());
  if (out) {
    env->SetByteArrayRegion(out, 0, bytes.size(), reinterpret_cast<const jbyte*>(bytes.data()));
  }
  return out;
}

std::optional<UsbObservationData> ProtectedObservation(UsbObservationKind kind,
                                                     std::wstring_view path = {}) {
  if (!gesher::usb_storage::ProtectedRuntimeIsSystemServer()) return std::nullopt;
  std::vector<std::byte> bytes;
  UsbObservationData value;
  if (gesher::usb_storage::ProtectedUsbObservation(kind, path, &bytes) != ERROR_SUCCESS ||
      !gesher::usb_storage::DecodeUsbObservation(kind, bytes, &value)) return std::nullopt;
  return value;
}

jboolean Available(JNIEnv*, jclass) try {
  if (gesher::usb_storage::ProtectedRuntimeEnabled()) {
    auto value = ProtectedObservation(UsbObservationKind::Available);
    return value && value->available;
  }
  return gesher::usb::AttachmentMonitoringAvailable();
} catch (const std::bad_alloc&) {
  return JNI_FALSE;
}

jobjectArray PathArray(JNIEnv* env, const std::vector<std::wstring>& paths) {
  jclass str = env->FindClass("java/lang/String");
  if (!str) return nullptr;
  jobjectArray out = env->NewObjectArray(paths.size(), str, nullptr);
  env->DeleteLocalRef(str);
  if (!out) return nullptr;
  for (size_t i = 0; i < paths.size(); ++i) {
    jstring path = String(env, paths[i]);
    if (!path) return nullptr;
    env->SetObjectArrayElement(out, i, path);
    env->DeleteLocalRef(path);
  }
  return out;
}

jobjectArray Paths(JNIEnv* env, jclass) try {
  // The protected realm does not transport arbitrary WinUSB endpoint handles.
  // Its storage observations have a distinct Java backend with transfers
  // unavailable; do not advertise inaccessible WinUSB transfer capabilities.
  if (gesher::usb_storage::ProtectedRuntimeEnabled()) return PathArray(env, {});
  auto paths = gesher::usb::InterfacePaths();
  return paths ? PathArray(env, *paths) : nullptr;
} catch (const std::bad_alloc&) {
  return nullptr;
}

jobjectArray StoragePaths(JNIEnv* env, jclass) try {
  if (gesher::usb_storage::ProtectedRuntimeEnabled()) {
    auto value = ProtectedObservation(UsbObservationKind::Paths);
    return value ? PathArray(env, value->paths) : nullptr;
  }
  auto paths = gesher::usb::StorageInterfacePaths();
  return paths ? PathArray(env, *paths) : nullptr;
} catch (const std::bad_alloc&) {
  return nullptr;
}

jobject InspectStorage(JNIEnv* env, jclass, jstring name) try {
  std::optional<gesher::usb::StorageObservation> observation;
  if (gesher::usb_storage::ProtectedRuntimeEnabled()) {
    auto value = ProtectedObservation(UsbObservationKind::Inspect, Wide(env, name));
    if (value) observation = std::move(value->storage);
  } else {
    observation = gesher::usb::ObserveStorage(Wide(env, name));
  }
  if (!observation) return nullptr;
  jbyteArray raw = Bytes(env, observation->descriptors);
  if (!raw || env->ExceptionCheck()) return nullptr;
  jclass str = env->FindClass("java/lang/String");
  if (!str) return nullptr;
  jobjectArray strings = env->NewObjectArray(256, str, nullptr);
  if (!strings) return nullptr;
  for (const auto& [index, value] : observation->strings) {
    jstring text = String(env, value);
    if (!text) return nullptr;
    env->SetObjectArrayElement(strings, index, text);
    env->DeleteLocalRef(text);
    if (env->ExceptionCheck()) return nullptr;
  }
  jclass cls = env->FindClass("com/gesher/server/UsbHost$StorageDescriptors");
  if (!cls) return nullptr;
  jmethodID constructor = env->GetMethodID(cls, "<init>", "([B[Ljava/lang/String;)V");
  return constructor ? env->NewObject(cls, constructor, raw, strings) : nullptr;
} catch (const std::bad_alloc&) {
  return nullptr;
}

jlong Version(JNIEnv* env, jclass, jstring name) try {
  if (gesher::usb_storage::ProtectedRuntimeEnabled()) {
    auto value = ProtectedObservation(UsbObservationKind::Version, Wide(env, name));
    return value ? value->version : -1;
  }
  return gesher::usb::AttachmentVersion(Wide(env, name).c_str());
} catch (const std::bad_alloc&) {
  return -1;
}

jbyteArray Inspect(JNIEnv* env, jclass, jstring name) try {
  if (gesher::usb_storage::ProtectedRuntimeEnabled()) return nullptr;
  auto file = OpenDevice(Wide(env, name).c_str());
  if (!file) return nullptr;
  return Bytes(env, Connection(file.get()).Descriptors());
} catch (const std::bad_alloc&) {
  return nullptr;
}

jstring DeviceString(JNIEnv* env, jclass, jstring name, jint index) try {
  if (gesher::usb_storage::ProtectedRuntimeEnabled()) return nullptr;
  if (index < 1 || index > 255) return nullptr;
  auto file = OpenDevice(Wide(env, name).c_str());
  if (!file) return nullptr;
  auto value = Connection(file.get()).StringDescriptor(index);
  return value.empty() ? nullptr : String(env, value);
} catch (const std::bad_alloc&) {
  return nullptr;
}

jint Publish(JNIEnv* env, jclass, jstring name, jlong version) try {
  if (gesher::usb_storage::ProtectedRuntimeEnabled()) return -1;
  auto path = Wide(env, name);
  const auto paths = gesher::usb::InterfacePaths();
  if (version < 0 || !paths || std::find(paths->begin(), paths->end(), path) == paths->end() ||
      gesher::usb::AttachmentVersion(path.c_str()) != version) return -1;
  auto file = OpenDevice(path.c_str(), true);
  if (!file || Connection(file.get()).Descriptors().empty() ||
      gesher::usb::AttachmentVersion(path.c_str()) != version) return -1;
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file.get()),
                                _O_RDWR | _O_BINARY | _O_NOINHERIT);
  if (fd >= 0) file.release();
  return fd;
} catch (const std::bad_alloc&) {
  return -1;
}

struct OwnedFd {
  int number;
  explicit OwnedFd(int fd) : number(fd) {}
  OwnedFd(OwnedFd&& other) noexcept : number(std::exchange(other.number, -1)) {}
  ~OwnedFd() { if (number >= 0) close(number); }
};

struct State {
  explicit State(OwnedFd owned)
      : fd(std::move(owned)), file(reinterpret_cast<HANDLE>(_get_osfhandle(fd.number))),
        device(file) {}
  OwnedFd fd;
  HANDLE file;
  Connection device;
  std::timed_mutex wait;
  gesher::usb::File change{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  std::atomic<bool> closed{false};
};

std::mutex registry_lock;
std::unordered_map<jlong, std::shared_ptr<State>> registry;
jlong sequence;
void CloseRequests(JNIEnv* env, const std::shared_ptr<State>& state);

std::shared_ptr<State> Find(JNIEnv* env, jobject self) {
  std::lock_guard lock(registry_lock);
  auto item = registry.find(env->GetLongField(self, context));
  return item == registry.end() ? nullptr : item->second;
}

jboolean Open(JNIEnv* env, jobject self, jstring, jobject descriptor) try {
  if (!descriptor) return JNI_FALSE;
  jclass clazz = env->GetObjectClass(descriptor);
  if (!clazz) return JNI_FALSE;
  jfieldID field = env->GetFieldID(clazz, "descriptor", "I");
  env->DeleteLocalRef(clazz);
  if (!field) return JNI_FALSE;
  int copy = fcntl(env->GetIntField(descriptor, field), F_DUPFD_CLOEXEC, 0);
  if (copy < 0) return JNI_FALSE;
  OwnedFd owned{copy};
  auto state = std::make_shared<State>(std::move(owned));
  if (!state->device) return JNI_FALSE;
  std::lock_guard lock(registry_lock);
  if (env->GetLongField(self, context) != 0) return JNI_FALSE;
  jlong id = ++sequence;
  registry.emplace(id, state);
  env->SetLongField(self, context, id);
  return JNI_TRUE;
} catch (const std::bad_alloc&) {
  return JNI_FALSE;
}

void Close(JNIEnv* env, jobject self) {
  std::shared_ptr<State> state;
  {
    std::lock_guard lock(registry_lock);
    auto item = registry.find(env->GetLongField(self, context));
    if (item == registry.end()) return;
    state = std::move(item->second);
    registry.erase(item);
    env->SetLongField(self, context, 0);
  }
  state->closed.store(true);
  state->device.Cancel();
  if (state->change) SetEvent(state->change.get());
  CloseRequests(env, state);
}

jint Fd(JNIEnv* env, jobject self) {
  auto state = Find(env, self);
  return state ? state->fd.number : -1;
}

jbyteArray RawDescriptors(JNIEnv* env, jobject self) try {
  auto state = Find(env, self);
  if (!state) return nullptr;
  return Bytes(env, state->device.Descriptors());
} catch (const std::bad_alloc&) {
  return nullptr;
}

jstring Serial(JNIEnv* env, jobject self) try {
  auto state = Find(env, self);
  if (!state) return nullptr;
  auto descriptors = state->device.Descriptors();
  if (descriptors.empty()) return nullptr;
  auto serial = state->device.StringDescriptor(descriptors[16]);
  return serial.empty() ? nullptr : String(env, serial);
} catch (const std::bad_alloc&) {
  return nullptr;
}

jboolean Claim(JNIEnv* env, jobject self, jint number, jboolean) {
  auto state = Find(env, self);
  if (!state) return false;
  return state->device.Claim(number);
}

jboolean Release(JNIEnv* env, jobject self, jint number) {
  auto state = Find(env, self);
  if (!state) return false;
  return state->device.Release(number);
}

jboolean Alternate(JNIEnv* env, jobject self, jint number, jint alternate) {
  auto state = Find(env, self);
  if (!state) return false;
  return state->device.SetAlternate(number, alternate);
}

template<class Transfer>
jint TransferBytes(JNIEnv* env, jobject self, jbyteArray data, jint offset, jint size,
                   jint timeout, bool input, Transfer transfer) try {
  if (offset < 0 || size < 0 || timeout < 0 || (data == nullptr && (size != 0 || offset != 0)))
    return -1;
  if (data != nullptr && (offset > env->GetArrayLength(data) ||
      size > env->GetArrayLength(data) - offset)) return -1;
  auto state = Find(env, self);
  if (!state) return -1;
  std::vector<unsigned char> bytes(size);
  if (!input && size != 0)
    env->GetByteArrayRegion(data, offset, size, reinterpret_cast<jbyte*>(bytes.data()));
  if (env->ExceptionCheck()) return -1;
  const jint result = transfer(state->device, bytes.data(), size, timeout);
  if (input && result > 0 && result <= size)
    env->SetByteArrayRegion(data, offset, result, reinterpret_cast<const jbyte*>(bytes.data()));
  return result;
} catch (const std::bad_alloc&) {
  return -1;
}

jint Bulk(JNIEnv* env, jobject self, jint endpoint, jbyteArray data, jint offset, jint size,
          jint timeout) {
  return TransferBytes(env, self, data, offset, size, timeout, endpoint & 0x80,
                       [endpoint](Connection& device, unsigned char* bytes, unsigned n, unsigned ms) {
                         return device.Transfer(endpoint, bytes, n, ms);
                       });
}

jint Control(JNIEnv* env, jobject self, jint type, jint request, jint value, jint index,
             jbyteArray data, jint offset, jint size, jint timeout) {
  return TransferBytes(env, self, data, offset, size, timeout, type & 0x80,
                       [=](Connection& device, unsigned char* bytes, unsigned n, unsigned ms) {
                         return device.Control(type, request, value, index, bytes, n, ms);
                       });
}

jboolean SetConfiguration(JNIEnv*, jobject, jint) { return JNI_FALSE; }
jboolean Reset(JNIEnv*, jobject) { return JNI_FALSE; }
struct Request {
  std::shared_ptr<State> state;
  int endpoint;
  gesher::usb::File event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  OVERLAPPED operation{};
  std::vector<unsigned char> bytes;
  jobject reference = nullptr;
  jobject buffer_ref = nullptr;
  unsigned char* direct = nullptr;
  int actual = -1;
  bool submitted = false;
  bool delivered = false;
};

std::mutex requests_lock;
std::unordered_map<jlong, std::shared_ptr<Request>> requests;
jlong request_sequence;

// Callers hold requests_lock.
std::shared_ptr<Request> FindRequest(JNIEnv* env, jobject self) {
  auto entry = requests.find(env->GetLongField(self, request_context));
  return entry == requests.end() ? nullptr : entry->second;
}

jboolean RequestInit(JNIEnv* env, jobject self, jobject connection, jint endpoint,
                     jint attributes, jint, jint) try {
  auto state = Find(env, connection);
  if (!state || !state->change || state->closed.load() || endpoint < 1 || endpoint > 255 ||
      ((attributes & 3) != 2 && (attributes & 3) != 3)) return false;
  auto request = std::make_shared<Request>();
  if (!request->event) return false;
  request->state = state;
  request->endpoint = endpoint;
  std::lock_guard lock(requests_lock);
  if (state->closed.load() || FindRequest(env, self) || requests.size() >= 512) return false;
  jlong id = ++request_sequence;
  requests.emplace(id, std::move(request));
  env->SetLongField(self, request_context, id);
  return true;
} catch (const std::bad_alloc&) {
  return JNI_FALSE;
}

void ForgetQueue(JNIEnv* env, Request& request) {
  if (request.reference) env->DeleteGlobalRef(request.reference);
  if (request.buffer_ref) env->DeleteGlobalRef(request.buffer_ref);
  request.reference = nullptr;
  request.buffer_ref = nullptr;
  request.direct = nullptr;
  request.submitted = false;
  request.delivered = false;
  std::vector<unsigned char>().swap(request.bytes);
}

void CloseRequests(JNIEnv* env, const std::shared_ptr<State>& state) {
  std::lock_guard lock(requests_lock);
  for (auto it = requests.begin(); it != requests.end();) {
    auto& request = *it->second;
    if (request.state != state) { ++it; continue; }
    if (request.delivered) {
      // Java may be between native_request_wait and native_dequeue.
      request.state.reset();
      ++it;
      continue;
    }
    if (request.submitted) state->device.Result(&request.operation, true);
    ForgetQueue(env, request);
    it = requests.erase(it);
  }
}

void RequestClose(JNIEnv* env, jobject self) {
  std::lock_guard lock(requests_lock);
  auto request = FindRequest(env, self);
  if (!request) return;
  if (request->submitted && !request->delivered) {
    request->state->device.CancelRequest(&request->operation);
    request->state->device.Result(&request->operation, true);
  }
  ForgetQueue(env, *request);
  if (request->state) SetEvent(request->state->change.get());
  requests.erase(env->GetLongField(self, request_context));
  env->SetLongField(self, request_context, 0);
}

jboolean SubmitRequest(JNIEnv* env, jobject self, Request& request, jobject buffer,
                       unsigned char* direct, jbyteArray array, int size) try {
  if (!request.state || request.submitted || request.state->closed.load() ||
      size < 0 || size > 16 * 1024 * 1024)
    return false;
  int queued = 0;
  for (const auto& entry : requests) {
    const Request& other = *entry.second;
    if (other.state == request.state && other.submitted && !other.delivered) ++queued;
  }
  if (queued >= 32) return false;
  request.bytes.resize(size);
  if ((request.endpoint & 0x80) == 0 && size != 0) {
    if (direct) std::memcpy(request.bytes.data(), direct, size);
    else if (array) env->GetByteArrayRegion(array, 0, size,
                                            reinterpret_cast<jbyte*>(request.bytes.data()));
    else return false;
  }
  if (env->ExceptionCheck()) {
    ForgetQueue(env, request);
    return false;
  }
  request.reference = env->NewGlobalRef(self);
  if (buffer) request.buffer_ref = env->NewGlobalRef(buffer);
  if (!request.reference || (buffer && !request.buffer_ref)) {
    ForgetQueue(env, request);
    return false;
  }
  request.direct = direct;
  request.operation = {};
  request.operation.hEvent = request.event.get();
  ResetEvent(request.event.get());
  if (!request.state->device.Submit(request.endpoint, request.bytes.data(), size,
                                    &request.operation)) {
    ForgetQueue(env, request);
    return false;
  }
  request.submitted = true;
  SetEvent(request.state->change.get());
  return true;
} catch (const std::bad_alloc&) {
  ForgetQueue(env, request);
  return JNI_FALSE;
}

jboolean Queue(JNIEnv* env, jobject self, jobject buffer, jint offset, jint size) {
  std::lock_guard lock(requests_lock);
  auto request = FindRequest(env, self);
  if (!request || offset < 0 || size < 0) return false;
  unsigned char* direct = nullptr;
  if (buffer) {
    jlong capacity = env->GetDirectBufferCapacity(buffer);
    if (capacity < offset || size > capacity - offset) return false;
    direct = static_cast<unsigned char*>(env->GetDirectBufferAddress(buffer));
    if (direct) direct += offset;
    else if (size != 0) return false;
  } else if (offset != 0 || size != 0) return false;
  return SubmitRequest(env, self, *request, buffer, direct, nullptr, size);
}

jboolean QueueDirect(JNIEnv* env, jobject self, jobject buffer, jint size, jboolean) {
  return Queue(env, self, buffer, 0, size);
}

jboolean QueueArray(JNIEnv* env, jobject self, jbyteArray array, jint size, jboolean) {
  std::lock_guard lock(requests_lock);
  auto request = FindRequest(env, self);
  return request && size >= 0 && size <= (array ? env->GetArrayLength(array) : 0)
      && SubmitRequest(env, self, *request, array, nullptr, array, size);
}

jint DequeueArray(JNIEnv* env, jobject self, jbyteArray array, jint requested, jboolean) {
  std::lock_guard lock(requests_lock);
  auto request = FindRequest(env, self);
  if (!request || !request->delivered) return -1;
  int result = request->actual;
  if (array && (request->endpoint & 0x80) != 0 && result > 0) {
    int count = std::min({result, requested, env->GetArrayLength(array)});
    if (count > 0) env->SetByteArrayRegion(
        array, 0, count, reinterpret_cast<const jbyte*>(request->bytes.data()));
  }
  const bool detached = !request->state;
  ForgetQueue(env, *request);
  if (detached) requests.erase(env->GetLongField(self, request_context));
  return result;
}

jint DequeueDirect(JNIEnv* env, jobject self) {
  std::lock_guard lock(requests_lock);
  auto request = FindRequest(env, self);
  if (!request || !request->delivered) return -1;
  int result = request->actual;
  if (request->direct && (request->endpoint & 0x80) != 0 && result > 0 &&
      static_cast<size_t>(result) <= request->bytes.size())
    std::memcpy(request->direct, request->bytes.data(), result);
  const bool detached = !request->state;
  ForgetQueue(env, *request);
  if (detached) requests.erase(env->GetLongField(self, request_context));
  return result;
}

jboolean RequestCancel(JNIEnv* env, jobject self) {
  std::lock_guard lock(requests_lock);
  auto request = FindRequest(env, self);
  return request && request->submitted && !request->delivered &&
      request->state->device.CancelRequest(&request->operation);
}

jobject TimedOut(JNIEnv* env) {
  jclass error = env->FindClass("java/util/concurrent/TimeoutException");
  if (error) env->ThrowNew(error, "USB request wait timed out");
  return nullptr;
}

jobject RequestWait(JNIEnv* env, jobject self, jlong timeout) {
  auto state = Find(env, self);
  if (!state || !state->change) return nullptr;
  const ULONGLONG start = GetTickCount64();
  // One event consumer per connection prevents a second waiter from clearing
  // the queue-change event before the first waiter has observed it.
  std::unique_lock<std::timed_mutex> waiter(state->wait, std::defer_lock);
  if (timeout < 0) waiter.lock();
  else if (!waiter.try_lock_for(std::chrono::milliseconds(
               std::min<jlong>(timeout, INFINITE - 1)))) return TimedOut(env);
  for (;;) {
    constexpr size_t kMaxPending = 32;
    std::shared_ptr<Request> pinned[kMaxPending];
    HANDLE waits[kMaxPending + 1]{state->change.get()};
    DWORD count = 1;
    {
      std::lock_guard lock(requests_lock);
      ResetEvent(state->change.get());
      if (state->closed.load()) return nullptr;
      for (const auto& entry : requests) {
        const auto& request = entry.second;
        if (request->state != state || !request->submitted || request->delivered) continue;
        if (WaitForSingleObject(request->event.get(), 0) == WAIT_OBJECT_0) {
          jobject reference = env->NewLocalRef(request->reference);
          if (!reference) return nullptr;
          // usbdevfs returns a reaped request with a zero byte count when no
          // data was transferred, including cancellation.
          request->actual = std::max(0, state->device.Result(&request->operation, false));
          request->delivered = true;
          return reference;
        }
        pinned[count - 1] = request;
        waits[count++] = request->event.get();
      }
    }
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (timeout >= 0 && elapsed >= static_cast<ULONGLONG>(timeout)) return TimedOut(env);
    DWORD delay = timeout < 0 ? INFINITE : static_cast<DWORD>(
        std::min<ULONGLONG>(static_cast<ULONGLONG>(timeout) - elapsed, INFINITE - 1));
    DWORD result = WaitForMultipleObjects(count, waits, FALSE, delay);
    if (result == WAIT_FAILED) return nullptr;
  }
}

jint DeviceId(JNIEnv* env, jclass, jstring device) try {
  std::wstring path = Wide(env, device);
  int bus = -1, address = -1, end = -1;
  if (std::swscanf(path.c_str(), L"/dev/bus/usb/%3d/%3d%n", &bus, &address, &end) != 2 ||
      end != static_cast<int>(path.size()) || bus < 1 || bus > 999 ||
      address < 1 || address > 127) return -1;
  return bus * 1000 + address;
} catch (const std::bad_alloc&) {
  return -1;
}

jstring DeviceName(JNIEnv* env, jclass, jint id) {
  if (id < 1001 || id > 999127 || id % 1000 < 1 || id % 1000 > 127) return nullptr;
  wchar_t name[32];
  std::swprintf(name, std::size(name), L"/dev/bus/usb/%03d/%03d", id / 1000, id % 1000);
  return String(env, name);
}

#define USB_METHOD(name, signature, fn) {name, signature, GESHER_JNI(fn)}
const JNINativeMethod host_methods[] = {
    USB_METHOD("nativeAvailable", "()Z", Available),
    USB_METHOD("nativePaths", "()[Ljava/lang/String;", Paths),
    USB_METHOD("nativeStoragePaths", "()[Ljava/lang/String;", StoragePaths),
    USB_METHOD("nativeInspectStorage",
               "(Ljava/lang/String;)Lcom/gesher/server/UsbHost$StorageDescriptors;", InspectStorage),
    USB_METHOD("nativeVersion", "(Ljava/lang/String;)J", Version),
    USB_METHOD("nativeInspect", "(Ljava/lang/String;)[B", Inspect),
    USB_METHOD("nativeString", "(Ljava/lang/String;I)Ljava/lang/String;", DeviceString),
    USB_METHOD("nativeOpen", "(Ljava/lang/String;J)I", Publish),
};
const JNINativeMethod connection_methods[] = {
    USB_METHOD("native_open", "(Ljava/lang/String;Ljava/io/FileDescriptor;)Z", Open),
    USB_METHOD("native_close", "()V", Close),
    USB_METHOD("native_get_fd", "()I", Fd),
    USB_METHOD("native_get_desc", "()[B", RawDescriptors),
    USB_METHOD("native_get_serial", "()Ljava/lang/String;", Serial),
    USB_METHOD("native_claim_interface", "(IZ)Z", Claim),
    USB_METHOD("native_release_interface", "(I)Z", Release),
    USB_METHOD("native_set_interface", "(II)Z", Alternate),
    USB_METHOD("native_control_request", "(IIII[BIII)I", Control),
    USB_METHOD("native_bulk_request", "(I[BIII)I", Bulk),
    USB_METHOD("native_set_configuration", "(I)Z", SetConfiguration),
    USB_METHOD("native_reset_device", "()Z", Reset),
    USB_METHOD("native_request_wait", "(J)Landroid/hardware/usb/UsbRequest;", RequestWait),
};
const JNINativeMethod device_methods[] = {
    USB_METHOD("native_get_device_id", "(Ljava/lang/String;)I", DeviceId),
    USB_METHOD("native_get_device_name", "(I)Ljava/lang/String;", DeviceName),
};
const JNINativeMethod request_methods[] = {
    USB_METHOD("native_init", "(Landroid/hardware/usb/UsbDeviceConnection;IIII)Z", RequestInit),
    USB_METHOD("native_close", "()V", RequestClose),
    USB_METHOD("native_queue", "(Ljava/nio/ByteBuffer;II)Z", Queue),
    USB_METHOD("native_queue_array", "([BIZ)Z", QueueArray),
    USB_METHOD("native_dequeue_array", "([BIZ)I", DequeueArray),
    USB_METHOD("native_queue_direct", "(Ljava/nio/ByteBuffer;IZ)Z", QueueDirect),
    USB_METHOD("native_dequeue_direct", "()I", DequeueDirect),
    USB_METHOD("native_cancel", "()Z", RequestCancel),
};

// JNI registration runs before the guest main class executes. Only the
// independently launched system_server gets host inventory access. Protected
// identity comes from the authenticated executable bootstrap, never argv text.
bool IsSystemServerEntryPoint() {
  if (gesher::usb_storage::ProtectedRuntimeEnabled())
    return gesher::usb_storage::ProtectedRuntimeIsSystemServer();
  int count = 0;
  wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!args) return false;
  bool server = false;
  for (int i = 1; i < count; ++i) {
    if (std::wcscmp(args[i], L"-cp") == 0 || std::wcscmp(args[i], L"-classpath") == 0) {
      ++i;
      continue;
    }
    if (args[i][0] == L'-') continue;
    server = std::wcscmp(args[i], L"com.gesher.server.SystemServer") == 0;
    break;
  }
  LocalFree(args);
  return server;
}
}  // namespace

int register_gesher_usb(JNIEnv* env) {
  jclass connection = env->FindClass("android/hardware/usb/UsbDeviceConnection");
  if (!connection) return -1;
  context = env->GetFieldID(connection, "mNativeContext", "J");
  env->DeleteLocalRef(connection);
  if (!context) return -1;
  if (RegisterGesherNatives(env, "android/hardware/usb/UsbDeviceConnection",
                           connection_methods, std::size(connection_methods)) != 0) return -1;
  if (RegisterGesherNatives(env, "android/hardware/usb/UsbDevice",
                           device_methods, std::size(device_methods)) != 0) return -1;
  jclass request = env->FindClass("android/hardware/usb/UsbRequest");
  if (!request) return -1;
  request_context = env->GetFieldID(request, "mNativeContext", "J");
  env->DeleteLocalRef(request);
  if (!request_context || RegisterGesherNatives(env, "android/hardware/usb/UsbRequest",
                                             request_methods, std::size(request_methods)) != 0)
    return -1;
  return !IsSystemServerEntryPoint() ? 0
      : RegisterGesherNatives(env, "com/gesher/server/UsbHost",
                             host_methods, std::size(host_methods));
}
}  // namespace android
