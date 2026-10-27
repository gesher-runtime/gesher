// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Common POSIX actor for host and both Android ABIs. The CI parent launches
// separate broker/server/client processes and controls only ordinary files;
// no production pause/failure/allocator hooks are used.
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace binder_sg_fixture {

struct Report {
  const char* role;
  unsigned checks = 0, failed = 0;
  bool Check(bool condition, const char* name) {
    ++checks;
    if (!condition) ++failed;
    printf("%s SG %s: %s\n", condition ? "PASS" : "FAIL", role, name);
    fflush(stdout);
    return condition;
  }
  int Finish() const {
    printf("SG %s: %u checks, %u failed\n", role, checks, failed);
    fflush(stdout);
    return failed ? 1 : 0;
  }
};

inline bool Mark(const std::string& path) {
  int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0) return false;
  return close(fd) == 0;
}
inline bool WaitFile(const std::string& path) {
  for (unsigned i = 0; i < 1500; ++i) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd >= 0) { close(fd); return true; }
    if (errno != ENOENT) return false;
    usleep(20000);
  }
  return false;
}

template <typename T> void Append(std::vector<uint8_t>* bytes, const T& value) {
  const auto* first = reinterpret_cast<const uint8_t*>(&value);
  bytes->insert(bytes->end(), first, first + sizeof(value));
}

struct Command {
  uint32_t code = 0;
  binder_transaction_data transaction{};
};

struct Driver {
  int fd = -1;
  uint8_t* mapping = nullptr;
  size_t capacity = 0;
  std::vector<uint8_t> outgoing;
  ~Driver() {
    if (mapping) munmap(mapping, capacity);
    if (fd >= 0) close(fd);
  }
  bool Open(size_t bytes = 1 << 20) {
    for (unsigned i = 0; i < 1500; ++i) {
      fd = open("/dev/binder", O_RDWR | O_CLOEXEC | O_NONBLOCK);
      if (fd >= 0 || errno != ENOENT) break;
      usleep(20000);
    }
    if (fd < 0) return false;
    void* result = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
    if (result == MAP_FAILED) return false;
    mapping = static_cast<uint8_t*>(result);
    capacity = bytes;
    uint32_t maximum = 0;
    return ioctl(fd, BINDER_SET_MAX_THREADS, &maximum) == 0;
  }
  bool InMapping(uint64_t pointer, uint64_t length) const {
    const uint64_t start = reinterpret_cast<uintptr_t>(mapping);
    return pointer >= start && pointer - start <= capacity &&
        length <= capacity - (pointer - start);
  }
  void Code(uint32_t value) { Append(&outgoing, value); }
  void Free(const binder_transaction_data& tr) {
    Code(BC_FREE_BUFFER);
    Append(&outgoing, tr.data.ptr.buffer);
  }
  int Talk(std::vector<Command>* commands, bool read = true, bool invalid_read = false,
           uint64_t* consumed = nullptr, void* custom_read = nullptr) {
    uint8_t incoming[1024]{};
    binder_write_read rw{};
    rw.write_size = outgoing.size();
    rw.write_buffer = reinterpret_cast<binder_uintptr_t>(outgoing.data());
    rw.read_size = read ? sizeof(incoming) : 0;
    auto* destination = custom_read ? static_cast<uint8_t*>(custom_read) : incoming;
    rw.read_buffer = invalid_read ? 1 : reinterpret_cast<binder_uintptr_t>(destination);
    const int status = ioctl(fd, BINDER_WRITE_READ, &rw);
    const int saved = errno;
    if (rw.write_consumed > outgoing.size() || rw.read_consumed > sizeof(incoming)) return EPROTO;
    outgoing.erase(outgoing.begin(), outgoing.begin() + rw.write_consumed);
    if (consumed) *consumed = rw.read_consumed;
    // Invalid user memory is deliberately never dereferenced by the fixture.
    if (!invalid_read) {
      size_t at = 0;
      while (at < rw.read_consumed) {
        if (rw.read_consumed - at < sizeof(uint32_t)) return EPROTO;
        Command command;
        memcpy(&command.code, destination + at, sizeof(command.code));
        const size_t size = _IOC_SIZE(command.code);
        if (size > rw.read_consumed - at - sizeof(uint32_t)) return EPROTO;
        if (command.code == BR_TRANSACTION || command.code == BR_REPLY) {
          if (size != sizeof(command.transaction)) return EPROTO;
          memcpy(&command.transaction, destination + at + 4, sizeof(command.transaction));
        }
        if (commands) commands->push_back(command);
        at += 4 + size;
      }
    }
    return status == 0 ? 0 : saved;
  }
  bool Flush() { return Talk(nullptr, false) == 0 && outgoing.empty(); }
  bool Next(Command* transaction, uint32_t expected) {
    for (unsigned i = 0; i < 1500; ++i) {
      std::vector<Command> commands;
      int error = Talk(&commands);
      if (error != 0 && error != EAGAIN) return false;
      for (const auto& command : commands) {
        if (command.code == expected) { *transaction = command; return true; }
        if (command.code != BR_NOOP && command.code != BR_TRANSACTION_COMPLETE &&
            command.code != BR_SPAWN_LOOPER) return false;
      }
      usleep(20000);
    }
    return false;
  }
};

// Positive parent slots start at16, avoiding the kernel's offset-zero
// last-fixup sentinel. Three independent original allocations are copied.
struct Payload {
  std::vector<uint8_t> root, child, leaf, data;
  std::vector<binder_size_t> offsets;
  explicit Payload(bool large = false) : root(large ? 16384 : 64), child(32), leaf(17), data(136, 0x5a) {
    for (size_t i = 0; i < root.size(); ++i) root[i] = static_cast<uint8_t>(i * 3 + 11);
    for (size_t i = 0; i < child.size(); ++i) child[i] = static_cast<uint8_t>(i * 5 + 13);
    for (size_t i = 0; i < leaf.size(); ++i) leaf[i] = static_cast<uint8_t>(i * 7 + 17);
    Add(16, root, 0, 0, false);
    Add(56, child, 0, 16, true);
    Add(96, leaf, 1, 0, true);
  }
  void Add(size_t offset, const std::vector<uint8_t>& bytes, size_t parent, size_t slot, bool has_parent) {
    binder_buffer_object object{};
    object.hdr.type = BINDER_TYPE_PTR;
    object.flags = has_parent ? BINDER_BUFFER_FLAG_HAS_PARENT : 0;
    object.buffer = reinterpret_cast<binder_uintptr_t>(bytes.data());
    object.length = bytes.size();
    object.parent = parent;
    object.parent_offset = slot;
    memcpy(data.data() + offset, &object, sizeof(object));
    offsets.push_back(offset);
  }
  binder_buffer_object Object(size_t index) const {
    binder_buffer_object object;
    memcpy(&object, data.data() + offsets[index], sizeof(object));
    return object;
  }
  void Set(size_t index, const binder_buffer_object& object) {
    memcpy(data.data() + offsets[index], &object, sizeof(object));
  }
  size_t Extra() const { return ((root.size() + 7) & ~size_t{7}) + 32 + 24; }
  void Queue(Driver* driver, uint32_t command, uint32_t code, uint32_t flags = 0) const {
    binder_transaction_data_sg sg{};
    sg.transaction_data.code = code;
    sg.transaction_data.flags = flags;
    sg.transaction_data.data_size = data.size();
    sg.transaction_data.offsets_size = offsets.size() * sizeof(binder_size_t);
    sg.transaction_data.data.ptr.buffer = reinterpret_cast<binder_uintptr_t>(data.data());
    sg.transaction_data.data.ptr.offsets = reinterpret_cast<binder_uintptr_t>(offsets.data());
    sg.buffers_size = Extra();
    driver->Code(command);
    Append(&driver->outgoing, sg);
  }
  void DestroyOriginalBytes() {
    std::vector<uint8_t>().swap(root);
    std::vector<uint8_t>().swap(child);
    std::vector<uint8_t>().swap(leaf);
    // Encourage immediate allocator reuse after originals have been freed.
    root.assign(65536, 0xee);
  }
  void AddFd(int fd) {
    binder_fd_object object{};
    object.hdr.type = BINDER_TYPE_FD;
    object.fd = static_cast<uint32_t>(fd);
    offsets.push_back(data.size());
    Append(&data, object);
  }
};

inline bool Verify(Driver* driver, const binder_transaction_data& tr, Report* report) {
  bool valid = report->Check(tr.data_size == 136 && tr.offsets_size == 24 &&
      driver->InMapping(tr.data.ptr.buffer, tr.data_size) &&
      driver->InMapping(tr.data.ptr.offsets, tr.offsets_size), "data and offsets belong to receiver mapping");
  if (!valid) return false;
  binder_size_t offsets[3];
  memcpy(offsets, reinterpret_cast<const void*>(tr.data.ptr.offsets), sizeof(offsets));
  if (!report->Check(offsets[0] == 16 && offsets[1] == 56 && offsets[2] == 96, "ordered object table retained")) return false;
  binder_buffer_object objects[3];
  const size_t sizes[]{64, 32, 17};
  for (size_t i = 0; i < 3; ++i) {
    memcpy(&objects[i], reinterpret_cast<const uint8_t*>(tr.data.ptr.buffer) + offsets[i], sizeof(objects[i]));
    valid &= report->Check(objects[i].hdr.type == BINDER_TYPE_PTR && objects[i].length == sizes[i] &&
        driver->InMapping(objects[i].buffer, objects[i].length), "PTR targets receiver-owned mapped bytes");
  }
  if (!valid) return false;
  binder_uintptr_t child, leaf;
  memcpy(&child, reinterpret_cast<const uint8_t*>(objects[0].buffer) + 16, sizeof(child));
  memcpy(&leaf, reinterpret_cast<const void*>(objects[1].buffer), sizeof(leaf));
  valid &= report->Check(child == objects[1].buffer && leaf == objects[2].buffer, "two nested parent slots rebased");
  for (size_t part = 0; part < 3; ++part) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(objects[part].buffer);
    bool same = true;
    for (size_t i = 0; i < sizes[part]; ++i) {
      if ((part == 0 && i >= 16 && i < 24) || (part == 1 && i < 8)) continue;
      const uint8_t expected = static_cast<uint8_t>(i * (part == 0 ? 3 : part == 1 ? 5 : 7) +
          (part == 0 ? 11 : part == 1 ? 13 : 17));
      same &= bytes[i] == expected;
    }
    valid &= report->Check(same, "copied payload survives sender mutation/free/death");
  }
  return valid;
}

inline int Client(const char* scenario, const std::string& prefix) {
  Report report{"client"};
  Driver driver;
  if (!report.Check(driver.Open(), "open independent sender")) return report.Finish();
  if (!strcmp(scenario, "malformed")) {
    for (unsigned test = 0; test < 11; ++test) {
      Payload payload;
      auto object = payload.Object(1);
      switch (test) {
        case 0: payload.offsets[1] = 20; break; // overlap
        case 1: payload.offsets[1] = UINT64_MAX; break;
        case 2: object.length = UINT64_MAX; payload.Set(1, object); break;
        case 3: object.parent = 2; payload.Set(1, object); break; // future
        case 4: object.parent_offset = 60; payload.Set(1, object); break;
        case 5: object.buffer = 1; payload.Set(1, object); break; // real bad user pointer
        case 6: payload.offsets[0] = 17; break;
        case 7: object.hdr.type = BINDER_TYPE_FDA; payload.Set(1, object); break;
        case 8: object.parent_offset = UINT64_MAX; payload.Set(1, object); break;
        case 9: payload.offsets[2] = 56; break; // repeated object
        case 10: object.parent = UINT64_MAX; payload.Set(1, object); break;
      }
      payload.Queue(&driver, BC_TRANSACTION_SG, 100 + test);
      if (!report.Check(driver.Flush(), "malformed request is consumed without sender fault")) return report.Finish();
      Command failed;
      if (!report.Check(driver.Next(&failed, BR_FAILED_REPLY), "malformed request produces real failed reply")) return report.Finish();
    }
  }
  const bool wire = !strncmp(scenario, "wire-", 5);
  const bool oneway = !strcmp(scenario, "oneway") || !strcmp(scenario, "wire-queue");
  Payload payload(!strcmp(scenario, "smallmap"));
  int sent_fd = -1;
  if (wire || !strcmp(scenario, "adoptfail")) {
    const std::string path = prefix + ".descriptor";
    sent_fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    static const char identity[] = "sg-owned-file";
    if (!report.Check(sent_fd >= 0 && write(sent_fd, identity, sizeof(identity)) == static_cast<ssize_t>(sizeof(identity)) &&
        lseek(sent_fd, 0, SEEK_SET) == 0, "prepare real descriptor transfer")) return report.Finish();
    payload.AddFd(sent_fd);
  }
  payload.Queue(&driver, BC_TRANSACTION_SG, 1, oneway ? TF_ONE_WAY : 0);
  if (oneway) payload.Queue(&driver, BC_TRANSACTION_SG, 2, TF_ONE_WAY);
  if (!report.Check(driver.Flush(), "SG snapshot queued before receiver reads")) return report.Finish();
  payload.DestroyOriginalBytes();
  if (!report.Check(Mark(prefix + ".queued"), "sender mutation/free completed")) return report.Finish();
  if (sent_fd >= 0) close(sent_fd);
  if (oneway) return report.Finish(); // process really exits before receiver is released
  Command response;
  const bool rejected = wire || !strcmp(scenario, "smallmap") || !strcmp(scenario, "badread") ||
      !strcmp(scenario, "prefixfault") || !strcmp(scenario, "adoptfail");
  const uint32_t expected = (!strcmp(scenario, "wire-finishloss") || !strcmp(scenario, "wire-death-afterfinish"))
      ? BR_DEAD_REPLY : rejected ? BR_FAILED_REPLY : BR_REPLY;
  if (!report.Check(driver.Next(&response, expected),
      rejected ? "receiver failure unblocks real synchronous sender" : "SG reply reaches independent sender")) return report.Finish();
  if (!rejected) {
    Verify(&driver, response.transaction, &report);
    driver.Free(response.transaction);
    report.Check(driver.Flush(), "reply allocation freed through real BC_FREE_BUFFER");
  }
  return report.Finish();
}

inline int Server(const char* scenario, const std::string& prefix) {
  Report report{"server"};
  Driver driver;
  const bool small = !strcmp(scenario, "smallmap");
  if (!report.Check(driver.Open(small ? 4096 : 1 << 20), "open independent receiver mapping")) return report.Finish();
  flat_binder_object manager{};
  manager.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS;
  if (!report.Check(ioctl(driver.fd, BINDER_SET_CONTEXT_MGR_EXT, &manager) == 0, "become actual context manager")) return report.Finish();
  driver.Code(BC_ENTER_LOOPER);
  if (!report.Check(driver.Flush() && Mark(prefix + ".ready"), "receiver ready without consuming queued transaction")) return report.Finish();
  if (!report.Check(WaitFile(prefix + ".release"), "parent releases actual delivery")) return report.Finish();
  const bool adopt_failure = !strcmp(scenario, "adoptfail");
  if (small || !strcmp(scenario, "badread") || !strcmp(scenario, "prefixfault") || adopt_failure) {
    const int expected = small ? ENOMEM : adopt_failure ? EMFILE : EFAULT;
    int actual = EAGAIN;
    uint64_t consumed = 0;
    const bool partial = !strcmp(scenario, "prefixfault");
    void* guarded = MAP_FAILED;
    void* destination = nullptr;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    if (partial) {
      guarded = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (!report.Check(guarded != MAP_FAILED && mprotect(static_cast<uint8_t*>(guarded) + page, page, PROT_NONE) == 0,
          "real protected page follows writable BR_NOOP prefix")) return report.Finish();
      destination = static_cast<uint8_t*>(guarded) + page - sizeof(uint32_t);
    }
    std::vector<int> filled;
    if (adopt_failure) {
      for (unsigned i = 0; i < 32768; ++i) {
        int descriptor = open("/dev/null", O_RDONLY);
        if (descriptor < 0) break;
        filled.push_back(descriptor);
      }
      if (!report.Check(!filled.empty() && errno == EMFILE, "real receiver descriptor table reaches EMFILE")) return report.Finish();
    }
    for (unsigned i = 0; i < 1500 && (actual == EAGAIN || actual == 0); ++i) {
      actual = driver.Talk(nullptr, true, !small && !partial && !adopt_failure, &consumed, destination);
      if (actual != expected && actual != EAGAIN && actual != 0) break;
      if (actual != expected) usleep(20000);
    }
    report.Check(actual == expected, small ? "real mapped capacity exhaustion returns ENOMEM" :
        adopt_failure ? "real FD adoption failure returns EMFILE" : "real invalid read address returns EFAULT");
    report.Check(partial ? consumed == sizeof(uint32_t) : consumed <= sizeof(uint32_t),
        partial ? "published BR_NOOP prefix retained after transaction user-copy failure" : "unpublished transaction excluded from read_consumed");
    if (guarded != MAP_FAILED) munmap(guarded, 2 * page);
    for (int descriptor : filled) close(descriptor);
    return report.Finish();
  }
  const unsigned count = !strcmp(scenario, "oneway") ? 2 : 1;
  for (unsigned i = 0; i < count; ++i) {
    Command incoming;
    if (!report.Check(driver.Next(&incoming, BR_TRANSACTION), "queued SG transaction delivered")) return report.Finish();
    report.Check(incoming.transaction.sender_pid != getpid(), "sender and receiver are separate processes");
    report.Check(incoming.transaction.code == i + 1, "one-way queue preserves order and progresses after free");
    if (!Verify(&driver, incoming.transaction, &report)) return report.Finish();
    driver.Free(incoming.transaction);
    if (count == 1) {
      Payload reply;
      reply.Queue(&driver, BC_REPLY_SG, 0);
      if (!report.Check(driver.Flush(), "receiver frees request and snapshots SG reply")) return report.Finish();
      reply.DestroyOriginalBytes();
    } else if (!report.Check(driver.Flush(), "one-way buffer free releases next queue owner")) return report.Finish();
  }
  return report.Finish();
}

inline int Main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 4) { fprintf(stderr, "usage: SGActor server|client scenario marker-prefix\n"); return 2; }
  if (!strcmp(argv[1], "server")) return Server(argv[2], argv[3]);
  if (!strcmp(argv[1], "client")) return Client(argv[2], argv[3]);
  return 2;
}

}  // namespace binder_sg_fixture
