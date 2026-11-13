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
#ifdef GESHER_INPUT_SURFACE_HOST
  StaleListener();
#endif
  std::printf("input surface callback contracts: %d checks, %d failures\n",
              checks.load(), failures.load());
  return failures != 0;
}
