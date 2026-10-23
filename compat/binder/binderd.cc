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
// docs/decisions/0014-services-binder-ipc.md.
//
//   binderd [device]      (default: $GESHER_BINDER, else "default")
//
// serves \\.\pipe\gesher-binder-<device> until it is killed. It exits with
// code 2 if another binderd already serves the device. GESHER_BINDER_TRACE=1
// traces every command on stderr.
//
// Not done: file descriptors in transactions (BINDER_TYPE_FD, FDA), scatter-
// gather buffers (BINDER_TYPE_PTR, used by HIDL), security contexts
// (BR_TRANSACTION_SEC_CTX), freezing, priority inheritance, polling; such
// transactions fail with BR_FAILED_REPLY.
#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <linux/android/binder.h>

#include "../posix/src/binder_wire.h"

namespace {

namespace wire = gesher::binder_wire;

bool g_trace = false;

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
};

struct Transaction : Work {
  int debug_id = 0;
  Thread* from = nullptr;
  int from_pid = 0;
  Transaction* from_parent = nullptr;
  Proc* to_proc = nullptr;
  Thread* to_thread = nullptr;
  Transaction* to_parent = nullptr;
  bool need_reply = false;
  uint32_t code = 0;
  uint32_t flags = 0;
  uint32_t sender_euid = 0;
  Buffer* buffer = nullptr;
  Transaction() : Work(WorkType::kTransaction) {}
};

struct Thread {
  Proc* proc = nullptr;
  int tid = 0;
  uint32_t looper = 0;
  bool looper_need_return = true;
  bool is_dead = false;
  bool waiting = false;  // on proc->waiting_threads
  bool process_todo = false;
  Transaction* transaction_stack = nullptr;
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
// releases it. Procs and threads are never freed (transactions may still
// name them after they die); nodes, refs, transactions and buffers are.
std::mutex g_lock;
std::map<uint32_t, Proc*> g_procs;
Node* g_context_mgr_node = nullptr;
uint32_t g_next_proc_id = 1;
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
    Thread* target_thread = t->from;
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
    if (type != BINDER_TYPE_BINDER && type != BINDER_TYPE_WEAK_BINDER &&
        type != BINDER_TYPE_HANDLE && type != BINDER_TYPE_WEAK_HANDLE) {
      Log("proc %u: object type %08x is not supported", proc->id, type);
      return BR_FAILED_REPLY;
    }
    if (off + kFlatObjectSize > buffer->data.size()) return BR_FAILED_REPLY;
    flat_binder_object fp;
    memcpy(&fp, buffer->data.data() + off, sizeof(fp));
    if (type == BINDER_TYPE_BINDER || type == BINDER_TYPE_WEAK_BINDER) {
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
    if (in_reply_to->to_thread != thread) {
      Log("proc %u thread %d: reply to a transaction of another thread", proc->id, thread->tid);
      in_reply_to = nullptr;
      return fail(BR_FAILED_REPLY);
    }
    thread->transaction_stack = in_reply_to->to_parent;
    target_thread = in_reply_to->from;
    if (target_thread == nullptr || target_thread->is_dead) return fail(BR_DEAD_REPLY);
    if (target_thread->transaction_stack != in_reply_to) {
      Log("proc %u: reply out of order", proc->id);
      in_reply_to = nullptr;
      return fail(BR_FAILED_REPLY);
    }
    target_proc = target_thread->proc;
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
      if (tmp->to_thread != thread) {
        Log("proc %u thread %d: new transaction not at the top of its stack", proc->id,
            thread->tid);
        return fail(BR_FAILED_REPLY);
      }
      // A nested call goes to the thread of the target process that waits
      // for this thread's (or an outer) call.
      while (tmp != nullptr) {
        Thread* from = tmp->from;
        if (from != nullptr && from->proc == target_proc) {
          target_thread = from;
          break;
        }
        tmp = tmp->from_parent;
      }
    }
  }
  if (td.buffers_size != 0) {
    Log("proc %u: scatter-gather buffers are not supported", proc->id);
    return fail(BR_FAILED_REPLY);
  }

  Transaction* t = new Transaction;
  t->debug_id = g_next_debug_id++;
  t->from = (!reply && (tr.flags & TF_ONE_WAY) == 0) ? thread : nullptr;
  t->from_pid = proc->pid;
  t->sender_euid = proc->uid;
  t->to_proc = target_proc;
  t->to_thread = target_thread;
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

// --- thread and process release ---------------------------------------------------

// binder_thread_release
void ThreadRelease(Thread* thread) {
  if (thread->is_dead) return;
  Proc* proc = thread->proc;
  proc->threads.remove(thread);
  if (thread->waiting) {
    proc->waiting_threads.remove(thread);
    thread->waiting = false;
  }
  Transaction* t = thread->transaction_stack;
  Transaction* send_reply = nullptr;
  if (t != nullptr && t->to_thread == thread) send_reply = t;
  thread->is_dead = true;
  while (t != nullptr) {
    if (t->to_thread == thread) {
      t->to_proc = nullptr;
      t->to_thread = nullptr;
      t = t->to_parent;
    } else if (t->from == thread) {
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
    const uint8_t* b = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), b, b + n);
  }
  template <typename T>
  void Put(const T& v) {
    Write(&v, sizeof(T));
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
        if (td.tr.offsets_size % sizeof(binder_size_t) != 0) return -EINVAL;
        td.data.resize(td.tr.data_size);
        td.offsets.resize(td.tr.offsets_size / sizeof(binder_size_t));
        if (!in.Read(td.data.data(), td.data.size()) ||
            !in.Read(td.offsets.data(), td.tr.offsets_size)) {
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
    rs->Put32(cmd);
    rs->out.Put(tr);
    rs->size += sizeof(tr);
    rs->out.Put(buffer->id);
    rs->out.Write(buffer->data.data(), buffer->data.size());
    rs->out.Write(buffer->offsets.data(), tr.offsets_size);
    TRACE("proc %u thread %d: %s %d delivered", proc->id, thread->tid,
          cmd == BR_REPLY ? "reply" : "transaction", t->debug_id);
    if (cmd != BR_REPLY && (t->flags & TF_ONE_WAY) == 0) {
      t->to_parent = thread->transaction_stack;
      t->to_thread = thread;
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

bool WriteMessage(HANDLE pipe, const Out& out) {
  uint32_t len = static_cast<uint32_t>(out.bytes.size());
  std::vector<uint8_t> buf(sizeof(len) + out.bytes.size());
  memcpy(buf.data(), &len, sizeof(len));
  if (!out.bytes.empty()) memcpy(buf.data() + sizeof(len), out.bytes.data(), out.bytes.size());
  return WriteExact(pipe, buf.data(), buf.size());
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

void ServeProc(HANDLE pipe, In in) {
  uint32_t version, pid, uid;
  if (!in.Get(&version) || version != wire::kVersion || !in.Get(&pid) || !in.Get(&uid)) return;
  Proc* proc = new Proc;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    proc->id = g_next_proc_id++;
    proc->key = RandomKey();
    proc->pid = static_cast<int>(pid);
    proc->uid = uid;
    g_procs[proc->id] = proc;
  }
  TRACE("proc %u: pid %u opened the device", proc->id, pid);
  Out reply;
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
  Thread* thread = nullptr;
  Proc* proc = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_procs.find(proc_id);
    if (it != g_procs.end() && it->second->key == key && !it->second->is_dead) {
      proc = it->second;
      thread = new Thread;
      thread->proc = proc;
      thread->tid = static_cast<int>(tid);
      proc->threads.push_back(thread);
    }
  }
  Out hello;
  hello.Put<int32_t>(thread != nullptr ? 0 : -ESRCH);
  if (!WriteMessage(pipe, hello) || thread == nullptr) return;

  std::vector<uint8_t> msg;
  while (ReadMessage(pipe, &msg)) {
    In req{msg.data(), msg.data() + msg.size()};
    uint32_t op;
    if (!req.Get(&op)) break;
    Out reply;
    std::unique_lock<std::mutex> lock(g_lock);
    if (thread->is_dead || proc->is_dead) {
      lock.unlock();
      reply.Put<int32_t>(-EBADF);
      if (!WriteMessage(pipe, reply)) break;
      continue;
    }
    if (op == wire::kWriteRead) {
      uint64_t read_size;
      uint32_t first, non_block;
      if (!req.Get(&read_size) || !req.Get(&first) || !req.Get(&non_block)) break;
      uint32_t consumed = 0;
      int ret = ThreadWrite(proc, thread, req, &consumed);
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

void Serve(HANDLE pipe) {
  std::vector<uint8_t> msg;
  if (ReadMessage(pipe, &msg)) {
    In in{msg.data(), msg.data() + msg.size()};
    uint32_t op, magic;
    if (in.Get(&op) && in.Get(&magic) && magic == wire::kMagic) {
      if (op == wire::kHelloProc) {
        ServeProc(pipe, in);
      } else if (op == wire::kHelloThread) {
        ServeThread(pipe, in);
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
  std::string device;
  if (argc > 1) {
    device = argv[1];
  } else if (const char* env = getenv("GESHER_BINDER"); env != nullptr && env[0] != '\0') {
    device = env;
  } else {
    device = "default";
  }
  std::wstring name = wire::kPipePrefix;
  for (char c : device) name += static_cast<wchar_t>(static_cast<unsigned char>(c));

  bool first = true;
  for (;;) {
    HANDLE pipe = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
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
