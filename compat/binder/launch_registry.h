// Broker-owned launch/admission primitive. This is NOT a public wire operation.
// The caller of Publish is the private inherited bootstrap control worker;
// never invoke it from a client hello, PID query, environment or claimed role.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace gesher { namespace binder_launch {
struct Install {
  std::string package;
  std::array<uint8_t, 16> generation;
  uint32_t uid;
  bool system = false; // only private bootstrap worker may select reserved instances
  bool operator==(const Install& other) const {
    return package == other.package && generation == other.generation && uid == other.uid &&
        system == other.system;
  }
};

struct Record {
  HANDLE process;  // owned broker-relative duplicate of actual CreateProcess HANDLE
  const DWORD pid;
  const uint64_t birth;
  const Install install;
  bool revoked = false;  // protected by Registry::lock_, never by client key
  unsigned proofs = 0;
  Record(HANDLE h, DWORD p, uint64_t b, Install i)
      : process(h), pid(p), birth(b), install(std::move(i)) {}
  ~Record() { CloseHandle(process); }
  Record(const Record&) = delete;
  Record& operator=(const Record&) = delete;
};

class Registry {
 public:
  using Lease = std::shared_ptr<Record>;
  // Takes ownership even on rejection. Package install must already have been
  // authorized by the trusted PM writer under its publication/revocation gate.
  bool Publish(HANDLE owned_process, const Install& install) {
    FILETIME born{}, exited{}, kernel{}, user{};
    DWORD pid = owned_process ? GetProcessId(owned_process) : 0;
    bool valid_uid = install.system
        ? ((install.package == "@launcher" && install.uid == 2000) ||
           ((install.package == "@servicemanager" || install.package == "@system_server") && install.uid == 1000))
        : (!install.package.empty() && install.package[0] != '@' && install.uid >= 10000 && install.uid <= 19999);
    if (!pid || pid > INT32_MAX || !valid_uid ||
        WaitForSingleObject(owned_process, 0) != WAIT_TIMEOUT ||
        !GetProcessTimes(owned_process, &born, &exited, &kernel, &user)) {
      if (owned_process) CloseHandle(owned_process);
      return false;
    }
    uint64_t birth = (uint64_t(born.dwHighDateTime) << 32) | born.dwLowDateTime;
    auto record = std::make_shared<Record>(owned_process, pid, birth, install);
    std::lock_guard<std::mutex> guard(lock_);
    auto old = records_.find(pid);
    if (old != records_.end() && !LiveLocked(old->second)) {
      old->second->revoked = true;
      records_.erase(old);
    }
    if (!birth || !authority_alive_ || records_.count(pid) || revoked_.count({pid, birth}))
      return false;
    records_.emplace(pid, std::move(record));
    return true;
  }

  // Numeric PID only indexes a PRE-EXISTING instance. No OpenProcess here.
  Lease Find(DWORD transport_pid, uint64_t observed_birth, uint32_t claimed_uid) {
    std::lock_guard<std::mutex> guard(lock_);
    auto found = records_.find(transport_pid);
    if (found == records_.end() || !LiveLocked(found->second) ||
        found->second->birth != observed_birth || found->second->install.uid != claimed_uid)
      return {};
    return found->second;
  }

  // Live challenge closes the PID-query/OpenProcess reuse gap: event is created
  // afresh and duplicated into the retained registered instance, never reopened
  // by numeric PID. The public connection receives only that child's HANDLE
  // locator. It must SetEvent and acknowledge on the SAME connection. A buffered
  // hello from a dead/reused PID cannot signal an event in the new instance.
  // Sender must use a bounded transport write, then ReadAck with same deadline.
  template<class Send, class ReadAck>
  bool Challenge(const Lease& record, Send send, ReadAck ack) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    HANDLE remote = nullptr;
    {
      std::lock_guard<std::mutex> guard(lock_);
      // Bound hostile proof retries/remote handles to this app instance.
      if (!LiveLocked(record) || record->proofs >= 64 ||
          !DuplicateHandle(GetCurrentProcess(), event, record->process, &remote,
                           EVENT_MODIFY_STATE, FALSE, 0)) {
        CloseHandle(event);
        return false;
      }
      ++record->proofs;
    }
    uint64_t deadline = GetTickCount64() + 5000;
    bool ok = send(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(remote)), deadline)
        && ack(deadline);
    HANDLE waits[]{record->process, event};
    uint64_t now = GetTickCount64();
    DWORD left = now < deadline ? DWORD(deadline - now) : 0;
    ok = ok && WaitForMultipleObjects(2, waits, FALSE, left) == WAIT_OBJECT_0 + 1;
    CloseHandle(event);
    // Cooperative client closes its own duplicate after signalling. Never use
    // DUPLICATE_CLOSE_SOURCE against a remote handle: guest may have closed and
    // reused the slot. An uncooperative app leaks at most 64 handles in itself.
    return ok && Live(record);
  }

  bool Live(const Lease& record) {
    std::lock_guard<std::mutex> guard(lock_);
    return LiveLocked(record);
  }
  void Revoke(const Install& install) {
    std::lock_guard<std::mutex> guard(lock_);
    for (auto it = records_.begin(); it != records_.end();) {
      if (it->second->install == install) {
        it->second->revoked = true;
        revoked_.emplace(it->second->pid, it->second->birth);
        it = records_.erase(it);
      } else ++it;
    }
  }
  void RevokeGeneration(const std::string& package, const std::array<uint8_t, 16>& generation) {
    std::lock_guard<std::mutex> guard(lock_);
    for (auto it = records_.begin(); it != records_.end();) {
      if (!it->second->install.system && it->second->install.package == package &&
          it->second->install.generation == generation) {
        it->second->revoked = true;
        revoked_.emplace(it->second->pid, it->second->birth);
        it = records_.erase(it);
      } else ++it;
    }
  }
  void AuthorityDied() {
    // All retained leases become invalid even while an accepted pipe stays open.
    std::lock_guard<std::mutex> guard(lock_);
    authority_alive_ = false;
    for (auto& entry : records_) entry.second->revoked = true;
    records_.clear();
  }
  void Cancel(DWORD pid, uint64_t birth) {
    std::lock_guard<std::mutex> guard(lock_);
    auto found = records_.find(pid);
    if (found != records_.end() && found->second->birth == birth) {
      found->second->revoked = true;
      revoked_.emplace(pid, birth);
      records_.erase(found);
    }
  }
 private:
  bool LiveLocked(const Lease& record) {
    return authority_alive_ && record && !record->revoked &&
        WaitForSingleObject(record->process, 0) == WAIT_TIMEOUT;
  }
  std::mutex lock_;
  bool authority_alive_ = true;
  std::map<DWORD, Lease> records_;
  // Never republish an uninstalled process under a new generation. Retained
  // tombstones are per broker lifetime; a reused PID with a new birth is distinct.
  std::set<std::pair<DWORD, uint64_t>> revoked_;
};
} }
