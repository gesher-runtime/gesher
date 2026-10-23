// libbinder across processes on Windows: servicemanager (compat/binder) and
// two processes over binderd, through libbinder's own ProcessState,
// IPCThreadState, BpBinder and Parcel. Run in CI on Windows.
//
//   libbinder_probe            starts binderd and servicemanager (next to
//                              this program) on a device of its own, then a
//                              service process, and is the client
//   libbinder_probe service    the service side
//
// The service registers "gesher.probe" with servicemanager. The client finds
// it there, calls it with a Parcel holding a Binder of its own; the service
// calls that Binder back (a nested call into the waiting client) and replies
// with the client's Binder and itself, which must come back as the client's
// very object and the same proxy. The client lists the services, links to
// the service's death, and tells it to exit: binderDied must follow on the
// client's binder thread, and the proxy must then be dead.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <android/os/IServiceManager.h>
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <utils/String8.h>

using android::BBinder;
using android::IBinder;
using android::interface_cast;
using android::IPCThreadState;
using android::Parcel;
using android::ProcessState;
using android::sp;
using android::status_t;
using android::String16;
using android::String8;
using android::wp;

namespace {

int failures = 0;

#define EXPECT(cond)                                                        \
  do {                                                                      \
    if (!(cond)) {                                                          \
      fprintf(stderr, "FAILED %s (line %d)\n", #cond, __LINE__);            \
      ++failures;                                                           \
    }                                                                       \
  } while (0)

constexpr uint32_t kPing = IBinder::FIRST_CALL_TRANSACTION;
constexpr uint32_t kExit = IBinder::FIRST_CALL_TRANSACTION + 1;
constexpr uint32_t kCallback = IBinder::FIRST_CALL_TRANSACTION + 2;
const char kServiceName[] = "gesher.probe";

sp<android::os::IServiceManager> ServiceManager() {
  sp<IBinder> context = ProcessState::self()->getContextObject(nullptr);
  if (context == nullptr) return nullptr;
  return interface_cast<android::os::IServiceManager>(context);
}

class ProbeService : public BBinder {
 protected:
  status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
    switch (code) {
      case kPing: {
        sp<IBinder> callback = data.readStrongBinder();
        String16 text = data.readString16();
        if (callback == nullptr || text != String16("hello")) return android::BAD_VALUE;
        // Back into the client, which waits for this call's reply.
        Parcel d, r;
        d.writeString16(String16("from the service"));
        status_t s = callback->transact(kCallback, d, &r);
        if (s != android::OK) return s;
        reply->writeString16(r.readString16());
        reply->writeStrongBinder(callback);
        reply->writeStrongBinder(sp<IBinder>::fromExisting(this));
        reply->writeInt32(static_cast<int32_t>(IPCThreadState::self()->getCallingPid()));
        return android::OK;
      }
      case kExit:
        reply->writeInt32(0);
        std::thread([] {
          Sleep(200);
          ExitProcess(0);
        }).detach();
        return android::OK;
      default:
        return BBinder::onTransact(code, data, reply, flags);
    }
  }
};

int Service() {
  sp<android::os::IServiceManager> sm = ServiceManager();
  if (sm == nullptr) {
    fprintf(stderr, "service: no context object\n");
    return 1;
  }
  sp<ProbeService> service = sp<ProbeService>::make();
  android::binder::Status st =
      sm->addService(kServiceName, service, false,
                     android::os::IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT);
  if (!st.isOk()) {
    fprintf(stderr, "service: addService failed: %s\n", st.toString8().c_str());
    return 1;
  }
  fprintf(stderr, "service: registered\n");
  ProcessState::self()->startThreadPool();
  IPCThreadState::self()->joinThreadPool();
  return 0;
}

class Callback : public BBinder {
 protected:
  status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
    if (code != kCallback) return BBinder::onTransact(code, data, reply, flags);
    called_on = GetCurrentThreadId();
    String16 text("callback got: ");
    text.append(data.readString16());
    reply->writeString16(text);
    return android::OK;
  }

 public:
  DWORD called_on = 0;
};

struct Recipient : IBinder::DeathRecipient {
  std::mutex mutex;
  std::condition_variable cv;
  bool died = false;
  void binderDied(const wp<IBinder>&) override {
    std::lock_guard<std::mutex> lock(mutex);
    died = true;
    cv.notify_all();
  }
};

HANDLE Spawn(const std::wstring& exe, const std::wstring& args) {
  std::wstring cmdline = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                      &si, &pi)) {
    fprintf(stderr, "cannot start %ls (%lu)\n", exe.c_str(), GetLastError());
    return nullptr;
  }
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

int Client() {
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring dir = self;
  dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
  char device[64];
  snprintf(device, sizeof(device), "libbinder-probe-%lu", GetCurrentProcessId());
  std::string env = std::string("GESHER_BINDER=") + device;
  _putenv(env.c_str());
  std::wstring wdevice(device, device + strlen(device));

  HANDLE binderd = Spawn(dir + L"binderd.exe", wdevice);
  // The device appears once binderd serves it.
  for (int i = 0; i < 100; ++i) {
    int fd = open("/dev/binder", O_RDWR);
    if (fd >= 0) {
      close(fd);
      break;
    }
    Sleep(50);
  }
  HANDLE servicemanager = Spawn(dir + L"servicemanager.exe", L"");
  HANDLE service = nullptr;
  auto cleanup = [&] {
    for (HANDLE h : {service, servicemanager, binderd}) {
      if (h != nullptr) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 5000);
      }
    }
  };
  if (binderd == nullptr || servicemanager == nullptr) {
    cleanup();
    return 1;
  }

  // Handle 0 answers once servicemanager is the context manager.
  sp<IBinder> context = ProcessState::self()->getContextObject(nullptr);
  bool up = false;
  for (int i = 0; i < 200 && !up; ++i) {
    up = context != nullptr && context->pingBinder() == android::OK;
    if (!up) {
      Sleep(50);
      context = ProcessState::self()->getContextObject(nullptr);
    }
  }
  EXPECT(up);
  if (!up) {
    cleanup();
    return 1;
  }
  // Incoming calls (binderDied) need a binder thread.
  ProcessState::self()->startThreadPool();

  service = Spawn(self, L"service");
  sp<android::os::IServiceManager> sm = interface_cast<android::os::IServiceManager>(context);
  sp<IBinder> remote;
  for (int i = 0; i < 200 && remote == nullptr; ++i) {
    android::os::Service s;
    if (sm->checkService2(kServiceName, &s).isOk() &&
        s.getTag() == android::os::Service::Tag::serviceWithMetadata) {
      remote = s.get<android::os::Service::Tag::serviceWithMetadata>().service;
    }
    if (remote == nullptr) Sleep(50);
  }
  EXPECT(remote != nullptr);
  if (remote == nullptr) {
    cleanup();
    return 1;
  }
  EXPECT(remote->remoteBinder() != nullptr);  // a proxy, in another process

  sp<Callback> callback = sp<Callback>::make();
  Parcel data, reply;
  data.writeStrongBinder(callback);
  data.writeString16(String16("hello"));
  status_t s = remote->transact(kPing, data, &reply);
  EXPECT(s == android::OK);
  if (s == android::OK) {
    EXPECT(String8(reply.readString16()) == "callback got: from the service");
    sp<IBinder> back = reply.readStrongBinder();
    sp<IBinder> service_self = reply.readStrongBinder();
    int32_t calling_pid = reply.readInt32();
    EXPECT(back.get() == callback.get());  // the client's own object
    EXPECT(service_self.get() == remote.get());  // the same proxy
    EXPECT(calling_pid == static_cast<int32_t>(GetCurrentProcessId()));
    EXPECT(callback->called_on == GetCurrentThreadId());  // the nested call
  }

  std::vector<std::string> names;
  EXPECT(sm->listServices(android::os::IServiceManager::DUMP_FLAG_PRIORITY_ALL, &names).isOk());
  bool listed = false, manager = false;
  for (const std::string& n : names) {
    listed |= n == kServiceName;
    manager |= n == "manager";
  }
  EXPECT(listed && manager);

  sp<Recipient> recipient = sp<Recipient>::make();
  EXPECT(remote->linkToDeath(recipient) == android::OK);
  Parcel exit_data, exit_reply;
  EXPECT(remote->transact(kExit, exit_data, &exit_reply) == android::OK);
  {
    std::unique_lock<std::mutex> lock(recipient->mutex);
    recipient->cv.wait_for(lock, std::chrono::seconds(20), [&] { return recipient->died; });
    EXPECT(recipient->died);
  }
  EXPECT(remote->pingBinder() == android::DEAD_OBJECT);
  EXPECT(!remote->isBinderAlive());

  cleanup();
  if (failures == 0) {
    printf("libbinder probe: all passed\n");
  } else {
    printf("libbinder probe: %d failure(s)\n", failures);
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1 && strcmp(argv[1], "service") == 0) return Service();
  return Client();
}
