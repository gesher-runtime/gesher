// Complete production InputSurface and QueueSurface on Windows. The only JNI
// boundary is Surface wrapper construction below, which exposes the producer
// without starting ART. The Linux runner declares its additional queue adapter.
#include "compat/media/surface/queue_surface.h"
#include "compat/media/video/input_surface.h"
#include <android_runtime/android_view_Surface.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using gesher::media::InputSurface;
using gesher::media::QueueSurface;
using namespace std::chrono_literals;
thread_local android::sp<QueueSurface> captured_producer;
std::atomic<int> checks{0}, failures{0};

bool Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL input surface callbacks: %s\n", reason);
  }
  return ok;
}

class Signal {
 public:
  void Set() {
    std::lock_guard<std::mutex> lock(mu_);
    ready_ = true;
    cv_.notify_all();
  }
  bool Wait(std::chrono::milliseconds timeout = 5s) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [this] { return ready_; });
  }
 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool ready_ = false;
};

void Require(Signal& signal, const char* reason) {
  if (!Check(signal.Wait(), reason)) {
    // An incorrect wait implementation must fail the bounded process rather
    // than leaving a test thread to access objects after fixture teardown.
    std::fflush(stderr);
    std::_Exit(2);
  }
}

struct Input {
  std::unique_ptr<InputSurface> consumer{gesher::media::CreateInputSurface(2, 2)};
  android::sp<QueueSurface> producer;
  Input() {
    consumer->NewJavaSurface(nullptr);
    producer = captured_producer;
    captured_producer = nullptr;
    if (!Check(producer != nullptr, "new input exposes its producer")) std::_Exit(2);
  }
};

bool Post(QueueSurface* surface) {
  ANativeWindow_Buffer buffer = {};
  if (surface->lock(&buffer, nullptr) != 0) return false;
  static_cast<uint32_t*>(buffer.bits)[0] = 0xff123456;
  return surface->unlockAndPost() == 0;
}

void NormalAndReplacement() {
  Input in;
  Check(Post(in.producer.get()), "a frame can precede callback registration");
  int first = 0, second = 0;
  in.consumer->SetCallback([&](const uint32_t* pixels, int width, int height, int64_t time) {
    ++first;
    Check(width == 2 && height == 2 && pixels[0] == 0xff123456,
          "callback receives the actual frame geometry and pixels");
    Check(time == in.producer->Latest()->timestamp_ns,
          "callback receives the published frame timestamp");
  });
  Check(Post(in.producer.get()) && first == 1, "registered callback runs once");
  in.consumer->SetCallback([&](const uint32_t*, int, int, int64_t) { ++second; });
  Check(Post(in.producer.get()) && first == 1 && second == 1,
        "replacement handles later frames only");
  in.consumer->SetCallback(nullptr);
  Check(Post(in.producer.get()) && second == 1, "clearing callback leaves the producer usable");
  Check(in.consumer->frames() == 4, "all produced frames are counted");
}

void Quiescence(bool destroy) {
  Input in;
  Signal entered, permit_return, retired, retire_started;
  std::atomic<bool> active{false};
  in.consumer->SetCallback([&](const uint32_t*, int, int, int64_t) {
    active = true;
    entered.Set();
    Require(permit_return, "callback is allowed to finish");
    active = false;
  });
  std::thread producer([&] { Check(Post(in.producer.get()), "in-flight frame completes"); });
  Require(entered, "ordinary callback entered");
  std::thread retirement([&] {
    retire_started.Set();
    if (destroy) in.consumer.reset();
    else in.consumer->Abandon();
    Check(!active.load(), "retirement cannot complete while a callback is active");
    retired.Set();
  });
  Require(retire_started, "retirement thread started");
  Check(!retired.Wait(100ms), "retirement waits for the admitted callback");
  permit_return.Set();
  Require(retired, "retirement completes after the admitted callback returns");
  producer.join();
  retirement.join();
  Check(!Post(in.producer.get()), "retired producer rejects a later frame");
  if (!destroy) {
    in.consumer->Abandon();
    Check(!Post(in.producer.get()), "repeated abandonment remains retired");
    in.consumer->SetCallback([](const uint32_t*, int, int, int64_t) {
      Check(false, "callback replacement must not revive an abandoned input");
    });
    Check(!Post(in.producer.get()), "replacement cannot revive abandoned producer");
  }
}

void ReplacementWaits() {
  Input in;
  Signal entered, permit_return, replacing, replaced;
  std::atomic<bool> active{false};
  int replacements = 0;
  in.consumer->SetCallback([&](const uint32_t*, int, int, int64_t) {
    active = true;
    entered.Set();
    Require(permit_return, "old callback is allowed to return");
    active = false;
  });
  std::thread producer([&] { Check(Post(in.producer.get()), "old callback's frame completes"); });
  Require(entered, "old callback is admitted");
  std::thread replacement([&] {
    replacing.Set();
    in.consumer->SetCallback([&](const uint32_t*, int, int, int64_t) { ++replacements; });
    Check(!active.load(), "replacement waits for old callback receiver use");
    replaced.Set();
  });
  Require(replacing, "replacement thread started");
  Check(!replaced.Wait(100ms), "replacement is quiescent with its previous callback");
  permit_return.Set();
  Require(replaced, "replacement completes after callback return");
  producer.join();
  replacement.join();
  Check(Post(in.producer.get()) && replacements == 1, "new callback receives the next frame");
}

void ReentrantOperations() {
  Input in;
  int first = 0, second = 0;
  auto capture = std::make_shared<int>(7);
  std::weak_ptr<int> observed = capture;
  in.consumer->SetCallback([&, capture](const uint32_t*, int, int, int64_t) {
    ++first;
    in.consumer->SetCallback([&](const uint32_t*, int, int, int64_t) { ++second; });
    Check(!observed.expired(), "replaced callback captures survive its current invocation");
    Check(*capture == 7, "current callback can still use its captures after replacement");
  });
  capture.reset();
  Check(Post(in.producer.get()) && first == 1, "callback can replace itself without deadlock");
  Check(observed.expired(), "replaced captures release after callback return");
  Check(Post(in.producer.get()) && second == 1, "reentrant replacement handles future frames");
  in.consumer->SetCallback([&](const uint32_t*, int, int, int64_t) {
    in.consumer->Abandon();
    in.consumer->Abandon();
    Check(!Post(in.producer.get()), "callback abandonment retires producer immediately");
    in.consumer->SetCallback([](const uint32_t*, int, int, int64_t) {
      Check(false, "reentrant replacement after abandonment stays retired");
    });
  });
  Check(Post(in.producer.get()), "callback can abandon its own input without deadlock");
  Check(!Post(in.producer.get()), "reentrantly abandoned producer stays retired");
}

void ExceptionUnlocks() {
  Input in;
  in.consumer->SetCallback([](const uint32_t*, int, int, int64_t) {
    throw std::runtime_error("declared callback boundary");
  });
  bool caught = false;
  try { Post(in.producer.get()); } catch (const std::runtime_error&) { caught = true; }
  Check(caught, "callback exception reaches its caller");
  Signal retired;
  std::thread retirement([&] { in.consumer->Abandon(); retired.Set(); });
  Require(retired, "exception releases callback admission lock");
  retirement.join();
  Check(!Post(in.producer.get()), "exceptional callback can still be retired");
}

#ifdef GESHER_PERSISTENT_INPUT_TEST
struct PersistentInput {
  std::shared_ptr<gesher::media::PersistentInputSurface> source{
      gesher::media::CreatePersistentInputSurface()};
  android::sp<QueueSurface> producer;
  PersistentInput() {
    source->NewJavaSurface(nullptr);
    producer = captured_producer;
    captured_producer = nullptr;
    if (!Check(producer != nullptr, "persistent source exposes its producer")) std::_Exit(2);
  }
};

void PersistentReuse() {
  PersistentInput in;
  Check(Post(in.producer.get()), "unattached persistent producer accepts a frame");
  Check(in.source->Attach(0, 2) == nullptr, "invalid dimensions cannot consume the lease");
  auto first = in.source->Attach(2, 2);
  Check(first != nullptr && first->persistent(), "first consumer holds a persistent lease");
  Check(in.source->Attach(2, 2) == nullptr, "persistent producer has at most one consumer");
  int calls = 0;
  first->SetCallback([&](const uint32_t*, int, int, int64_t) { ++calls; });
  Check(Post(in.producer.get()) && calls == 1, "first encoder receives its frame");
  first->Abandon();
  Check(Post(in.producer.get()) && calls == 1, "detached producer remains usable without callbacks");
  for (int session = 0; session < 1000; ++session) {
    const int width = 2 + session % 3, height = 3 + session % 2;
    auto next = in.source->Attach(width, height);
    if (!Check(next != nullptr, "successive session acquires the original producer")) std::_Exit(2);
    next->NewJavaSurface(nullptr);
    Check(captured_producer.get() == in.producer.get(), "producer identity survives encoder replacement");
    captured_producer = nullptr;
    const int before = calls;
    next->SetCallback([&](const uint32_t* pixels, int w, int h, int64_t timestamp) {
      ++calls;
      Check(w == width && h == height && pixels[0] == 0xff123456,
            "successor receives its configured geometry and real pixels");
      Check(timestamp == in.producer->Latest()->timestamp_ns, "successor preserves the frame timestamp");
    });
    // Repeated close/callback replacement on the first lease must not affect
    // any later generation, even while that retired object is still alive.
    first->Abandon();
    first->SetCallback([](const uint32_t*, int, int, int64_t) {
      Check(false, "retired lease cannot register a callback on a successor");
    });
    Check(Post(in.producer.get()) && calls == before + 1,
          "stale consumer cannot detach or replace its successor");
  }
  Check(first->frames() == 1003, "producer frame count remains continuous across sessions");
  first.reset();
  Check(Post(in.producer.get()) && calls == 1001, "last consumer destruction leaves source live");
  in.source.reset();
  Check(!Post(in.producer.get()), "last source release abandons the producer");
}

void PersistentQuiescence() {
  PersistentInput in;
  auto lease = in.source->Attach(2, 2);
  Signal entered, permit_return, retiring, retired;
  std::atomic<bool> active{false};
  lease->SetCallback([&](const uint32_t*, int, int, int64_t) {
    active = true;
    entered.Set();
    Require(permit_return, "persistent callback may complete");
    active = false;
  });
  std::thread producer([&] { Check(Post(in.producer.get()), "persistent in-flight frame completes"); });
  Require(entered, "persistent callback admitted");
  std::thread consumer([&] {
    retiring.Set();
    lease->Abandon();
    Check(!active, "persistent retirement waits for admitted callback");
    auto next = in.source->Attach(3, 2);
    Check(next != nullptr, "next owner attaches after quiescence");
    retired.Set();
  });
  Require(retiring, "persistent retirement started");
  Check(!retired.Wait(100ms), "new owner cannot overtake old callback shutdown");
  permit_return.Set();
  Require(retired, "persistent retirement completes");
  producer.join(); consumer.join();
  Check(Post(in.producer.get()), "quiescence preserves persistent producer");
}

void PersistentReentrantAndReleasedSource() {
  PersistentInput in;
  auto lease = in.source->Attach(2, 2);
  std::unique_ptr<InputSurface> next;
  auto capture = std::make_shared<int>(19);
  std::weak_ptr<int> observed = capture;
  int calls = 0;
  lease->SetCallback([&, capture](const uint32_t*, int, int, int64_t) {
    lease->Abandon();
    next = in.source->Attach(2, 2);
    Check(next != nullptr, "callback may install successor without owner lock inversion");
    next->SetCallback([&](const uint32_t*, int, int, int64_t) { ++calls; });
    lease.reset();
    Check(!observed.expired() && *capture == 19, "retired invocation retains its captures");
  });
  capture.reset();
  Check(Post(in.producer.get()), "reentrant retirement completes");
  Check(observed.expired(), "retired invocation releases captures after returning");
  in.source.reset();
  Check(Post(in.producer.get()) && calls == 1,
        "admitted native lease survives application source release");
  next.reset();
  Check(!Post(in.producer.get()), "last native lease release retires an otherwise released source");
}
#endif

#ifdef GESHER_INPUT_SURFACE_HOST
void StaleListener() {
  Input in;
  int calls = 0;
  in.consumer->SetCallback([&](const uint32_t*, int, int, int64_t) { ++calls; });
  Check(Post(in.producer.get()) && calls == 1, "initial callback completed");
  // Model a producer that copied QueueSurface's listener before Abandon, but
  // has not invoked it yet. No destroyed object is dereferenced by this test.
  auto listener = in.producer->SnapshotListener();
  in.consumer->Abandon();
  listener();
  Check(calls == 1, "already copied listener cannot admit a callback after abandonment");
  in.consumer->SetCallback([&](const uint32_t*, int, int, int64_t) { ++calls; });
  listener();
  Check(calls == 1, "callback replacement cannot reactivate a copied retired listener");
  in.consumer.reset();
  listener();
  Check(calls == 1, "copied listener safely retains only retired shared state");
}

#ifdef GESHER_PERSISTENT_INPUT_TEST
void StalePersistentListener() {
  PersistentInput in;
  auto lease = in.source->Attach(2, 2);
  int old_calls = 0, new_calls = 0;
  lease->SetCallback([&](const uint32_t*, int, int, int64_t) { ++old_calls; });
  Check(Post(in.producer.get()), "old generation published a frame");
  auto old_listener = in.producer->SnapshotListener();
  lease.reset();
  auto next = in.source->Attach(2, 2);
  next->SetCallback([&](const uint32_t*, int, int, int64_t) { ++new_calls; });
  old_listener();
  Check(old_calls == 1 && new_calls == 0, "copied old listener cannot deliver a frame to the new owner");
  Check(Post(in.producer.get()) && new_calls == 1, "new generation receives only its own publication");
  in.source.reset(); next.reset();
  old_listener();
  Check(old_calls == 1 && new_calls == 1, "copied persistent listener safely outlives all owners");
}
#endif
#endif
}  // namespace

namespace android {
jobject android_view_Surface_createFromSurface(JNIEnv*, const sp<Surface>& surface) {
  captured_producer = static_cast<QueueSurface*>(surface.get());
  return nullptr;
}
}  // namespace android

int main() {
  NormalAndReplacement();
  Quiescence(false);
  Quiescence(true);
  ReplacementWaits();
  ReentrantOperations();
  ExceptionUnlocks();
#ifdef GESHER_PERSISTENT_INPUT_TEST
  PersistentReuse();
  PersistentQuiescence();
  PersistentReentrantAndReleasedSource();
#endif
#ifdef GESHER_INPUT_SURFACE_HOST
  StaleListener();
#ifdef GESHER_PERSISTENT_INPUT_TEST
  StalePersistentListener();
#endif
#endif
  std::printf("input surface callback contracts: %d checks, %d failures\n",
              checks.load(), failures.load());
  return failures != 0;
}
