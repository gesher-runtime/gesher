// binderd: the binder driver as a Windows process.
//
// Android's binder is a kernel driver: each process opens /dev/binder, maps a
// receive buffer, and talks to the driver with BINDER_WRITE_READ ioctls whose
// command streams carry transactions, replies, reference counts and death
// notifications. Windows has no such driver, so gesher-base's /dev/binder
// (compat/posix/src/binder.cc) forwards each ioctl over a named pipe to this
// broker, which keeps what the kernel keeps (processes, threads, nodes, refs,
// transactions and their buffers) and follows drivers/android/binder.c's rules
// for them: which thread gets a transaction (the caller's thread for nested
// calls, otherwise a waiting looper), when a node's owner hears BR_INCREFS,
// BR_ACQUIRE, BR_RELEASE and BR_DECREFS, how a reply finds its caller, oneway
// ordering per node, BR_SPAWN_LOOPER, and BR_DEAD_BINDER when a process dies.
// The wire format is in binder_wire.h; the design in
// docs/decisions/0015-services-binder-ipc.md.
//
//   binderd [device]      (default: $GESHER_BINDER, else "default")
//
// serves \\.\pipe\gesher-binder-<device> until it is killed. It exits with
// code 2 if another binderd already serves the device. GESHER_BINDER_TRACE=1
// traces every command on stderr.
//
// File descriptors (BINDER_TYPE_FD) travel as Windows handles: the sender's
// /dev/binder puts the fd's handle in the object, binderd duplicates it into
// the receiving process as broker-owned staging. The receiver duplicates a
// distinct owned copy for its fd; finish/abort closes the staging handle.
//
// Not done: fd arrays (BINDER_TYPE_FDA), security contexts
// (BR_TRANSACTION_SEC_CTX), freezing, priority inheritance, polling; such
// transactions fail with BR_FAILED_REPLY.
#include <windows.h>
#include <bcrypt.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <linux/android/binder.h>

#include "../posix/src/binder_wire.h"
#include "../posix/include/gesher/windows_process_identity.h"
#include "../posix/include/gesher/audio_device_state.h"
#include "../framework/app_container.h"
#include "../posix/src/binder_file_descriptions.h"
#include "../posix/src/binder_sg.h"
#include "launch_channel.h"
#include "../posix/include/gesher/capture_policy.h"
#ifdef GESHER_AUDIO_RENDER_BROKER
#include "brokered_audio.h"
#include "shared_state.h"
#endif
#ifdef GESHER_CAMERA_BROKER_HOST
#include "brokered_camera.h"
#endif
#ifdef GESHER_RECORD_LOCK_BROKER
#include "../posix/src/record_lock_descriptors.h"
#endif

namespace {

namespace wire = gesher::binder_wire;
namespace launch = gesher::binder_launch;
namespace peer = gesher::binder_peer;

bool g_trace = false;
bool g_record_only = false;
gesher::process_identity::Sid g_record_container;

void Log(const char* format, ...) {
  va_list args;
  va_start(args, format);
  fprintf(stderr, "binderd: ");
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
  va_end(args);
}

#define TRACE(...)                  \
  do {                              \
    if (g_trace) Log(__VA_ARGS__);  \
  } while (0)

// --- the driver's objects ----------------------------------------------------

enum LooperState : uint32_t {
  kLooperRegistered = 0x01,
  kLooperEntered = 0x02,
  kLooperExited = 0x04,
  kLooperInvalid = 0x08,
  kLooperWaiting = 0x10,
};

enum class WorkType {
  kTransaction,
  kTransactionComplete,
  kReturnError,
  kNode,
  kDeadBinder,
  kDeadBinderAndClear,
  kClearDeathNotification,
};

struct Work;
using WorkList = std::list<Work*>;

struct Work {
  WorkType type;
  WorkList* list = nullptr;  // the list it is queued on, if any
  explicit Work(WorkType t) : type(t) {}
  virtual ~Work() = default;
};

struct ErrorWork : Work {
  uint32_t cmd = BR_OK;
  ErrorWork() : Work(WorkType::kReturnError) {}
};

struct Proc;
struct Thread;
struct Node;
struct Ref;
struct Transaction;
struct Buffer;

struct Death : Work {
  uint64_t cookie = 0;
  Death() : Work(WorkType::kDeadBinder) {}
};

struct Node : Work {
  int debug_id = 0;
  Proc* proc = nullptr;  // null once the owner died
  uint64_t ptr = 0;
  uint64_t cookie = 0;
  uint32_t flags = 0;
  int internal_strong_refs = 0;
  int local_strong_refs = 0;
  int local_weak_refs = 0;
  int tmp_refs = 0;
  bool has_strong_ref = false;
  bool pending_strong_ref = false;
  bool has_weak_ref = false;
  bool pending_weak_ref = false;
  std::set<Ref*> refs;
  bool has_async_transaction = false;
  WorkList async_todo;
  Node() : Work(WorkType::kNode) {}
};

struct Ref {
  Proc* proc = nullptr;
  Node* node = nullptr;
  uint32_t desc = 0;
  int strong = 0;
  int weak = 0;
  Death* death = nullptr;
};

struct Buffer {
  uint64_t id = 0;
  Proc* proc = nullptr;  // the receiver
  Transaction* transaction = nullptr;
  Node* target_node = nullptr;
  bool async_transaction = false;
  bool allow_user_free = false;  // delivered to the receiver
  std::vector<uint8_t> data;
  std::vector<uint64_t> offsets;
  std::vector<uint8_t> extra;
  std::vector<wire::FileDescription> descriptions;
};

struct Transaction : Work {
  int debug_id = 0;
  std::shared_ptr<Thread> from;
  int from_pid = 0;
  Transaction* from_parent = nullptr;
  std::shared_ptr<Thread> to_thread;
  Transaction* to_parent = nullptr;
  bool need_reply = false;
  uint32_t code = 0;
  uint32_t flags = 0;
  uint32_t sender_euid = 0;
  Buffer* buffer = nullptr;
  Transaction() : Work(WorkType::kTransaction) {}
};

struct Thread : std::enable_shared_from_this<Thread> {
  std::shared_ptr<Proc> proc;
  int tid = 0;
  uint32_t looper = 0;
  bool looper_need_return = true;
  bool is_dead = false;
  bool waiting = false;  // on proc->waiting_threads
  bool process_todo = false;
  Transaction* transaction_stack = nullptr;
  uint64_t pending_buffer = 0;
  bool pending_buffer_claimed = false;  // validation state, never FD ownership
  WorkList todo;
  ErrorWork return_error;
  ErrorWork reply_error;
  std::condition_variable cv;
};

struct Proc {
  uint32_t id = 0;
  uint64_t key = 0;
  int pid = 0;
  uint32_t uid = 0;
  gesher::binder_peer::Registry::Lease identity;
  bool is_dead = false;
  std::map<uint64_t, Node*> nodes;
  std::map<uint32_t, Ref*> refs_by_desc;
  std::map<Node*, Ref*> refs_by_node;
  std::list<Thread*> threads;
  WorkList todo;
  std::list<Thread*> waiting_threads;
  WorkList delivered_death;
  std::map<uint64_t, Buffer*> buffers;
  uint32_t max_threads = 0;
  uint32_t requested_threads = 0;
  uint32_t requested_threads_started = 0;
};

// Every object is guarded by one lock, as the driver's are by its locks; the
// broker's threads only block on their thread's condition variable, which
// releases it. Connection scopes own threads and processes; transactions pin
// their caller/receiver threads, and each thread pins its process. Registry
// lists remain non-owning and are detached under this lock before destruction.
std::mutex g_lock;
std::map<uint32_t, std::shared_ptr<Proc>> g_procs;
Node* g_context_mgr_node = nullptr;
uint32_t g_next_proc_id = 1;
// GESHER_SANDBOX=appcontainer (ADR 0090): set once in main, before the first client.
bool g_sandbox = false;
std::unique_ptr<gesher::binder_peer::Registry> g_peer_registry;
uint64_t g_next_buffer_id = 1;
int g_next_debug_id = 1;

// --- work lists --------------------------------------------------------------

void Enqueue(WorkList* list, Work* w) {
  w->list = list;
  list->push_back(w);
}

void Dequeue(Work* w) {
  if (w->list == nullptr) return;
  auto it = std::find(w->list->begin(), w->list->end(), w);
  if (it != w->list->end()) w->list->erase(it);
  w->list = nullptr;
}

Work* DequeueHead(WorkList* list) {
  if (list->empty()) return nullptr;
  Work* w = list->front();
  list->pop_front();
  w->list = nullptr;
  return w;
}

void EnqueueThreadWork(Thread* thread, Work* w) {
  Enqueue(&thread->todo, w);
  thread->process_todo = true;
}

// Work the thread must not return to user space for on its own (the
// TRANSACTION_COMPLETE of a call, which waits for its reply).
void EnqueueDeferredThreadWork(Thread* thread, Work* w) { Enqueue(&thread->todo, w); }

void WakeThread(Thread* thread) { thread->cv.notify_all(); }

Thread* SelectThread(Proc* proc) {
  if (proc->waiting_threads.empty()) return nullptr;
  Thread* thread = proc->waiting_threads.front();
  proc->waiting_threads.pop_front();
  thread->waiting = false;
  return thread;
}

void WakeProc(Proc* proc) {
  if (Thread* thread = SelectThread(proc)) WakeThread(thread);
}

bool AvailableForProcWork(Thread* thread) {
  return thread->transaction_stack == nullptr && thread->todo.empty() &&
         (thread->looper & (kLooperEntered | kLooperRegistered)) != 0;
}

bool HasWork(Thread* thread, bool do_proc_work) {
  return thread->process_todo || thread->looper_need_return ||
         (do_proc_work && !thread->proc->todo.empty());
}

// --- nodes and refs (binder_inc_node, binder_dec_node, binder_inc_ref ...) ----

Node* NewNode(Proc* proc, uint64_t ptr, uint64_t cookie, uint32_t flags) {
  Node* node = new Node;
  node->debug_id = g_next_debug_id++;
  node->proc = proc;
  node->ptr = ptr;
  node->cookie = cookie;
  node->flags = flags;
  proc->nodes[ptr] = node;
  TRACE("proc %u: new node %d ptr %llx", proc->id, node->debug_id, (unsigned long long)ptr);
  return node;
}

Node* GetNode(Proc* proc, uint64_t ptr) {
  auto it = proc->nodes.find(ptr);
  return it == proc->nodes.end() ? nullptr : it->second;
}

void FreeNode(Node* node) {
  TRACE("free node %d", node->debug_id);
  Dequeue(node);
  if (node->proc != nullptr) node->proc->nodes.erase(node->ptr);
  if (g_context_mgr_node == node) g_context_mgr_node = nullptr;
  delete node;
}

// target_list: where the owner hears of a new strong or weak reference, if it
// must be told now (the sending thread's own list when it passes its own
// object). Returns false on a reference count error.
bool IncNode(Node* node, bool strong, bool internal, Thread* target_thread) {
  if (strong) {
    if (internal) {
      if (target_thread == nullptr && node->internal_strong_refs == 0 &&
          !(node == g_context_mgr_node && node->has_strong_ref)) {
        Log("invalid inc strong node %d", node->debug_id);
        return false;
      }
      node->internal_strong_refs++;
    } else {
      node->local_strong_refs++;
    }
    if (!node->has_strong_ref && target_thread != nullptr) {
      Dequeue(node);
      EnqueueDeferredThreadWork(target_thread, node);
    }
  } else {
    if (!internal) node->local_weak_refs++;
    if (!node->has_weak_ref && node->list == nullptr) {
      if (target_thread == nullptr) {
        Log("invalid inc weak node %d", node->debug_id);
        return false;
      }
      EnqueueDeferredThreadWork(target_thread, node);
    }
  }
  return true;
}

// Returns true if the node was freed.
bool DecNode(Node* node, bool strong, bool internal) {
  Proc* proc = node->proc;
  if (strong) {
    if (internal) {
      node->internal_strong_refs--;
    } else {
      node->local_strong_refs--;
    }
    if (node->local_strong_refs > 0 || node->internal_strong_refs > 0) return false;
  } else {
    if (!internal) node->local_weak_refs--;
    if (node->local_weak_refs > 0 || node->tmp_refs > 0 || !node->refs.empty()) return false;
  }
  if (proc != nullptr && (node->has_strong_ref || node->has_weak_ref)) {
    if (node->list == nullptr) {
      Enqueue(&proc->todo, node);
      WakeProc(proc);
    }
  } else if (node->refs.empty() && node->local_strong_refs == 0 && node->local_weak_refs == 0 &&
             node->tmp_refs == 0) {
    FreeNode(node);
    return true;
  }
  return false;
}

// binder_dec_node_tmpref: DecNode(weak, internal) releases no count, only
// frees the node or tells its owner if nothing holds it any more.
void NodeTmpRefDec(Node* node) {
  node->tmp_refs--;
  DecNode(node, false, true);
}

Ref* GetRef(Proc* proc, uint32_t desc, bool need_strong_ref) {
  auto it = proc->refs_by_desc.find(desc);
  if (it == proc->refs_by_desc.end()) return nullptr;
  if (need_strong_ref && it->second->strong == 0) {
    Log("proc %u: tried to use weak ref %u as strong ref", proc->id, desc);
    return nullptr;
  }
  return it->second;
}

Ref* GetRefForNode(Proc* proc, Node* node) {
  auto it = proc->refs_by_node.find(node);
  if (it != proc->refs_by_node.end()) return it->second;
  Ref* ref = new Ref;
  ref->proc = proc;
  ref->node = node;
  uint32_t desc = node == g_context_mgr_node ? 0 : 1;
  for (auto& [d, r] : proc->refs_by_desc) {
    if (d > desc) break;
    desc = d + 1;
  }
  ref->desc = desc;
  proc->refs_by_desc[desc] = ref;
  proc->refs_by_node[node] = ref;
  node->refs.insert(ref);
  TRACE("proc %u: new ref %u for node %d", proc->id, desc, node->debug_id);
  return ref;
}

bool IncRef(Ref* ref, bool strong, Thread* target_thread) {
  if (strong) {
    if (ref->strong == 0 && !IncNode(ref->node, true, true, target_thread)) return false;
    ref->strong++;
  } else {
    if (ref->weak == 0 && !IncNode(ref->node, false, true, target_thread)) return false;
    ref->weak++;
  }
  return true;
}

void CleanupRef(Ref* ref) {
  Proc* proc = ref->proc;
  proc->refs_by_desc.erase(ref->desc);
  proc->refs_by_node.erase(ref->node);
  Node* node = ref->node;
  if (ref->strong > 0) DecNode(node, true, true);
  node->refs.erase(ref);
  DecNode(node, false, true);
  if (ref->death != nullptr) {
    Dequeue(ref->death);
    delete ref->death;
    ref->death = nullptr;
  }
  delete ref;
}

// Returns true if the ref was freed.
bool DecRef(Ref* ref, bool strong) {
  if (strong) {
    if (ref->strong == 0) {
      Log("proc %u: invalid dec strong ref %u", ref->proc->id, ref->desc);
      return false;
    }
    ref->strong--;
    if (ref->strong == 0) DecNode(ref->node, true, true);
  } else {
    if (ref->weak == 0) {
      Log("proc %u: invalid dec weak ref %u", ref->proc->id, ref->desc);
      return false;
    }
    ref->weak--;
  }
  if (ref->strong == 0 && ref->weak == 0) {
    CleanupRef(ref);
    return true;
  }
  return false;
}

Ref* IncRefForNode(Proc* proc, Node* node, bool strong, Thread* target_thread) {
  Ref* ref = GetRefForNode(proc, node);
  if (!IncRef(ref, strong, target_thread)) return nullptr;
  return ref;
}

bool UpdateRefForHandle(Proc* proc, uint32_t desc, bool increment, bool strong) {
  Ref* ref = GetRef(proc, desc, strong);
  if (ref == nullptr) return false;
  if (increment) return IncRef(ref, strong, nullptr);
  DecRef(ref, strong);
  return true;
}

// --- transactions --------------------------------------------------------------

constexpr size_t kFlatObjectSize = sizeof(flat_binder_object);

// A handle of another process, for a BINDER_TYPE_FD object: duplicated from
// the sender into the receiver (0 if that fails), or closed in its process.
HANDLE ProcessForTransfer(const Proc* proc, peer::OwnedHandle* legacy_owner) {
  if (proc->identity) return proc->identity->process.get();
  if (g_peer_registry) return nullptr;
  legacy_owner->reset(OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(proc->pid)));
  return legacy_owner->get();
}

uint64_t DuplicateRemoteHandle(const Proc* from_proc, const Proc* to_proc, uint64_t handle) {
  if (handle == 0) return 0;
  if (g_peer_registry && (!g_peer_registry->Live(from_proc->identity) ||
                          !g_peer_registry->Live(to_proc->identity))) return 0;
  peer::OwnedHandle legacy_from;
  peer::OwnedHandle legacy_to;
  HANDLE from = ProcessForTransfer(from_proc, &legacy_from);
  HANDLE to = ProcessForTransfer(to_proc, &legacy_to);
  HANDLE dup = nullptr;
  bool ok = from != nullptr && to != nullptr &&
            DuplicateHandle(from, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)), to,
                            &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
  if (!ok) Log("DuplicateHandle from pid %d to pid %d failed: %lu",
               from_proc->pid, to_proc->pid, GetLastError());
  return ok ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dup)) : 0;
}

void CloseRemoteHandle(const Proc* proc, uint64_t handle) {
  if (handle == 0) return;
  peer::OwnedHandle legacy;
  HANDLE p = ProcessForTransfer(proc, &legacy);
  if (p == nullptr) return;
  DuplicateHandle(p, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)), nullptr, nullptr,
                  0, FALSE, DUPLICATE_CLOSE_SOURCE);
}

wire::FileDescription* DescriptionAt(Buffer* buffer, uint64_t offset) {
  // The manifest is ordered like the validated, strictly increasing offsets.
  auto found = std::lower_bound(buffer->descriptions.begin(), buffer->descriptions.end(), offset,
      [](const wire::FileDescription& entry, uint64_t value) { return entry.object_offset < value; });
  return found != buffer->descriptions.end() && found->object_offset == offset ? &*found : nullptr;
}

void CloseStagingDescription(Buffer* buffer, uint64_t offset) {
  if (auto* description = DescriptionAt(buffer, offset)) {
    CloseRemoteHandle(buffer->proc, description->shared_handle);
    description->shared_handle = 0;
  }
}

// binder_transaction_buffer_release: drops the references a buffer's objects
// hold, in the receiver's terms.
void ReleaseBufferObjects(Buffer* buffer) {
  Proc* proc = buffer->proc;
  for (uint64_t off : buffer->offsets) {
    if (off + kFlatObjectSize > buffer->data.size()) continue;
    flat_binder_object fp;
    memcpy(&fp, buffer->data.data() + off, sizeof(fp));
    switch (fp.hdr.type) {
      case BINDER_TYPE_BINDER:
      case BINDER_TYPE_WEAK_BINDER: {
        if (proc->is_dead) break;
        Node* node = GetNode(proc, fp.binder);
        if (node != nullptr) DecNode(node, fp.hdr.type == BINDER_TYPE_BINDER, false);
        break;
      }
      case BINDER_TYPE_HANDLE:
      case BINDER_TYPE_WEAK_HANDLE:
        if (proc->is_dead) break;
        UpdateRefForHandle(proc, fp.handle, false, fp.hdr.type == BINDER_TYPE_HANDLE);
        break;
      case BINDER_TYPE_FD:
        // Staging handles always belong to the broker. Successful delivery
        // already closes/zeros them; abort/release closes any still present.
        CloseRemoteHandle(proc, fp.binder);
        CloseStagingDescription(buffer, off);
        break;
      default:
        break;
    }
  }
}

void FreeBuffer(Buffer* buffer, bool release_objects) {
  if (buffer->transaction != nullptr) buffer->transaction->buffer = nullptr;
  if (buffer->target_node != nullptr) {
    Node* node = buffer->target_node;
    buffer->target_node = nullptr;
    NodeTmpRefDec(node);
  }
  if (release_objects) ReleaseBufferObjects(buffer);
  buffer->proc->buffers.erase(buffer->id);
  delete buffer;
}

void FreeTransaction(Transaction* t) {
  if (t->buffer != nullptr) {
    t->buffer->transaction = nullptr;
    // A buffer its receiver never saw goes with the transaction.
    if (!t->buffer->allow_user_free) FreeBuffer(t->buffer, true);
  }
  delete t;
}

void PopTransaction(Thread* target_thread, Transaction* t) {
  if (target_thread->transaction_stack == t) {
    target_thread->transaction_stack = t->from_parent;
  } else {
    Log("pop of a transaction not on top of its caller's stack");
  }
  t->from = nullptr;
}

// binder_send_failed_reply: tells the caller (or its caller, if it is gone)
// that its call failed.
void SendFailedReply(Transaction* t, uint32_t error_code) {
  for (;;) {
    Thread* target_thread = t->from.get();
    if (target_thread != nullptr && !target_thread->is_dead) {
      PopTransaction(target_thread, t);
      if (target_thread->reply_error.cmd == BR_OK) {
        target_thread->reply_error.cmd = error_code;
        EnqueueThreadWork(target_thread, &target_thread->reply_error);
        WakeThread(target_thread);
      }
      FreeTransaction(t);
      return;
    }
    Transaction* next = t->from_parent;
    FreeTransaction(t);
    if (next == nullptr) return;
    t = next;
  }
}

void ReleaseWork(WorkList* list) {
  while (Work* w = DequeueHead(list)) {
    switch (w->type) {
      case WorkType::kTransaction: {
        Transaction* t = static_cast<Transaction*>(w);
        if (t->buffer != nullptr && t->buffer->target_node != nullptr &&
            (t->flags & TF_ONE_WAY) == 0) {
          SendFailedReply(t, BR_DEAD_REPLY);
        } else {
          FreeTransaction(t);
        }
        break;
      }
      case WorkType::kTransactionComplete:
        delete w;
        break;
      case WorkType::kDeadBinderAndClear:
      case WorkType::kClearDeathNotification:
        delete w;
        break;
      case WorkType::kReturnError:
      case WorkType::kNode:
      case WorkType::kDeadBinder:
        break;
    }
  }
}

// binder_proc_transaction: queues t on a thread of proc. Returns BR_OK or
// BR_DEAD_REPLY.
uint32_t ProcTransaction(Transaction* t, Proc* proc, Thread* thread) {
  Node* node = t->buffer->target_node;
  bool oneway = (t->flags & TF_ONE_WAY) != 0;
  bool pending_async = false;
  if (oneway) {
    if (node->has_async_transaction) {
      pending_async = true;
    } else {
      node->has_async_transaction = true;
    }
  }
  if (proc->is_dead || (thread != nullptr && thread->is_dead)) return BR_DEAD_REPLY;
  if (thread == nullptr && !pending_async) thread = SelectThread(proc);
  if (thread != nullptr) {
    EnqueueThreadWork(thread, t);
    WakeThread(thread);
  } else if (!pending_async) {
    Enqueue(&proc->todo, t);
  } else {
    Enqueue(&node->async_todo, t);
  }
  return BR_OK;
}

struct TransactionData {
  binder_transaction_data tr;
  std::vector<uint8_t> data;
  std::vector<uint64_t> offsets;
  uint64_t buffers_size = 0;
  int32_t copy_error = 0;
  std::vector<uint8_t> extra;
  std::vector<wire::FileDescription> descriptions;
};

// Translates the objects of a buffer from the sender's terms (proc) to the
// receiver's (target_proc). Returns BR_OK or the error to report; on error
// the buffer's offsets are cut to the objects translated, so that releasing
// the buffer drops the references they took.
uint32_t TranslateObjectsAll(Proc* proc, Thread* thread, Proc* target_proc, Buffer* buffer,
                             size_t* translated);

uint32_t TranslateObjects(Proc* proc, Thread* thread, Proc* target_proc, Buffer* buffer) {
  size_t translated = 0;
  uint32_t error = TranslateObjectsAll(proc, thread, target_proc, buffer, &translated);
  if (error != BR_OK) buffer->offsets.resize(translated);
  return error;
}

uint32_t TranslateObjectsAll(Proc* proc, Thread* thread, Proc* target_proc, Buffer* buffer,
                             size_t* translated) {
  for (uint64_t off : buffer->offsets) {
    if (off % 4 != 0 || off + sizeof(binder_object_header) > buffer->data.size()) {
      Log("proc %u: invalid object offset %llu", proc->id, (unsigned long long)off);
      return BR_FAILED_REPLY;
    }
    uint32_t type;
    memcpy(&type, buffer->data.data() + off, sizeof(type));
    if (type == BINDER_TYPE_PTR) {
      ++*translated;  // Owned extra storage is released with the same Buffer.
      continue;
    }
    if (type != BINDER_TYPE_BINDER && type != BINDER_TYPE_WEAK_BINDER &&
        type != BINDER_TYPE_HANDLE && type != BINDER_TYPE_WEAK_HANDLE &&
        type != BINDER_TYPE_FD) {
      Log("proc %u: object type %08x is not supported", proc->id, type);
      return BR_FAILED_REPLY;
    }
    if (off + kFlatObjectSize > buffer->data.size()) return BR_FAILED_REPLY;
    flat_binder_object fp;
    memcpy(&fp, buffer->data.data() + off, sizeof(fp));
    if (type == BINDER_TYPE_FD) {
      // binder_translate_fd: fp.binder is binder_fd_object's pad_binder,
      // where the sender's /dev/binder put the fd's handle.
      if (fp.binder == 0) {
        Log("proc %u: a file descriptor without a Windows handle cannot be sent", proc->id);
        return BR_FAILED_REPLY;
      }
      uint64_t dup = DuplicateRemoteHandle(proc, target_proc, fp.binder);
      if (dup == 0) return BR_FAILED_REPLY;
      auto* description = DescriptionAt(buffer, off);
      uint64_t shared = description
          ? DuplicateRemoteHandle(proc, target_proc, description->shared_handle) : 0;
      // Sockets and completed native fences are typed handles without an OFD.
      const bool typed = description && (description->kind == wire::kSocketFileDescription ||
                                         description->kind == wire::kNativeFenceFileDescription);
      if (!description || (!typed && shared == 0)) {
        CloseRemoteHandle(target_proc, dup);
        return BR_FAILED_REPLY;
      }
      description->shared_handle = shared;
      TRACE("proc %u: fd handle %llx -> %llx in proc %u", proc->id,
            (unsigned long long)fp.binder, (unsigned long long)dup, target_proc->id);
      fp.binder = dup;
    } else if (type == BINDER_TYPE_BINDER || type == BINDER_TYPE_WEAK_BINDER) {
      // binder_translate_binder
      Node* node = GetNode(proc, fp.binder);
      if (node == nullptr) node = NewNode(proc, fp.binder, fp.cookie, fp.flags);
      if (fp.cookie != node->cookie) {
        Log("proc %u: node %d cookie mismatch", proc->id, node->debug_id);
        return BR_FAILED_REPLY;
      }
      Ref* ref = IncRefForNode(target_proc, node, type == BINDER_TYPE_BINDER, thread);
      if (ref == nullptr) return BR_FAILED_REPLY;
      fp.hdr.type = type == BINDER_TYPE_BINDER ? BINDER_TYPE_HANDLE : BINDER_TYPE_WEAK_HANDLE;
      fp.binder = 0;
      fp.handle = ref->desc;
      fp.cookie = 0;
    } else {
      // binder_translate_handle
      Ref* src = GetRef(proc, fp.handle, type == BINDER_TYPE_HANDLE);
      if (src == nullptr) {
        Log("proc %u: transaction with invalid handle %u", proc->id, fp.handle);
        return BR_FAILED_REPLY;
      }
      Node* node = src->node;
      if (node->proc == target_proc) {
        fp.hdr.type = type == BINDER_TYPE_HANDLE ? BINDER_TYPE_BINDER : BINDER_TYPE_WEAK_BINDER;
        fp.binder = node->ptr;
        fp.cookie = node->cookie;
        IncNode(node, fp.hdr.type == BINDER_TYPE_BINDER, false, nullptr);
      } else {
        Ref* ref = IncRefForNode(target_proc, node, type == BINDER_TYPE_HANDLE, nullptr);
        if (ref == nullptr) return BR_FAILED_REPLY;
        fp.binder = 0;
        fp.handle = ref->desc;
        fp.cookie = 0;
      }
    }
    memcpy(buffer->data.data() + off, &fp, sizeof(fp));
    ++*translated;
  }
  return BR_OK;
}

// binder_transaction: BC_TRANSACTION and BC_REPLY.
void DoTransaction(Proc* proc, Thread* thread, TransactionData& td, bool reply) {
  binder_transaction_data& tr = td.tr;
  Transaction* in_reply_to = nullptr;
  Thread* target_thread = nullptr;
  Proc* target_proc = nullptr;
  Node* target_node = nullptr;  // holds a tmp ref until the buffer takes it

  // The error paths of binder_transaction: a failed reply still completes for
  // the replying thread, and its caller (or the caller's caller) hears the
  // error; a failed call reports the error to the calling thread.
  auto fail = [&](uint32_t error) {
    TRACE("proc %u thread %d: %s failed: %s", proc->id, thread->tid,
          reply ? "reply" : "transaction", error == BR_DEAD_REPLY ? "dead" : "failed");
    if (target_node != nullptr) NodeTmpRefDec(target_node);
    if (in_reply_to != nullptr) {
      thread->return_error.cmd = BR_TRANSACTION_COMPLETE;
      EnqueueThreadWork(thread, &thread->return_error);
      SendFailedReply(in_reply_to, error);
    } else {
      thread->return_error.cmd = error;
      EnqueueThreadWork(thread, &thread->return_error);
    }
  };

  if (reply) {
    in_reply_to = thread->transaction_stack;
    if (in_reply_to == nullptr) {
      Log("proc %u thread %d: reply with no transaction stack", proc->id, thread->tid);
      return fail(BR_FAILED_REPLY);
    }
    if (in_reply_to->to_thread.get() != thread) {
      Log("proc %u thread %d: reply to a transaction of another thread", proc->id, thread->tid);
      in_reply_to = nullptr;
      return fail(BR_FAILED_REPLY);
    }
    thread->transaction_stack = in_reply_to->to_parent;
    target_thread = in_reply_to->from.get();
    if (target_thread == nullptr || target_thread->is_dead) return fail(BR_DEAD_REPLY);
    if (target_thread->transaction_stack != in_reply_to) {
      Log("proc %u: reply out of order", proc->id);
      in_reply_to = nullptr;
      return fail(BR_FAILED_REPLY);
    }
    target_proc = target_thread->proc.get();
  } else {
    Node* node;
    if (tr.target.handle != 0) {
      Ref* ref = GetRef(proc, tr.target.handle, true);
      if (ref == nullptr) {
        Log("proc %u: transaction to invalid handle %u", proc->id, tr.target.handle);
        return fail(BR_FAILED_REPLY);
      }
      node = ref->node;
    } else {
      node = g_context_mgr_node;
      if (node == nullptr) return fail(BR_DEAD_REPLY);
    }
    // binder_get_node_refs_for_txn
    if (node->proc == nullptr) return fail(BR_DEAD_REPLY);
    target_node = node;
    target_node->tmp_refs++;
    target_proc = target_node->proc;
    if (target_proc == proc) {
      Log("proc %u: transaction to itself", proc->id);
      return fail(BR_FAILED_REPLY);
    }
    if ((tr.flags & TF_ONE_WAY) == 0 && thread->transaction_stack != nullptr) {
      Transaction* tmp = thread->transaction_stack;
      if (tmp->to_thread.get() != thread) {
        Log("proc %u thread %d: new transaction not at the top of its stack", proc->id,
            thread->tid);
        return fail(BR_FAILED_REPLY);
      }
      // A nested call goes to the thread of the target process that waits
      // for this thread's (or an outer) call.
      while (tmp != nullptr) {
        Thread* from = tmp->from.get();
        if (from != nullptr && from->proc.get() == target_proc) {
          target_thread = from;
          break;
        }
        tmp = tmp->from_parent;
      }
    }
  }
  gesher::binder_sg::Layout sg_layout;
  if (td.copy_error != 0 ||
      !gesher::binder_sg::WireLayout(td.data, td.offsets, td.extra.size(), &sg_layout) ||
      !wire::ValidFileDescriptions(td.data, td.offsets, td.descriptions)) {
    Log("proc %u: invalid transaction user copy or scatter-gather layout", proc->id);
    return fail(BR_FAILED_REPLY);
  }

  Transaction* t = new Transaction;
  t->debug_id = g_next_debug_id++;
  t->from = (!reply && (tr.flags & TF_ONE_WAY) == 0) ? thread->shared_from_this() : nullptr;
  t->from_pid = proc->pid;
  t->sender_euid = proc->uid;
  t->to_thread = target_thread != nullptr ? target_thread->shared_from_this() : nullptr;
  t->code = tr.code;
  t->flags = tr.flags;

  Buffer* buffer = new Buffer;
  buffer->id = g_next_buffer_id++;
  buffer->proc = target_proc;
  buffer->transaction = t;
  buffer->target_node = target_node;  // takes over the tmp ref
  target_node = nullptr;
  buffer->async_transaction = (tr.flags & TF_ONE_WAY) != 0;
  buffer->data = std::move(td.data);
  buffer->offsets = std::move(td.offsets);
  buffer->extra = std::move(td.extra);
  buffer->descriptions = std::move(td.descriptions);
  target_proc->buffers[buffer->id] = buffer;
  t->buffer = buffer;

  TRACE("proc %u thread %d: %s %d to proc %u%s code %u size %zu objects %zu", proc->id,
        thread->tid, reply ? "reply" : "transaction", t->debug_id, target_proc->id,
        (tr.flags & TF_ONE_WAY) != 0 ? " (oneway)" : "", tr.code, buffer->data.size(),
        buffer->offsets.size());

  uint32_t error = TranslateObjects(proc, thread, target_proc, buffer);
  if (error != BR_OK) {
    FreeTransaction(t);  // and its buffer, with what was translated
    return fail(error);
  }

  Work* tcomplete = new Work(WorkType::kTransactionComplete);
  uint32_t r = BR_OK;
  if (reply) {
    EnqueueThreadWork(thread, tcomplete);
    if (target_thread->is_dead) {
      r = BR_DEAD_REPLY;
    } else {
      PopTransaction(target_thread, in_reply_to);
      EnqueueThreadWork(target_thread, t);
      WakeThread(target_thread);
      FreeTransaction(in_reply_to);
      return;
    }
  } else if ((t->flags & TF_ONE_WAY) == 0) {
    EnqueueDeferredThreadWork(thread, tcomplete);
    t->need_reply = true;
    t->from_parent = thread->transaction_stack;
    thread->transaction_stack = t;
    r = ProcTransaction(t, target_proc, target_thread);
    if (r == BR_OK) return;
    thread->transaction_stack = t->from_parent;  // binder_pop_transaction_ilocked
  } else {
    EnqueueThreadWork(thread, tcomplete);
    r = ProcTransaction(t, target_proc, nullptr);
    if (r == BR_OK) return;
  }
  // err_dead_proc_or_thread
  Dequeue(tcomplete);
  delete tcomplete;
  FreeTransaction(t);
  fail(r);
}

// BC_FREE_BUFFER
void DoFreeBuffer(Proc* proc, uint64_t id) {
  auto it = proc->buffers.find(id);
  if (it == proc->buffers.end()) {
    Log("proc %u: BC_FREE_BUFFER of unknown buffer %llu", proc->id, (unsigned long long)id);
    return;
  }
  Buffer* buffer = it->second;
  if (!buffer->allow_user_free) {
    Log("proc %u: BC_FREE_BUFFER of a buffer not delivered", proc->id);
    return;
  }
  if (buffer->transaction != nullptr) {
    buffer->transaction->buffer = nullptr;
    buffer->transaction = nullptr;
  }
  if (buffer->async_transaction && buffer->target_node != nullptr) {
    Node* node = buffer->target_node;
    Work* w = DequeueHead(&node->async_todo);
    if (w == nullptr) {
      node->has_async_transaction = false;
    } else {
      Enqueue(&proc->todo, w);
      WakeProc(proc);
    }
  }
  FreeBuffer(buffer, true);
}

// Close only broker-owned staging handles. The receiver's adopted descriptors
// refer to distinct duplicates, so finish/abort/ACK loss cannot close a handle
// whose value the receiver has already freed and reused.
void CloseStagingFds(Buffer* buffer) {
  for (uint64_t offset : buffer->offsets) {
    if (!gesher::binder_sg::Range(offset, sizeof(binder_fd_object), buffer->data.size())) continue;
    binder_fd_object object;
    memcpy(&object, buffer->data.data() + offset, sizeof(object));
    if (object.hdr.type != BINDER_TYPE_FD) continue;
    const uint64_t staging = object.pad_binder;
    object.pad_binder = 0;
    memcpy(buffer->data.data() + offset, &object, sizeof(object));
    CloseRemoteHandle(buffer->proc, staging);
    CloseStagingDescription(buffer, offset);
  }
}

// Roll back a delivery the receiver could not publish. A synchronous caller
// gets a genuine failed reply; oneway queue ownership is released normally.
int FinishBuffer(Thread* thread, uint64_t id, bool accepted) {
  Proc* proc = thread->proc.get();
  auto found = proc->buffers.find(id);
  if (id == 0 || thread->pending_buffer != id || found == proc->buffers.end()) return -EINVAL;
  Buffer* buffer = found->second;
  Transaction* transaction = buffer->transaction;
  if (transaction != nullptr && (transaction->to_thread.get() != thread || thread->transaction_stack != transaction)) {
    return -EINVAL;
  }
  if (accepted && !thread->pending_buffer_claimed) return -EINVAL;
  thread->pending_buffer = 0;
  thread->pending_buffer_claimed = false;
  CloseStagingFds(buffer);
  if (accepted) return 0;
  if (transaction != nullptr) {
    thread->transaction_stack = transaction->to_parent;
    transaction->to_thread = nullptr;
    transaction->buffer = nullptr;
    buffer->transaction = nullptr;
    SendFailedReply(transaction, BR_FAILED_REPLY);
  }
  DoFreeBuffer(proc, id);
  return 0;
}

// --- thread and process release ---------------------------------------------------

// binder_thread_release
void ThreadRelease(Thread* thread) {
  if (thread->is_dead) return;
  if (thread->pending_buffer != 0) FinishBuffer(thread, thread->pending_buffer, false);
  Proc* proc = thread->proc.get();
  proc->threads.remove(thread);
  if (thread->waiting) {
    proc->waiting_threads.remove(thread);
    thread->waiting = false;
  }
  Transaction* t = thread->transaction_stack;
  Transaction* send_reply = nullptr;
  if (t != nullptr && t->to_thread.get() == thread) send_reply = t;
  thread->is_dead = true;
  while (t != nullptr) {
    if (t->to_thread.get() == thread) {
      t->to_thread = nullptr;
      t = t->to_parent;
    } else if (t->from.get() == thread) {
      t->from = nullptr;
      t = t->from_parent;
    } else {
      break;
    }
  }
  if (send_reply != nullptr) SendFailedReply(send_reply, BR_DEAD_REPLY);
  ReleaseWork(&thread->todo);
  WakeThread(thread);
}

// binder_node_release, for a node whose owner died.
void NodeRelease(Node* node) {
  Dequeue(node);
  Proc* proc = node->proc;
  if (proc != nullptr) proc->nodes.erase(node->ptr);
  if (node->refs.empty() && node->tmp_refs == 0) {
    node->proc = nullptr;
    if (g_context_mgr_node == node) g_context_mgr_node = nullptr;
    delete node;
    return;
  }
  node->proc = nullptr;
  node->local_strong_refs = 0;
  node->local_weak_refs = 0;
  for (Ref* ref : node->refs) {
    if (ref->death == nullptr) continue;
    if (ref->death->list == nullptr) {
      ref->death->type = WorkType::kDeadBinder;
      Enqueue(&ref->proc->todo, ref->death);
      WakeProc(ref->proc);
    }
  }
}

// binder_deferred_release
void ProcRelease(Proc* proc) {
  std::unique_lock<std::mutex> lock(g_lock);
  if (proc->is_dead) return;
  TRACE("proc %u (pid %d) released", proc->id, proc->pid);
  proc->is_dead = true;
  if (g_context_mgr_node != nullptr && g_context_mgr_node->proc == proc) {
    g_context_mgr_node = nullptr;
  }
  std::list<Thread*> threads = proc->threads;
  for (Thread* thread : threads) ThreadRelease(thread);
  std::vector<Node*> nodes;
  for (auto& [ptr, node] : proc->nodes) nodes.push_back(node);
  for (Node* node : nodes) NodeRelease(node);
  proc->nodes.clear();
  std::vector<Ref*> refs;
  for (auto& [desc, ref] : proc->refs_by_desc) refs.push_back(ref);
  for (Ref* ref : refs) CleanupRef(ref);
  ReleaseWork(&proc->todo);
  ReleaseWork(&proc->delivered_death);
  std::vector<Buffer*> buffers;
  for (auto& [id, buffer] : proc->buffers) buffers.push_back(buffer);
  for (Buffer* buffer : buffers) FreeBuffer(buffer, false);
  g_procs.erase(proc->id);
}

// --- BINDER_WRITE_READ ---------------------------------------------------------------

struct In {
  const uint8_t* p;
  const uint8_t* end;
  bool Read(void* out, size_t n) {
    if (static_cast<size_t>(end - p) < n) return false;
    memcpy(out, p, n);
    p += n;
    return true;
  }
  template <typename T>
  bool Get(T* out) {
    return Read(out, sizeof(T));
  }
};

struct Out {
  std::vector<uint8_t> bytes;
  void Write(const void* data, size_t n) {
    if (n == 0) return;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), b, b + n);
  }
  template <typename T>
  void Put(const T& v) {
    Write(&v, sizeof(T));
  }
};

// Only complete pipe replies own the length prefix. Nested command streams
// stay plain Out values so their offsets and command-replacement rules do not
// change. Reserve room for the small status/handshake headers in one allocation.
struct WireReply : Out {
  WireReply() {
    bytes.reserve(32);
    bytes.resize(sizeof(uint32_t));
  }
};

// binder_thread_write. Returns 0 or -errno; *consumed counts commands.
int ThreadWrite(Proc* proc, Thread* thread, In in, uint32_t* consumed) {
  *consumed = 0;
  while (in.p < in.end) {
    uint32_t cmd;
    if (!in.Get(&cmd)) return -EFAULT;
    switch (cmd) {
      case BC_INCREFS:
      case BC_ACQUIRE:
      case BC_RELEASE:
      case BC_DECREFS: {
        uint32_t target;
        if (!in.Get(&target)) return -EFAULT;
        bool strong = cmd == BC_ACQUIRE || cmd == BC_RELEASE;
        bool increment = cmd == BC_INCREFS || cmd == BC_ACQUIRE;
        bool done = false;
        if (increment && target == 0 && g_context_mgr_node != nullptr) {
          if (g_context_mgr_node->proc == proc) {
            Log("proc %u: context manager node can't get a ref to itself", proc->id);
            done = true;
          } else {
            done = IncRefForNode(proc, g_context_mgr_node, strong, nullptr) != nullptr;
          }
        }
        if (!done && !UpdateRefForHandle(proc, target, increment, strong)) {
          Log("proc %u: %s of invalid ref %u", proc->id,
              increment ? (strong ? "BC_ACQUIRE" : "BC_INCREFS")
                        : (strong ? "BC_RELEASE" : "BC_DECREFS"),
              target);
        }
        break;
      }
      case BC_INCREFS_DONE:
      case BC_ACQUIRE_DONE: {
        binder_ptr_cookie pc;
        if (!in.Get(&pc)) return -EFAULT;
        Node* node = GetNode(proc, pc.ptr);
        if (node == nullptr || node->cookie != pc.cookie) {
          Log("proc %u: %s for unknown node", proc->id,
              cmd == BC_ACQUIRE_DONE ? "BC_ACQUIRE_DONE" : "BC_INCREFS_DONE");
          break;
        }
        if (cmd == BC_ACQUIRE_DONE) {
          if (!node->pending_strong_ref) {
            Log("proc %u: BC_ACQUIRE_DONE with no pending acquire", proc->id);
            break;
          }
          node->pending_strong_ref = false;
        } else {
          if (!node->pending_weak_ref) {
            Log("proc %u: BC_INCREFS_DONE with no pending increfs", proc->id);
            break;
          }
          node->pending_weak_ref = false;
        }
        DecNode(node, cmd == BC_ACQUIRE_DONE, false);
        break;
      }
      case BC_ATTEMPT_ACQUIRE:
      case BC_ACQUIRE_RESULT:
        return -EINVAL;
      case BC_FREE_BUFFER: {
        uint64_t id;
        if (!in.Get(&id)) return -EFAULT;
        DoFreeBuffer(proc, id);
        break;
      }
      case BC_TRANSACTION_SG:
      case BC_REPLY_SG:
      case BC_TRANSACTION:
      case BC_REPLY: {
        TransactionData td;
        if (cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG) {
          binder_transaction_data_sg sg;
          if (!in.Get(&sg)) return -EFAULT;
          td.tr = sg.transaction_data;
          td.buffers_size = sg.buffers_size;
        } else if (!in.Get(&td.tr)) {
          return -EFAULT;
        }
        if (!in.Get(&td.copy_error)) return -EFAULT;
        if (td.copy_error == 0) {
          if (!gesher::binder_sg::Dimensions(td.tr.data_size, td.tr.offsets_size, td.buffers_size)) return -EINVAL;
          uint32_t description_count;
          if (!in.Get(&description_count) || description_count > td.tr.offsets_size / sizeof(binder_size_t) ||
              description_count > wire::kMaxFileDescriptions)
            return -EINVAL;
          const uint64_t body_size = td.tr.data_size + td.tr.offsets_size + td.buffers_size +
              static_cast<uint64_t>(description_count) * sizeof(wire::FileDescription);
          if (body_size > static_cast<uint64_t>(in.end - in.p)) return -EFAULT;
          td.data.resize(td.tr.data_size);
          td.offsets.resize(td.tr.offsets_size / sizeof(binder_size_t));
          td.extra.resize(td.buffers_size);
          td.descriptions.resize(description_count);
          if (!in.Read(td.data.data(), td.data.size()) ||
              !in.Read(td.offsets.data(), td.tr.offsets_size) ||
              !in.Read(td.extra.data(), td.extra.size()) ||
              !in.Read(td.descriptions.data(), td.descriptions.size() * sizeof(wire::FileDescription)))
            return -EFAULT;
        }
        DoTransaction(proc, thread, td, cmd == BC_REPLY || cmd == BC_REPLY_SG);
        break;
      }
      case BC_REGISTER_LOOPER:
        if ((thread->looper & kLooperEntered) != 0) {
          thread->looper |= kLooperInvalid;
          Log("proc %u thread %d: BC_REGISTER_LOOPER after BC_ENTER_LOOPER", proc->id,
              thread->tid);
        } else if (proc->requested_threads == 0) {
          thread->looper |= kLooperInvalid;
          Log("proc %u thread %d: BC_REGISTER_LOOPER called without request", proc->id,
              thread->tid);
        } else {
          proc->requested_threads--;
          proc->requested_threads_started++;
        }
        thread->looper |= kLooperRegistered;
        break;
      case BC_ENTER_LOOPER:
        if ((thread->looper & kLooperRegistered) != 0) {
          thread->looper |= kLooperInvalid;
          Log("proc %u thread %d: BC_ENTER_LOOPER after BC_REGISTER_LOOPER", proc->id,
              thread->tid);
        }
        thread->looper |= kLooperEntered;
        break;
      case BC_EXIT_LOOPER:
        thread->looper |= kLooperExited;
        break;
      case BC_REQUEST_DEATH_NOTIFICATION:
      case BC_CLEAR_DEATH_NOTIFICATION: {
        binder_handle_cookie hc;
        if (!in.Get(&hc)) return -EFAULT;
        Ref* ref = GetRef(proc, hc.handle, false);
        if (ref == nullptr) {
          Log("proc %u: death notification for invalid ref %u", proc->id, hc.handle);
          break;
        }
        if (cmd == BC_REQUEST_DEATH_NOTIFICATION) {
          if (ref->death != nullptr) {
            Log("proc %u: death notification already set for ref %u", proc->id, hc.handle);
            break;
          }
          Death* death = new Death;
          death->cookie = hc.cookie;
          ref->death = death;
          if (ref->node->proc == nullptr) {
            death->type = WorkType::kDeadBinder;
            if ((thread->looper & (kLooperRegistered | kLooperEntered)) != 0) {
              EnqueueThreadWork(thread, death);
            } else {
              Enqueue(&proc->todo, death);
              WakeProc(proc);
            }
          }
        } else {
          Death* death = ref->death;
          if (death == nullptr) {
            Log("proc %u: clear death notification for ref %u without one", proc->id,
                hc.handle);
            break;
          }
          if (death->cookie != hc.cookie) {
            Log("proc %u: clear death notification cookie mismatch", proc->id);
            break;
          }
          ref->death = nullptr;
          if (death->list == nullptr) {
            death->type = WorkType::kClearDeathNotification;
            if ((thread->looper & (kLooperRegistered | kLooperEntered)) != 0) {
              EnqueueThreadWork(thread, death);
            } else {
              Enqueue(&proc->todo, death);
              WakeProc(proc);
            }
          } else {
            death->type = WorkType::kDeadBinderAndClear;
          }
        }
        break;
      }
      case BC_DEAD_BINDER_DONE: {
        uint64_t cookie;
        if (!in.Get(&cookie)) return -EFAULT;
        Death* death = nullptr;
        for (Work* w : proc->delivered_death) {
          if (static_cast<Death*>(w)->cookie == cookie) {
            death = static_cast<Death*>(w);
            break;
          }
        }
        if (death == nullptr) {
          Log("proc %u: BC_DEAD_BINDER_DONE of unknown cookie", proc->id);
          break;
        }
        Dequeue(death);
        if (death->type == WorkType::kDeadBinderAndClear) {
          death->type = WorkType::kClearDeathNotification;
          if ((thread->looper & (kLooperRegistered | kLooperEntered)) != 0) {
            EnqueueThreadWork(thread, death);
          } else {
            Enqueue(&proc->todo, death);
            WakeProc(proc);
          }
        }
        break;
      }
      default:
        Log("proc %u thread %d: unsupported command %08x", proc->id, thread->tid, cmd);
        return -EINVAL;
    }
    ++*consumed;
  }
  return 0;
}

// The read stream: the wire bytes, and the size it will have in the
// process's read buffer.
struct ReadStream {
  Out out;
  size_t size = 0;
  size_t limit = 0;
  void Put32(uint32_t v) {
    out.Put(v);
    size += 4;
  }
  void Put64(uint64_t v) {
    out.Put(v);
    size += 8;
  }
};

// binder_thread_read. Returns 0 or -errno.
int ThreadRead(std::unique_lock<std::mutex>& lock, Proc* proc, Thread* thread,
               ReadStream* rs, bool first, bool non_block) {
  if (first) rs->Put32(BR_NOOP);
  int ret = 0;
retry:
  bool wait_for_proc_work = AvailableForProcWork(thread);
  thread->looper |= kLooperWaiting;
  if (non_block) {
    if (!HasWork(thread, wait_for_proc_work)) ret = -EAGAIN;
  } else {
    for (;;) {
      if (HasWork(thread, wait_for_proc_work)) break;
      if (thread->is_dead || proc->is_dead) {
        ret = -EBADF;
        break;
      }
      if (wait_for_proc_work && !thread->waiting) {
        proc->waiting_threads.push_front(thread);
        thread->waiting = true;
      }
      thread->cv.wait(lock);
      if (thread->waiting) {
        proc->waiting_threads.remove(thread);
        thread->waiting = false;
      }
    }
  }
  thread->looper &= ~kLooperWaiting;
  if (ret != 0) return ret;

  for (;;) {
    WorkList* list;
    if (!thread->todo.empty()) {
      list = &thread->todo;
    } else if (!proc->todo.empty() && wait_for_proc_work) {
      list = &proc->todo;
    } else {
      if (rs->size == 4 && first && !thread->looper_need_return) goto retry;
      break;
    }
    if (rs->size + sizeof(binder_transaction_data_secctx) + 4 > rs->limit) break;
    Work* w = DequeueHead(list);
    if (thread->todo.empty()) thread->process_todo = false;

    Transaction* t = nullptr;
    switch (w->type) {
      case WorkType::kTransaction:
        t = static_cast<Transaction*>(w);
        break;
      case WorkType::kReturnError: {
        ErrorWork* e = static_cast<ErrorWork*>(w);
        rs->Put32(e->cmd);
        e->cmd = BR_OK;
        break;
      }
      case WorkType::kTransactionComplete:
        rs->Put32(BR_TRANSACTION_COMPLETE);
        delete w;
        break;
      case WorkType::kNode: {
        Node* node = static_cast<Node*>(w);
        bool strong = node->internal_strong_refs > 0 || node->local_strong_refs > 0;
        bool weak = !node->refs.empty() || node->local_weak_refs > 0 || node->tmp_refs > 0 || strong;
        bool has_strong_ref = node->has_strong_ref;
        bool has_weak_ref = node->has_weak_ref;
        uint64_t ptr = node->ptr;
        uint64_t cookie = node->cookie;
        if (weak && !has_weak_ref) {
          node->has_weak_ref = true;
          node->pending_weak_ref = true;
          node->local_weak_refs++;
        }
        if (strong && !has_strong_ref) {
          node->has_strong_ref = true;
          node->pending_strong_ref = true;
          node->local_strong_refs++;
        }
        if (!strong && has_strong_ref) node->has_strong_ref = false;
        if (!weak && has_weak_ref) node->has_weak_ref = false;
        if (!weak && !strong) FreeNode(node);
        auto put = [&](uint32_t cmd) {
          rs->Put32(cmd);
          rs->Put64(ptr);
          rs->Put64(cookie);
        };
        if (weak && !has_weak_ref) put(BR_INCREFS);
        if (strong && !has_strong_ref) put(BR_ACQUIRE);
        if (!strong && has_strong_ref) put(BR_RELEASE);
        if (!weak && has_weak_ref) put(BR_DECREFS);
        break;
      }
      case WorkType::kDeadBinder:
      case WorkType::kDeadBinderAndClear:
      case WorkType::kClearDeathNotification: {
        Death* death = static_cast<Death*>(w);
        uint32_t cmd = w->type == WorkType::kClearDeathNotification
                           ? BR_CLEAR_DEATH_NOTIFICATION_DONE
                           : BR_DEAD_BINDER;
        uint64_t cookie = death->cookie;
        if (w->type == WorkType::kClearDeathNotification) {
          delete death;
        } else {
          Enqueue(&proc->delivered_death, death);
        }
        rs->Put32(cmd);
        rs->Put64(cookie);
        if (cmd == BR_DEAD_BINDER) goto done;  // may cause transactions
        break;
      }
    }
    if (t == nullptr) continue;

    // A transaction or reply.
    binder_transaction_data tr = {};
    uint32_t cmd;
    Buffer* buffer = t->buffer;
    if (buffer == nullptr) {
      // Its buffer went with a failure; nothing to deliver.
      FreeTransaction(t);
      continue;
    }
    if (buffer->target_node != nullptr) {
      tr.target.ptr = buffer->target_node->ptr;
      tr.cookie = buffer->target_node->cookie;
      cmd = BR_TRANSACTION;
    } else {
      tr.target.ptr = 0;
      tr.cookie = 0;
      cmd = BR_REPLY;
    }
    tr.code = t->code;
    tr.flags = t->flags;
    tr.sender_euid = t->sender_euid;
    tr.sender_pid = t->from_pid;
    tr.data_size = buffer->data.size();
    tr.offsets_size = buffer->offsets.size() * sizeof(binder_size_t);
    buffer->allow_user_free = true;
    thread->pending_buffer = buffer->id;
    thread->pending_buffer_claimed = false;
    rs->Put32(cmd);
    rs->out.Put(tr);
    rs->size += sizeof(tr);
    rs->out.Put(buffer->id);
    rs->out.Put<uint64_t>(buffer->extra.size());
    rs->out.Put<uint32_t>(static_cast<uint32_t>(buffer->descriptions.size()));
    rs->out.Write(buffer->data.data(), buffer->data.size());
    rs->out.Write(buffer->offsets.data(), tr.offsets_size);
    rs->out.Write(buffer->extra.data(), buffer->extra.size());
    rs->out.Write(buffer->descriptions.data(), buffer->descriptions.size() * sizeof(wire::FileDescription));
    TRACE("proc %u thread %d: %s %d delivered", proc->id, thread->tid,
          cmd == BR_REPLY ? "reply" : "transaction", t->debug_id);
    if (cmd != BR_REPLY && (t->flags & TF_ONE_WAY) == 0) {
      t->to_parent = thread->transaction_stack;
      t->to_thread = thread->shared_from_this();
      thread->transaction_stack = t;
    } else {
      FreeTransaction(t);
    }
    break;
  }

done:
  if (proc->requested_threads == 0 && proc->waiting_threads.empty() &&
      proc->requested_threads_started < proc->max_threads &&
      (thread->looper & (kLooperRegistered | kLooperEntered)) != 0 && first &&
      rs->out.bytes.size() >= 4) {
    proc->requested_threads++;
    TRACE("proc %u: BR_SPAWN_LOOPER", proc->id);
    uint32_t spawn = BR_SPAWN_LOOPER;
    memcpy(rs->out.bytes.data(), &spawn, 4);  // in place of the BR_NOOP
  }
  return 0;
}

// --- connections ---------------------------------------------------------------------

bool ReadExact(HANDLE pipe, void* buf, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    DWORD got = 0;
    DWORD want = n > 0x10000000 ? 0x10000000 : static_cast<DWORD>(n);
    if (!ReadFile(pipe, p, want, &got, nullptr) || got == 0) return false;
    p += got;
    n -= got;
  }
  return true;
}

bool WriteExact(HANDLE pipe, const void* buf, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    DWORD put = 0;
    DWORD want = n > 0x10000000 ? 0x10000000 : static_cast<DWORD>(n);
    if (!WriteFile(pipe, p, want, &put, nullptr) || put == 0) return false;
    p += put;
    n -= put;
  }
  return true;
}

constexpr uint32_t kMaxMessage = 64u << 20;

bool ReadMessage(HANDLE pipe, std::vector<uint8_t>* msg) {
  uint32_t len;
  if (!ReadExact(pipe, &len, sizeof(len)) || len > kMaxMessage) return false;
  msg->resize(len);
  return ReadExact(pipe, msg->data(), len);
}

bool WriteMessage(HANDLE pipe, WireReply& out) {
  uint32_t len = static_cast<uint32_t>(out.bytes.size() - sizeof(uint32_t));
  memcpy(out.bytes.data(), &len, sizeof(len));
  return WriteExact(pipe, out.bytes.data(), out.bytes.size());
}

uint64_t RandomKey() {
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  uint64_t x = static_cast<uint64_t>(counter.QuadPart) ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
               reinterpret_cast<uintptr_t>(&counter);
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return x;
}

// Whether the client of `pipe` is restricted (GESHER_SANDBOX=appcontainer, ADR 0090):
// in an AppContainer, or below Medium integrity. The pipe's Low label admits both,
// and neither may speak for the system. Its token, not its hello, says so: the
// pipe impersonates the sender of the last message read. A token that cannot be
// read counts as restricted.
bool ClientIsRestricted(HANDLE pipe) {
  gesher::process_identity::Principal principal;
  return !gesher::process_identity::ReadPipePrincipal(pipe, &principal) ||
         principal.container.size != 0 || principal.restricted ||
         principal.integrity < SECURITY_MANDATORY_MEDIUM_RID;
}

void ReapRevokedPeers() {
  std::vector<std::shared_ptr<Proc>> revoked;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    for (const auto& entry : g_procs) {
      if (!g_peer_registry->Live(entry.second->identity)) revoked.push_back(entry.second);
    }
  }
  for (const auto& proc : revoked) ProcRelease(proc.get());
}

std::mutex g_capture_policy_lock;
peer::OwnedHandle g_capture_policy;

[[maybe_unused]] bool CaptureAuthorized(const peer::Registry::Lease& process,
                                        gesher::capture_policy::Kind kind,
                                        uint64_t session = 0) {
  const bool finish = kind == gesher::capture_policy::Kind::kCameraFinish;
  if (!process || process->role != peer::Role::kApplication ||
      (!finish && !g_peer_registry->Live(process))) return false;
  std::lock_guard<std::mutex> lock(g_capture_policy_lock);
  if (!g_capture_policy) return false;
  gesher::capture_policy::Request request{process->uid, kind, process->installation, session};
  uint32_t allowed = 0;
  const ULONGLONG deadline = GetTickCount64() + 500;
  if (!launch::Transfer(g_capture_policy.get(), &request, sizeof(request), true, deadline) ||
      !launch::Transfer(g_capture_policy.get(), &allowed, sizeof(allowed), false, deadline)) {
    g_capture_policy.reset();
    return false;
  }
  return allowed == 1 && (finish || g_peer_registry->Live(process));
}

void ServeLaunchAuthority(HANDLE endpoint, ULONG launcher_pid,
                          gesher::process_identity::Principal authority) {
  peer::OwnedHandle endpoint_owner(endpoint);
  peer::Registry::Lease system;
  bool admitted_standalone = false;
  launch::Request request;
  while (launch::Transfer(endpoint, &request, sizeof(request), false)) {
    gesher::process_identity::Principal sender;
    if (request.magic != 0x474c4152 || request.version != 1 ||
        !gesher::process_identity::ReadPipePrincipal(endpoint, &sender) || !(sender == authority) ||
        (system && !g_peer_registry->Live(system))) {
      break;
    }
    bool accepted = false;
    switch (request.action) {
      case launch::Action::kEnroll: {
        peer::OwnedHandle process(launch::DecodeHandle(request.process));
        const bool app = request.role == peer::Role::kApplication;
        const bool role_allowed = g_record_only ? app && !admitted_standalone
                                                : app == static_cast<bool>(system);
        auto* container = request.container.bytes.data();
        const bool sid_matches = app
            ? request.container.size != 0 && request.container.size <= request.container.bytes.size() &&
                IsValidSid(container) && GetLengthSid(container) == request.container.size &&
                (!g_record_only || g_record_container.Matches(container))
            : request.container.size == 0;
        if (role_allowed && sid_matches &&
            (request.role != peer::Role::kLauncher || request.pid == launcher_pid)) {
          accepted = g_peer_registry->Enroll(std::move(process), request.pid, request.birth,
              request.role, request.uid, request.installation, app ? container : nullptr);
          if (g_record_only && accepted) admitted_standalone = true;
        }
        break;
      }
      case launch::Action::kHandoff:
        if (!g_record_only && !system) {
          system = g_peer_registry->FindGeneration(request.pid, request.birth, peer::Role::kSystemServer);
          accepted = static_cast<bool>(system);
        }
        break;
      case launch::Action::kRetire:
        if (!g_record_only && (!system || g_peer_registry->FindGeneration(request.pid, request.birth,
                                                                          peer::Role::kApplication))) {
          accepted = g_peer_registry->Retire(request.pid, request.birth);
        }
        if (accepted) ReapRevokedPeers();
        break;
      case launch::Action::kRevokeInstallation:
        if (system && request.installation != std::array<uint8_t, 16>{}) {
          accepted = g_peer_registry->RevokeInstallation(request.installation);
          if (accepted) ReapRevokedPeers();
        }
        break;
      case launch::Action::kRestoreInstallation:
        if (system && request.installation != std::array<uint8_t, 16>{}) {
          accepted = g_peer_registry->RestoreInstallation(request.installation);
        }
        break;
      case launch::Action::kAttachJob: {
        peer::OwnedHandle job(launch::DecodeHandle(request.process));
        if (system || g_record_only) accepted = g_peer_registry->AttachJob(std::move(job));
        break;
      }
      case launch::Action::kAttachCaptureSessions: {
        peer::OwnedHandle pipe(launch::DecodeHandle(request.process));
        if ((system || g_record_only) && pipe && GetFileType(pipe.get()) == FILE_TYPE_PIPE) {
          std::lock_guard<std::mutex> lock(g_capture_policy_lock);
          if (!g_capture_policy) {
            g_capture_policy = std::move(pipe);
            accepted = true;
          }
        }
        break;
      }
    }
    uint32_t status = accepted ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
    if (!launch::Transfer(endpoint, &status, sizeof(status), true)) break;
  }
  g_peer_registry->CloseAuthority();
  ReapRevokedPeers();
  ExitProcess(ERROR_BROKEN_PIPE);
}

void ServeProc(HANDLE pipe, In in, bool operator_hello) {
  uint32_t version, pid, uid;
  if (!in.Get(&version) || version != wire::kVersion || !in.Get(&pid) || !in.Get(&uid)) return;
  peer::Registry::Lease identity;
  if (g_peer_registry) {
    identity = operator_hello ? g_peer_registry->AdmitOperator(pipe)
                              : g_peer_registry->Authenticate(pipe);
    if (!identity || identity->generation.pid > INT32_MAX) {
      WireReply reply;
      reply.Put<int32_t>(-EPERM);
      if (WriteMessage(pipe, reply)) FlushFileBuffers(pipe);
      return;
    }
    pid = identity->generation.pid;
    uid = identity->uid;
  } else if (operator_hello) {
    return;
  } else if (g_sandbox && ClientIsRestricted(pipe)) {
    // A first step of kernel-verified identity (ADR 0090, milestone M2 completes it):
    // a restricted process is an app, at its own pid. It cannot claim a system UID
    // (SYSTEM_UID 1000 and every other below FIRST_APPLICATION_UID) or another pid.
    ULONG actual = 0;
    bool known = GetNamedPipeClientProcessId(pipe, &actual) != FALSE;
    if (uid < 10000 || !known || actual != pid) {
      WireReply reply;
      reply.Put<int32_t>(-EPERM);
      // The client reads EPERM before the connection closes, not a broken pipe.
      if (WriteMessage(pipe, reply)) FlushFileBuffers(pipe);
      Log("rejected a restricted client: claimed pid %u uid %u, actual pid %lu", pid, uid,
          actual);
      return;
    }
  }
  auto proc_owner = std::make_shared<Proc>();
  Proc* proc = proc_owner.get();
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_peer_registry && !g_peer_registry->Live(identity)) return;
    proc->id = g_next_proc_id++;
    proc->key = RandomKey();
    proc->pid = static_cast<int>(pid);
    proc->uid = uid;
    proc->identity = std::move(identity);
    g_procs[proc->id] = proc_owner;
  }
  TRACE("proc %u: pid %u opened the device", proc->id, pid);
  WireReply reply;
  reply.Put<int32_t>(0);
  reply.Put<uint32_t>(proc->id);
  reply.Put<uint64_t>(proc->key);
  if (WriteMessage(pipe, reply)) {
    // Nothing more comes on this connection: it breaks when the process
    // closes the fd or exits.
    std::vector<uint8_t> msg;
    while (ReadMessage(pipe, &msg)) {
    }
  }
  ProcRelease(proc);
}

#ifdef GESHER_RECORD_LOCK_BROKER
bool WaitRecordClient(HANDLE pipe, bool accept_ack);
#endif

void ServeEnrollChild(HANDLE pipe, In in) {
  uint32_t version = 0;
  uint64_t source = 0;
  if (!g_peer_registry || !in.Get(&version) || version != wire::kVersion ||
      !in.Get(&source) || in.p != in.end) return;
  const auto parent = g_peer_registry->Authenticate(pipe);
  HANDLE child = nullptr;
  const bool duplicated = parent && parent->role == peer::Role::kApplication &&
      DuplicateHandle(parent->process.get(), launch::DecodeHandle(source), GetCurrentProcess(),
          &child, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE, FALSE, 0);
  const bool accepted = duplicated && g_peer_registry->EnrollDescendant(parent, peer::OwnedHandle(child));
  WireReply reply;
  reply.Put<int32_t>(accepted ? 0 : -EPERM);
  if (!WriteMessage(pipe, reply)) return;
#ifdef GESHER_RECORD_LOCK_BROKER
  if (g_record_only) {
    WaitRecordClient(pipe, false);
    return;
  }
#endif
  FlushFileBuffers(pipe);
}

#ifdef GESHER_RECORD_LOCK_BROKER
namespace locks = gesher::record_locks;

std::mutex g_record_lanes_lock;
std::set<std::pair<DWORD, uint64_t>> g_record_lanes;

struct RecordLane {
  explicit RecordLane(const gesher::process_identity::Process& generation)
      : id{generation.pid, generation.birth} {
    std::lock_guard<std::mutex> lock(g_record_lanes_lock);
    active = g_record_lanes.insert(id).second;
  }
  ~RecordLane() { Release(); }
  void Release() {
    if (!active) return;
    std::lock_guard<std::mutex> lock(g_record_lanes_lock);
    g_record_lanes.erase(id);
    active = false;
  }
  std::pair<DWORD, uint64_t> id;
  bool active = false;
};

bool ReplyRecordLock(HANDLE pipe, int error, const locks::LinuxFlock* result = nullptr) {
  WireReply reply;
  reply.Put<int32_t>(-error);
  if (result && !error) {
    reply.Put<int16_t>(result->type);
    reply.Put<int16_t>(result->whence);
    reply.Put<int64_t>(result->start);
    reply.Put<int64_t>(result->length);
    reply.Put<int32_t>(result->pid);
  }
  return WriteMessage(pipe, reply);
}

bool WaitRecordClient(HANDLE pipe, bool expect_acknowledgment) {
  // A guest cannot keep a broker thread or a temporary file pin indefinitely.
  const ULONGLONG deadline = GetTickCount64() + 5000;
  while (GetTickCount64() < deadline) {
    DWORD bytes = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes, nullptr)) return false;
    if (expect_acknowledgment && bytes >= 2 * sizeof(uint32_t)) {
      uint32_t acknowledgment[2];
      return ReadExact(pipe, acknowledgment, sizeof(acknowledgment)) &&
          acknowledgment[0] == sizeof(uint32_t) && acknowledgment[1] == 1;
    }
    Sleep(1);
  }
  return false;
}

void FinishRecordReply(HANDLE pipe, int error, const locks::LinuxFlock* result = nullptr) {
  // The client closes after reading its reply. A bounded wait avoids both
  // discarding unread bytes on DisconnectNamedPipe and blocking in FlushFileBuffers.
  if (ReplyRecordLock(pipe, error, result)) WaitRecordClient(pipe, false);
}

namespace playback = gesher::audio_state;

struct AudioRegion {
  peer::OwnedHandle section;
  playback::Activity* state = nullptr;
  ~AudioRegion() { if (state) UnmapViewOfFile(state); }
};

using WeakPeer = std::weak_ptr<const peer::Peer>;
std::map<WeakPeer, AudioRegion, std::owner_less<WeakPeer>> g_audio_activity;
std::mutex g_audio_lock;
uint32_t g_next_audio_session = 1;  // guarded by g_audio_lock
peer::OwnedHandle g_audio_device_controls;
#ifdef GESHER_AUDIO_RENDER_BROKER
playback::DeviceState* g_audio_controls_view = nullptr;
#endif

bool PrepareAudioDeviceControls() {
  if (!g_audio_device_controls) {
    peer::OwnedHandle section(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                 sizeof(playback::DeviceState), nullptr));
    if (!section) return false;
    auto* state = static_cast<playback::DeviceState*>(
        MapViewOfFile(section.get(), FILE_MAP_WRITE, 0, 0, sizeof(playback::DeviceState)));
    if (!state) return false;
    playback::InitDeviceState(state);
    g_audio_device_controls = std::move(section);
#ifdef GESHER_AUDIO_RENDER_BROKER
    g_audio_controls_view = state;
#else
    UnmapViewOfFile(state);
#endif
  }
  return true;
}

HANDLE DuplicateAudioDeviceControls(const peer::Registry::Lease& process, bool writable) {
  if (!PrepareAudioDeviceControls()) return nullptr;
  HANDLE child = nullptr;
  return DuplicateHandle(GetCurrentProcess(), g_audio_device_controls.get(), process->process.get(),
                         &child, writable ? FILE_MAP_WRITE : FILE_MAP_READ, FALSE, 0) ? child : nullptr;
}

bool AudioPeer(const peer::Registry::Lease& process) {
  return process && (process->role == peer::Role::kApplication ||
                     process->role == peer::Role::kSystemServer);
}

void ServeAudioPlayback(HANDLE pipe, In in, uint32_t op) {
  uint32_t version = 0, stream = 0, in_past_ms = 0;
  if (!g_peer_registry || !in.Get(&version) || version != wire::kVersion ||
      !in.Get(&stream) || !in.Get(&in_past_ms) || in.p != in.end) return;
  const auto process = g_peer_registry->Authenticate(pipe);
  int32_t status = -EACCES;
  uint64_t result = 0;
  if (AudioPeer(process)) {
    std::lock_guard<std::mutex> lock(g_audio_lock);
    // A revoked or exited producer cannot leave activity or a mapped view
    // behind. A retained identity refers to this exact process generation.
    for (auto at = g_audio_activity.begin(); at != g_audio_activity.end();) {
      auto owner = at->first.lock();
      if (!owner || !g_peer_registry->Live(owner)) at = g_audio_activity.erase(at);
      else ++at;
    }
    if (g_peer_registry->Live(process)) {
      status = 0;
      if (op == wire::kAudioPlaybackPublish) {
        auto& region = g_audio_activity[process];
        if (!region.section) {
          region.section.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                  sizeof(playback::Activity), nullptr));
          if (region.section) {
            region.state = static_cast<playback::Activity*>(
                MapViewOfFile(region.section.get(), FILE_MAP_WRITE, 0, 0, sizeof(playback::Activity)));
          }
        }
        HANDLE child = nullptr;
        if (region.state && DuplicateHandle(GetCurrentProcess(), region.section.get(),
                                            process->process.get(), &child, FILE_MAP_WRITE, FALSE, 0)) {
          result = launch::EncodeHandle(child);
        } else {
          if (!region.state) g_audio_activity.erase(process);
          status = -EIO;
        }
      } else if (op == wire::kAudioPlaybackQuery && stream < playback::kStreamCount) {
        const auto now = GetTickCount64();
        for (const auto& entry : g_audio_activity) {
          // The bounded timestamp comes from an untrusted producer. Discard
          // future values outside one mixer interval, rather than extending
          // them every time the service queries the stream.
          const auto until = static_cast<ULONGLONG>(
              InterlockedCompareExchange64(&entry.second.state->active_until_ms[stream], 0, 0));
          if (until && until <= now + playback::kActiveMs && until + in_past_ms >= now) {
            result = 1;
            break;
          }
        }
      } else if (op == wire::kAudioPlaybackSession && g_next_audio_session < INT_MAX / 8u) {
        result = (g_next_audio_session++ << 3) | 1u;
      } else if (op == wire::kAudioDeviceControls && stream <= 1) {
        const bool writable = stream == 1;
        // A record-only broker belongs to one standalone app's process tree.
        // Only the real SystemServer sets shared controls for a normal device.
        if (writable && !g_record_only && process->role != peer::Role::kSystemServer) {
          status = -EACCES;
        } else {
          const HANDLE section = DuplicateAudioDeviceControls(process, writable);
          if (section) result = launch::EncodeHandle(section);
          else status = -EIO;
        }
      } else {
        status = -EINVAL;
      }
    }
  }
  WireReply reply;
  reply.Put<int32_t>(status);
  reply.Put<uint64_t>(result);
  if (WriteMessage(pipe, reply)) WaitRecordClient(pipe, false);
}

#ifdef GESHER_AUDIO_RENDER_BROKER
std::set<std::pair<DWORD, uint64_t>> g_audio_renderers;
std::set<std::pair<DWORD, uint64_t>> g_audio_capturers;

void ServeAudioCapture(HANDLE pipe, In in) {
  uint32_t version = 0, info_only = 0;
  if (!g_peer_registry || !in.Get(&version) || version != wire::kVersion ||
      !in.Get(&info_only) || info_only > 1 || in.p != in.end) return;
  const auto process = g_peer_registry->Authenticate(pipe);
  if (!process || process->role != peer::Role::kApplication) return;
  const auto id = std::pair{process->generation.pid, process->generation.birth};
  if (!info_only) {
    std::lock_guard<std::mutex> lock(g_audio_lock);
    if (!g_audio_capturers.insert(id).second) return;
  }
  gesher::media::ServeBrokeredCapture(pipe, info_only != 0,
      [&] { return g_peer_registry->Live(process); },
      [&] { return CaptureAuthorized(process, gesher::capture_policy::Kind::kMicrophone); });
  if (!info_only) {
    std::lock_guard<std::mutex> lock(g_audio_lock);
    g_audio_capturers.erase(id);
  }
}

void ServeAudioRender(HANDLE pipe, In in) {
  uint32_t version = 0, length = 0;
  if (!g_peer_registry || !in.Get(&version) || version != wire::kVersion ||
      !in.Get(&length) || length > gesher::media::kAudioRenderLabelChars ||
      static_cast<size_t>(in.end - in.p) != length * sizeof(char16_t)) return;
  const auto process = g_peer_registry->Authenticate(pipe);
  if (!process || process->role != peer::Role::kApplication) return;
  std::u16string label(length, u'\0');
  if (!in.Read(label.data(), length * sizeof(char16_t))) return;
  for (char16_t& ch : label) {
    if (ch < u' ') ch = u' ';
  }
  const auto id = std::pair{process->generation.pid, process->generation.birth};
  {
    std::lock_guard<std::mutex> lock(g_audio_lock);
    if (!PrepareAudioDeviceControls() || !g_audio_renderers.insert(id).second) return;
    gesher::media::UseBrokerAudioControls(g_audio_controls_view);
  }
  gesher::media::ServeBrokeredAudio(pipe, label, process->generation.pid, [&] {
    return g_peer_registry->Live(process);
  });
  {
    std::lock_guard<std::mutex> lock(g_audio_lock);
    g_audio_renderers.erase(id);
  }
}
#endif

#ifdef GESHER_CAMERA_BROKER_HOST
std::mutex g_camera_lock;
std::set<std::pair<DWORD, uint64_t>> g_camera_sessions;
uint64_t g_camera_next_session = 1;

void ServeCamera(HANDLE pipe, In in) {
  uint32_t version = 0, info_only = 0;
  if (!g_peer_registry || !in.Get(&version) || version != wire::kVersion ||
      !in.Get(&info_only) || info_only > 1 || in.p != in.end) return;
  const auto process = g_peer_registry->Authenticate(pipe);
  if (!process || process->role != peer::Role::kApplication) return;
  const auto identity = std::pair{process->generation.pid, process->generation.birth};
  uint64_t session = 0;
  if (!info_only) {
    std::lock_guard lock(g_camera_lock);
    // CameraService advertises no concurrent camera sessions. Keep a bounded
    // number of readers and buffers even if a guest makes raw requests.
    if (g_camera_sessions.size() < 16 && g_camera_next_session &&
        g_camera_sessions.insert(identity).second) session = g_camera_next_session++;
  }
  using Kind = gesher::capture_policy::Kind;
  gesher::devices::camera::ServeBrokeredCamera(pipe, info_only != 0,
      [&] { return g_peer_registry->Live(process); },
      [&] { return CaptureAuthorized(process, Kind::kCameraStart, session); },
      [&] { return CaptureAuthorized(process, Kind::kCameraCheck, session); }, session != 0);
  if (session) {
    CaptureAuthorized(process, Kind::kCameraFinish, session);
    std::lock_guard lock(g_camera_lock);
    g_camera_sessions.erase(identity);
  }
}
#endif

void ServeFileRecordLock(HANDLE pipe, In in, bool close) {
  uint32_t version = 0;
  uint64_t source = 0;
  int32_t command = 0;
  locks::LinuxFlock input{};
  if (!in.Get(&version) || version != wire::kVersion || !in.Get(&source) ||
      !source || source > UINTPTR_MAX) return FinishRecordReply(pipe, EINVAL);
  if ((!close && (!in.Get(&command) || !in.Get(&input.type) || !in.Get(&input.whence) ||
          !in.Get(&input.start) || !in.Get(&input.length) || !in.Get(&input.pid))) || in.p != in.end) {
    return FinishRecordReply(pipe, EINVAL);
  }
  const auto caller_peer = g_peer_registry ? g_peer_registry->Authenticate(pipe) : nullptr;
  if (!caller_peer || caller_peer->role != peer::Role::kApplication) return FinishRecordReply(pipe, EACCES);
  RecordLane lane(caller_peer->generation);
  if (!lane.active) return FinishRecordReply(pipe, EBUSY);
  auto complete = [&](int error, const locks::LinuxFlock* result = nullptr) {
    lane.Release();
    FinishRecordReply(pipe, error, result);
  };
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(caller_peer->process.get(), launch::DecodeHandle(source), GetCurrentProcess(),
                       &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    return complete(EBADF);
  }
  peer::OwnedHandle file(duplicate);
  if (!g_peer_registry->Live(caller_peer)) return complete(EACCES);
  const locks::ProcessIdentity caller{caller_peer->generation.pid, 0, caller_peer->generation.birth};
  if (!close) {
    locks::LinuxFlock result{};
    int error = locks::BrokerDescriptorFcntl(file.get(), caller, command, input, &result);
    file.reset();
    return complete(error, command == 5 || command == 36 ? &result : nullptr);
  }

  locks::File identity{};
  int error = locks::FileIdentity(file.get(), &identity);
  if (error) return complete(error);
  if (!ReplyRecordLock(pipe, 0) || !WaitRecordClient(pipe, true)) return;
  file.reset();  // Neither temporary pin should be counted as an OFD user alias.
  if (g_peer_registry->Authenticate(pipe) != caller_peer) return complete(EACCES);
  complete(locks::BrokerDescriptorClosed(identity, caller));
}
#endif

// --- the descriptor broker (ADR 0088 section 3) --------------------------------------
//
// SCM_RIGHTS on Gesher's AF_UNIX socketpairs. A deposited group of
// descriptors is a set of handles this process owns (duplicated from the
// sender), with each descriptor's metadata. It leaves when a receiver claims
// it (by token: datagram pairs carry the token in the message), takes it from
// its stream connection's queue once it has read the bytes sent with it (the
// group's position, binder_wire.h), or the sender cancels it. As on Linux,
// descriptors in flight outlive their sender.

struct RightsEntry {
  HANDLE handle;
  uint64_t metadata;
  HANDLE shared;
  uint32_t kind;
};

struct RightsGroup {
  uint64_t token[2] = {};
  uint64_t queue = 0;  // 0: claimed by token only
  uint64_t position = 0;  // a queued group's: where its bytes begin
  std::vector<RightsEntry> entries;
};

std::mutex g_rights_lock;
std::map<std::pair<uint64_t, uint64_t>, std::shared_ptr<RightsGroup>> g_rights_groups;
std::map<uint64_t, std::deque<std::shared_ptr<RightsGroup>>> g_rights_queues;
uint32_t g_rights_in_flight = 0;
volatile LONG64* g_rights_doorbell = nullptr;  // wire::kRightsDoorbellSlots values

// Publishes the lowest position waiting for the receiving port of `queue`
// (binder_wire.h's doorbell); g_rights_lock held. Every queue of that port
// counts: the doorbell is per port.
void RefreshDoorbell(uint64_t queue) {
  if (g_rights_doorbell == nullptr || queue == 0) return;
  const uint64_t port = (queue >> 16) & 0xffff;
  uint64_t lowest = UINT64_MAX;
  for (auto it = g_rights_queues.lower_bound(port << 16);
       it != g_rights_queues.end() && (it->first >> 16) == port; ++it) {
    for (const auto& group : it->second) lowest = std::min(lowest, group->position);
  }
  InterlockedExchange64(&g_rights_doorbell[port],
                        lowest == UINT64_MAX ? 0 : static_cast<LONG64>(lowest + 1));
}

// g_rights_lock held.
void CloseRightsGroup(RightsGroup* group) {
  for (const RightsEntry& e : group->entries) {
    CloseHandle(e.handle);
    if (e.shared) CloseHandle(e.shared);
  }
  g_rights_in_flight -= static_cast<uint32_t>(group->entries.size());
  group->entries.clear();
}

// Moves a group's handles into `to` (the caller's process); g_rights_lock held.
// Each handle the broker held is closed, delivered or not; one that cannot be
// delivered is reported as handle 0, which the receiver reports as a failure.
void DeliverRights(HANDLE to, RightsGroup* group, WireReply* reply) {
  for (const RightsEntry& e : group->entries) {
    HANDLE out = nullptr, shared = nullptr;
    bool ok = DuplicateHandle(GetCurrentProcess(), e.handle, to, &out, 0, FALSE,
                               DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    if (e.shared && !DuplicateHandle(GetCurrentProcess(), e.shared, to, &shared, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) ok = false;
    if (!ok) {
      Log("rights: DuplicateHandle into the receiver failed: %lu", GetLastError());
      if (out) DuplicateHandle(to, out, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
      if (shared) DuplicateHandle(to, shared, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
      out = shared = nullptr;
    }
    reply->Put<uint64_t>(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(out)));
    reply->Put<uint64_t>(e.metadata);
    reply->Put<uint64_t>(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(shared)));
    reply->Put<uint32_t>(e.kind);
  }
  g_rights_in_flight -= static_cast<uint32_t>(group->entries.size());
  group->entries.clear();
}

void ServeRights(HANDLE pipe, In in) {
  uint32_t version;
  ULONG client_pid = 0;
  peer::Registry::Lease identity;
  peer::OwnedHandle legacy_client;
  HANDLE client = nullptr;
  if (in.Get(&version) && version == wire::kRightsVersion) {
    if (g_peer_registry) {
      identity = g_peer_registry->Authenticate(pipe);
      if (identity) {
        client_pid = identity->generation.pid;
        client = identity->process.get();
      }
    } else if (GetNamedPipeClientProcessId(pipe, &client_pid)) {
      legacy_client.reset(OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid));
      client = legacy_client.get();
    }
  }
  WireReply hello;
  hello.Put<int32_t>(client != nullptr ? 0 : -ESRCH);
  if (!WriteMessage(pipe, hello) || client == nullptr) {
    return;
  }
  std::vector<uint8_t> msg;
  while (ReadMessage(pipe, &msg)) {
    if (g_peer_registry && g_peer_registry->Authenticate(pipe) != identity) break;
    In req{msg.data(), msg.data() + msg.size()};
    uint32_t op;
    if (!req.Get(&op)) break;
    WireReply reply;
    if (op == wire::kRightsDeposit) {
      uint64_t queue, position;
      uint32_t count;
      if (!req.Get(&queue) || !req.Get(&position) || !req.Get(&count) || count == 0 ||
          count > wire::kRightsMaxPerMessage) {
        break;
      }
      auto group = std::make_shared<RightsGroup>();
      group->queue = queue;
      group->position = position;
      int32_t status = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint64_t handle, metadata, shared_handle;
        uint32_t kind;
        if (!req.Get(&handle) || !req.Get(&metadata) || !req.Get(&shared_handle) || !req.Get(&kind) ||
            handle == 0 || handle > UINTPTR_MAX || shared_handle > UINTPTR_MAX ||
            (kind == wire::kSocketFileDescription
              ? shared_handle != 0 || (metadata & 1) == 0
              : kind == wire::kNativeFenceFileDescription
              ? shared_handle != 0 || metadata != wire::kCompletedNativeFenceMetadata
              : kind > 2 || shared_handle == 0 || metadata != 0)) {
          status = -EINVAL;
          break;
        }
        HANDLE own = nullptr, shared = nullptr;
        if (!DuplicateHandle(client, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)),
                             GetCurrentProcess(), &own, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
          Log("rights: DuplicateHandle from pid %lu failed: %lu", client_pid, GetLastError());
          status = -EBADF;
          break;
        }
        if (shared_handle && !DuplicateHandle(client,
              reinterpret_cast<HANDLE>(static_cast<uintptr_t>(shared_handle)), GetCurrentProcess(),
              &shared, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
          CloseHandle(own);
          status = -EBADF;
          break;
        }
        group->entries.push_back({own, metadata, shared, kind});
      }
      if (status == 0 &&
          !BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(group->token),
                                          sizeof(group->token),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        status = -EIO;
      }
      {
        std::lock_guard<std::mutex> lock(g_rights_lock);
        g_rights_in_flight += static_cast<uint32_t>(group->entries.size());
        if (status == 0 && g_rights_in_flight > wire::kRightsMaxInFlight) status = -ETOOMANYREFS;
        if (status != 0) {
          CloseRightsGroup(group.get());
        } else {
          g_rights_groups[{group->token[0], group->token[1]}] = group;
          if (queue != 0) {
            g_rights_queues[queue].push_back(group);
            RefreshDoorbell(queue);
          }
        }
      }
      TRACE("rights: pid %lu deposited %u descriptor(s), queue %llx at %llu: %d", client_pid,
            count, (unsigned long long)queue, (unsigned long long)position, status);
      reply.Put<int32_t>(status);
      reply.Put<uint64_t>(status == 0 ? group->token[0] : 0);
      reply.Put<uint64_t>(status == 0 ? group->token[1] : 0);
    } else if (op == wire::kRightsClaim || op == wire::kRightsCancel) {
      uint64_t token[2];
      if (!req.Get(&token[0]) || !req.Get(&token[1])) break;
      std::lock_guard<std::mutex> lock(g_rights_lock);
      auto it = g_rights_groups.find({token[0], token[1]});
      std::shared_ptr<RightsGroup> group;
      if (it != g_rights_groups.end()) {
        group = it->second;
        if (group->queue != 0) {
          // A queued group leaves by its queue (take) or its sender's cancel.
          auto q = g_rights_queues.find(group->queue);
          bool queued = false;
          if (q != g_rights_queues.end()) {
            auto at = std::find(q->second.begin(), q->second.end(), group);
            if (at != q->second.end() && op == wire::kRightsCancel) {
              q->second.erase(at);
              if (q->second.empty()) g_rights_queues.erase(q);
              RefreshDoorbell(group->queue);
              queued = true;
            }
          }
          if (!queued) group = nullptr;
        }
        if (group != nullptr) g_rights_groups.erase(it);
      }
      if (group == nullptr) {
        reply.Put<int32_t>(-ENOENT);
        if (op == wire::kRightsClaim) reply.Put<uint32_t>(0);
      } else if (op == wire::kRightsCancel) {
        CloseRightsGroup(group.get());
        reply.Put<int32_t>(0);
      } else {
        reply.Put<int32_t>(0);
        reply.Put<uint32_t>(static_cast<uint32_t>(group->entries.size()));
        DeliverRights(client, group.get(), &reply);
      }
    } else if (op == wire::kRightsTake) {
      uint64_t queue, limit;
      if (!req.Get(&queue) || queue == 0 || !req.Get(&limit)) break;
      std::lock_guard<std::mutex> lock(g_rights_lock);
      // The groups whose bytes the receiver has begun to read; the others
      // stay queued, in order.
      std::deque<std::shared_ptr<RightsGroup>> groups;
      auto it = g_rights_queues.find(queue);
      if (it != g_rights_queues.end()) {
        std::deque<std::shared_ptr<RightsGroup>> waiting;
        for (auto& group : it->second) {
          (group->position < limit ? groups : waiting).push_back(std::move(group));
        }
        if (waiting.empty()) g_rights_queues.erase(it);
        else it->second.swap(waiting);
        RefreshDoorbell(queue);
      }
      uint32_t count = 0;
      for (const auto& group : groups) count += static_cast<uint32_t>(group->entries.size());
      reply.Put<int32_t>(0);
      reply.Put<uint32_t>(count);
      for (const auto& group : groups) {
        g_rights_groups.erase({group->token[0], group->token[1]});
        DeliverRights(client, group.get(), &reply);
      }
    } else {
      break;
    }
    if (!WriteMessage(pipe, reply)) break;
  }
}

int DoIoctl(Proc* proc, Thread* thread, uint32_t request, In in, Out* out) {
  switch (request) {
    case BINDER_SET_MAX_THREADS: {
      uint32_t max;
      if (!in.Get(&max)) return -EINVAL;
      proc->max_threads = max;
      return 0;
    }
    case BINDER_SET_CONTEXT_MGR:
    case BINDER_SET_CONTEXT_MGR_EXT: {
      if (g_peer_registry && (!proc->identity ||
                              proc->identity->role != peer::Role::kServiceManager)) return -EPERM;
      flat_binder_object fbo = {};
      if (request == BINDER_SET_CONTEXT_MGR_EXT && !in.Get(&fbo)) return -EINVAL;
      if (g_context_mgr_node != nullptr) {
        Log("proc %u: BINDER_SET_CONTEXT_MGR already set", proc->id);
        return -EBUSY;
      }
      Node* node = GetNode(proc, fbo.binder);
      if (node == nullptr) node = NewNode(proc, fbo.binder, fbo.cookie, fbo.flags);
      node->local_weak_refs++;
      node->local_strong_refs++;
      node->has_strong_ref = true;
      node->has_weak_ref = true;
      g_context_mgr_node = node;
      TRACE("proc %u: context manager", proc->id);
      return 0;
    }
    case BINDER_THREAD_EXIT:
      TRACE("proc %u thread %d: exit", proc->id, thread->tid);
      ThreadRelease(thread);
      return 0;
    case BINDER_GET_NODE_INFO_FOR_REF: {
      binder_node_info_for_ref info;
      if (!in.Get(&info)) return -EINVAL;
      Ref* ref = GetRef(proc, info.handle, false);
      if (ref == nullptr) return -EINVAL;
      info.strong_count = ref->node->local_strong_refs + ref->node->internal_strong_refs;
      info.weak_count = ref->node->local_weak_refs + static_cast<uint32_t>(ref->node->refs.size());
      out->Put(info);
      return 0;
    }
    default:
      return -EINVAL;
  }
}

void ServeThread(HANDLE pipe, In in) {
  uint32_t version, proc_id, tid;
  uint64_t key;
  if (!in.Get(&version) || version != wire::kVersion || !in.Get(&proc_id) || !in.Get(&key) ||
      !in.Get(&tid)) {
    return;
  }
  std::shared_ptr<Thread> thread_owner;
  std::shared_ptr<Proc> proc_owner;
  const auto identity = g_peer_registry ? g_peer_registry->Authenticate(pipe)
                                        : peer::Registry::Lease{};
  Thread* thread = nullptr;
  Proc* proc = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_procs.find(proc_id);
    if (it != g_procs.end() && it->second->key == key && !it->second->is_dead &&
        (!g_peer_registry || (identity && it->second->identity == identity &&
                              g_peer_registry->Live(identity)))) {
      proc_owner = it->second;
      proc = proc_owner.get();
      thread_owner = std::make_shared<Thread>();
      thread = thread_owner.get();
      thread->proc = proc_owner;
      thread->tid = static_cast<int>(tid);
      proc->threads.push_back(thread);
    }
  }
  WireReply hello;
  hello.Put<int32_t>(thread != nullptr ? 0 : -ESRCH);
  if (!WriteMessage(pipe, hello) || thread == nullptr) {
    if (thread != nullptr) {
      std::lock_guard<std::mutex> lock(g_lock);
      ThreadRelease(thread);
    }
    return;
  }

  std::vector<uint8_t> msg;
  while (ReadMessage(pipe, &msg)) {
    if (g_peer_registry && g_peer_registry->Authenticate(pipe) != identity) break;
    In req{msg.data(), msg.data() + msg.size()};
    uint32_t op;
    if (!req.Get(&op)) break;
    WireReply reply;
    std::unique_lock<std::mutex> lock(g_lock);
    if (thread->is_dead || proc->is_dead) {
      lock.unlock();
      reply.Put<int32_t>(-EBADF);
      reply.Put<uint32_t>(0);
      if (!WriteMessage(pipe, reply)) break;
      continue;
    }
    if (op == wire::kWriteRead) {
      uint64_t read_size;
      uint32_t first, non_block;
      if (!req.Get(&read_size) || !req.Get(&first) || !req.Get(&non_block)) break;
      uint32_t consumed = 0;
      int ret = thread->pending_buffer == 0 ? ThreadWrite(proc, thread, req, &consumed) : -EINVAL;
      ReadStream rs;
      rs.limit = static_cast<size_t>(read_size);
      if (ret == 0 && read_size > 0) {
        ret = ThreadRead(lock, proc, thread, &rs, first != 0, non_block != 0);
        if (!proc->todo.empty()) WakeProc(proc);
      }
      thread->looper_need_return = false;
      lock.unlock();
      reply.Put<int32_t>(ret);
      reply.Put<uint32_t>(consumed);
      reply.Write(rs.out.bytes.data(), rs.out.bytes.size());
    } else if (op == wire::kClaimBuffer || op == wire::kFinishBuffer) {
      uint64_t id;
      uint32_t accepted = 0;
      if (!req.Get(&id) || (op == wire::kFinishBuffer && !req.Get(&accepted))) break;
      int ret = 0;
      if (op == wire::kClaimBuffer) {
        if (id == 0 || id != thread->pending_buffer) ret = -EINVAL;
        else thread->pending_buffer_claimed = true;
      } else {
        ret = FinishBuffer(thread, id, accepted != 0);
      }
      lock.unlock();
      reply.Put<int32_t>(ret);
    } else if (op == wire::kIoctl) {
      uint32_t request;
      if (!req.Get(&request)) break;
      Out arg;
      int ret = DoIoctl(proc, thread, request, req, &arg);
      thread->looper_need_return = false;
      lock.unlock();
      reply.Put<int32_t>(ret);
      reply.Write(arg.bytes.data(), arg.bytes.size());
    } else {
      break;
    }
    if (!WriteMessage(pipe, reply)) break;
  }
  std::lock_guard<std::mutex> lock(g_lock);
  ThreadRelease(thread);
}

bool ReadStandaloneRequest(HANDLE pipe, std::vector<uint8_t>* request) {
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  uint32_t length = 0;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) ||
      !launch::Transfer(pipe, &length, sizeof(length), false, deadline) ||
      length < 2 * sizeof(uint32_t) || length > 256) return false;
  request->resize(length);
  return launch::Transfer(pipe, request->data(), length, false, deadline);
}

void Serve(HANDLE pipe) {
  std::vector<uint8_t> msg;
  if (g_record_only ? ReadStandaloneRequest(pipe, &msg) : ReadMessage(pipe, &msg)) {
    In in{msg.data(), msg.data() + msg.size()};
    uint32_t op, magic;
    if (in.Get(&op) && in.Get(&magic) && magic == wire::kMagic) {
      if (op == wire::kEnrollChild) {
        ServeEnrollChild(pipe, in);
#ifdef GESHER_RECORD_LOCK_BROKER
      } else if (op == wire::kFileRecordLock || op == wire::kFileRecordClose) {
        ServeFileRecordLock(pipe, in, op == wire::kFileRecordClose);
      } else if (op == wire::kAudioPlaybackPublish || op == wire::kAudioPlaybackQuery ||
                 op == wire::kAudioPlaybackSession || op == wire::kAudioDeviceControls) {
        ServeAudioPlayback(pipe, in, op);
#ifdef GESHER_AUDIO_RENDER_BROKER
      } else if (op == wire::kAudioRender) {
        ServeAudioRender(pipe, in);
      } else if (op == wire::kAudioCapture) {
        ServeAudioCapture(pipe, in);
#endif
#ifdef GESHER_CAMERA_BROKER_HOST
      } else if (op == wire::kCamera) {
        ServeCamera(pipe, in);
#endif
#endif
      } else if (!g_record_only) {
        if (op == wire::kHelloProc || op == wire::kHelloOperator) {
          ServeProc(pipe, in, op == wire::kHelloOperator);
        } else if (op == wire::kHelloThread) {
          ServeThread(pipe, in);
        } else if (op == wire::kHelloRights) {
          ServeRights(pipe, in);
        }
      }
    }
  }
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
}

}  // namespace

int main(int argc, char** argv) {
  const char* trace = getenv("GESHER_BINDER_TRACE");
  g_trace = trace != nullptr && trace[0] == '1';
  launch::Startup bootstrap;
  g_record_only = gesher::binder_bootstrap::ReadStartupRecord(&bootstrap) &&
      bootstrap.endpoint == launch::Endpoint::kRecordBroker;
  if (g_record_only && !launch::ReadStartup(launch::Endpoint::kRecordBroker, &bootstrap)) {
    return EXIT_FAILURE;
  }
#ifndef GESHER_RECORD_LOCK_BROKER
  if (g_record_only) return EXIT_FAILURE;
#endif
  const bool protected_launch = g_record_only || launch::ReadStartup(launch::Endpoint::kBroker, &bootstrap);
  std::string device;
  if (g_record_only) {
    device = "standalone record locks";
  } else if (argc > 1) {
    device = argv[1];
  } else if (const char* env = getenv("GESHER_BINDER"); env != nullptr && env[0] != '\0') {
    device = env;
  } else {
    device = "default";
  }
  std::wstring name;
  if (g_record_only) {
    name = wire::kStandaloneRecordPipe + std::to_wstring(GetCurrentProcessId());
  } else {
    name = wire::kPipePrefix;
    for (char c : device) name += static_cast<wchar_t>(static_cast<unsigned char>(c));

    // The descriptor broker's doorbell and the stream positions (binder_wire.h),
    // which live as long as the broker does. The positions are the processes'
    // own; the broker only keeps the section.
    std::wstring doorbell = wire::kRightsDoorbellPrefix;
    std::wstring positions = wire::kStreamPositionsPrefix;
    for (char c : device) {
      doorbell += static_cast<wchar_t>(static_cast<unsigned char>(c));
      positions += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    constexpr DWORD kDoorbellBytes = wire::kRightsDoorbellSlots * sizeof(LONG64);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        kDoorbellBytes, doorbell.c_str());
    if (section != nullptr) {
      g_rights_doorbell = static_cast<volatile LONG64*>(
          MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, kDoorbellBytes));
    }
    HANDLE positions_section = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        wire::kRightsDoorbellSlots * sizeof(wire::StreamPositions), positions.c_str());
    if (g_rights_doorbell == nullptr || positions_section == nullptr) {
      Log("descriptor broker doorbell or stream positions unavailable: %lu", GetLastError());
    }
  }

  // GESHER_SANDBOX=appcontainer (ADR 0090): an explicit descriptor that admits the
  // app processes' AppContainers through the binder-client capability and a Low
  // label, and nothing else beyond this user and SYSTEM. Otherwise the default.
  SECURITY_ATTRIBUTES pipe_security{sizeof(pipe_security), nullptr, FALSE};
  if (g_record_only) {
    PSID sid = nullptr;
    if (argc != 2 || !ConvertStringSidToSidA(argv[1], &sid)) return EXIT_FAILURE;
    const bool copied = g_record_container.CopyFrom(sid);
    const DWORD error = gesher::appcontainer::AppContainerPipeSecurity(&pipe_security.lpSecurityDescriptor, sid);
    LocalFree(sid);
    if (!copied || error != ERROR_SUCCESS) {
      Log("standalone record lock pipe security unavailable: %lu", error);
      return EXIT_FAILURE;
    }
  }
  if (protected_launch) {
    gesher::process_identity::Process process;
    const HANDLE endpoint = launch::DecodeHandle(bootstrap.channel);
    ULONG launcher_pid = 0;
    if (!gesher::process_identity::ReadLiveProcess(GetCurrentProcess(), &process) ||
        process.principal.container.size != 0 || process.principal.restricted ||
        process.principal.integrity < SECURITY_MANDATORY_MEDIUM_RID ||
        !GetNamedPipeClientProcessId(endpoint, &launcher_pid) ||
        !SetHandleInformation(endpoint, HANDLE_FLAG_INHERIT, 0)) {
      Log("invalid protected launch authority");
      return EXIT_FAILURE;
    }
    g_peer_registry = std::make_unique<peer::Registry>(process.principal);
    std::thread(ServeLaunchAuthority, endpoint, launcher_pid, process.principal).detach();
  }
  g_sandbox = g_peer_registry || gesher::appcontainer::Enabled();
  if (g_sandbox && !g_record_only) {
    DWORD error = gesher::appcontainer::BinderPipeSecurity(&pipe_security.lpSecurityDescriptor);
    if (error != ERROR_SUCCESS) {
      Log("GESHER_SANDBOX=appcontainer: no pipe security descriptor: %lu", error);
      return 1;
    }
  }

  bool first = true;
  for (;;) {
    HANDLE pipe = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0,
        g_sandbox ? &pipe_security : nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      if (first && (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY)) {
        Log("device %s is already served", device.c_str());
        return 2;
      }
      Log("CreateNamedPipe failed: %lu", error);
      return 1;
    }
    if (first) {
      // Logged once the pipe exists: a first write to stderr can be slow, and
      // clients wait for the pipe, not for the log.
      if (g_sandbox) Log("GESHER_SANDBOX=appcontainer: app processes run in AppContainers (experimental)");
      Log("serving device %s", device.c_str());
      fflush(stderr);
    }
    first = false;
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
      CloseHandle(pipe);
      continue;
    }
    std::thread(Serve, pipe).detach();
  }
}
