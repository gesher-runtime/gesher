// Production ownership is shared with the maintained codec-delivery fixture.
#include <array>
#include <string>
#include <ctime>

#ifndef IMAGE_READER_RGBA_TIMING
thread_local uint64_t rgba_copy_calls = 0, rgba_copy_bytes = 0;
void* ObserveRgbaCopy(void* destination, const void* source, size_t bytes) {
  ++rgba_copy_calls;
  rgba_copy_bytes += bytes;
  return std::memcpy(destination, source, bytes);
}
#define memcpy ObserveRgbaCopy
#endif
namespace android { namespace {
@QUEUE_RGBA@
} }
#ifndef IMAGE_READER_RGBA_TIMING
#undef memcpy
#endif

std::vector<uint32_t> RgbaPixels(int width, int height, uint32_t seed) {
  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
  for (auto& pixel : pixels) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    pixel = seed;
  }
  return pixels;
}

std::vector<uint32_t> RgbaOracle(const std::vector<uint32_t>& pixels,
                               int width, int height, int tw, int th) {
  std::vector<uint32_t> expected(static_cast<size_t>(tw) * th);
  for (int y = 0; y < th; ++y)
    for (int x = 0; x < tw; ++x)
      expected[static_cast<size_t>(y) * tw + x] = pixels[
          static_cast<size_t>(static_cast<int64_t>(y) * height / th) * width +
          static_cast<int64_t>(x) * width / tw];
  return expected;
}

void RgbaPush(Target& target, const uint32_t* pixels, int width, int height, int64_t timestamp) {
  current_env = &target.env;
  QueueRgba(target.reader, pixels, width, height, timestamp);
}

void RgbaPixelCases() {
  const std::array<int,4> cases[] = {
    {1,1,1,1}, {1,7,1,7}, {7,1,7,1}, {3,5,3,5}, {7,9,7,9},
    {17,13,17,13}, {64,32,64,32}, {257,129,257,129},
    {4,8,4,4}, {8,4,4,4}, {3,4,4,3}, {2,2,7,9}, {7,9,2,2},
    {0,0,0,0}, {0,5,0,5}
  };
  for (const auto& dims : cases) {
    const auto [w,h,tw,th] = dims;
    reader_vm = JavaVM{};
    Target target(tw,th,1);
    auto input = RgbaPixels(w,h,0x12345678);
    const auto saved = input;
    const auto expected = RgbaOracle(input,w,h,tw,th);
#ifndef IMAGE_READER_RGBA_TIMING
    rgba_copy_calls = rgba_copy_bytes = 0;
#endif
    RgbaPush(target,input.data(),w,h,4567);
    auto* image = target.Acquire();
    Check(image != nullptr, "RGBA queued image can be acquired");
    if (!image) continue;
    Check(image->data.size() == expected.size()*4, "RGBA exact owned storage extent");
    for (size_t i=0; i<expected.size(); ++i) {
      uint32_t actual; std::memcpy(&actual,image->data.data()+i*4,4);
      Check(actual == expected[i], "RGBA pixels match independent scaling oracle");
    }
    Check(image->width == tw && image->height == th && image->timestampNs == 4567 &&
          image->halFormat == 1 && image->dataSpace == target.reader->dataSpace,
          "RGBA metadata survives delivery");
    Check(image->planes.size() == 1 && image->planes[0].offset == 0 &&
          image->planes[0].size == expected.size()*4 &&
          image->planes[0].rowStride == tw*4 && image->planes[0].pixelStride == 4,
          "RGBA plane remains tightly packed");
    Check(input == saved, "RGBA producer input is immutable during delivery");
#ifndef IMAGE_READER_RGBA_TIMING
    const bool copied = !baseline && w == tw && h == th && !expected.empty();
    Check(rgba_copy_calls == (copied ? 1u : 0u), "matching extent uses one bounded bulk copy");
    Check(rgba_copy_bytes == (copied ? expected.size()*4 : 0), "RGBA copy byte extent is exact");
#endif
    if (!input.empty() && !image->data.empty()) {
      image->data[0] ^= 0xff;
      Check(input == saved, "RGBA acquired image has independent writable storage");
    }
    ReleaseImage(target.reader,image);
    Check(target.env.events == 1 && target.env.local_refs == 0 && target.reader->acquired == 0,
          "RGBA callback and local references retire");
  }
}

void RgbaDrainUnexpected(Target& target) {
  // Clean injected ownership bugs after their assertion; leak diagnostics must
  // not substitute for the intended negative-control failure.
  for (Image* image : target.reader->pending) delete image;
  target.reader->pending.clear();
}

void RgbaCloseRace() {
#ifndef IMAGE_READER_RGBA_TIMING
  reader_vm = JavaVM{};
  Target target(16,8,1);
  const auto input = RgbaPixels(16,8,7);
  std::promise<void> converting, closed;
  auto started = converting.get_future(); auto finished = closed.get_future();
  std::thread closer([&] { started.wait(); target.Close(); closed.set_value(); });
  bool fired = false;
  allocation_hook = [&](size_t bytes) {
    if (!fired && bytes == 16*8*4) { fired=true; converting.set_value(); finished.wait(); }
  };
  RgbaPush(target,input.data(),16,8,1);
  allocation_hook = nullptr; closer.join();
  Check(fired && target.reader->pending.empty() && target.env.events == 0,
        "RGBA close racing conversion cannot publish");
  RgbaDrainUnexpected(target);
#endif
}

void RgbaLifetimeCases() {
  reader_vm = JavaVM{};
  Target target(16,8,1,2);
  auto input = RgbaPixels(16,8,17);
  RgbaPush(target,input.data(),16,8,1);
  auto* held = target.Acquire();
  Check(held != nullptr, "RGBA first image acquired");
  if (!held) return;
  const auto saved = held->data;
  for (int i=2; i<=7; ++i) {
    input = RgbaPixels(16,8,static_cast<uint32_t>(i));
    RgbaPush(target,input.data(),16,8,i);
  }
  Check(held->data == saved && target.reader->pending.size() == 2,
        "RGBA held image survives producer reuse and bounded pending replacement");
  auto* next = target.Acquire();
  Check(next && next->timestampNs == 6, "RGBA pending FIFO drops only oldest images");
  Object blocked;
  Check(ImageReader_imageSetup(&target.env,&target.java_reader,&blocked) == 2,
        "RGBA acquired maximum remains enforced");
  if (next) ReleaseImage(target.reader,next);
  auto* last = target.Acquire();
  Check(last && last->timestampNs == 7, "RGBA latest surviving timestamp retained");
  if (last) ReleaseImage(target.reader,last);
  target.Close();
  // Internal native storage ownership only: Android forbids public Image access
  // after ImageReader.close(); this assertion does not extend that API contract.
  Check(held->data == saved, "RGBA acquired storage lives until explicit release");
  ReleaseImage(target.reader,held);
  const int events = target.env.events;
  RgbaPush(target,input.data(),16,8,8);
  Check(target.reader->pending.empty() && target.env.events == events,
        "RGBA closed owner has no new callback or image");
  RgbaDrainUnexpected(target);
}

void RgbaCallbackCases() {
  const auto input = RgbaPixels(16,8,29);
  for (int mode=0; mode<4; ++mode) {
    reader_vm = JavaVM{};
    Target target(16,8,1);
    reader_vm.detached = mode == 1 || mode == 2;
    reader_vm.fail_attach = mode == 2;
    reader_vm.empty_environment = mode == 3;
    RgbaPush(target,input.data(),16,8,99);
    const bool delivered = mode < 2;
    Check(target.env.events == (delivered ? 1 : 0), "RGBA JNI attachment failure preserves no delivery");
    Check(reader_vm.lookups == 1 && reader_vm.attaches == ((mode==1 || mode==2) ? 1 : 0) &&
          reader_vm.detaches == (mode==1 ? 1 : 0), "RGBA JNI attachment ownership retained");
    if (delivered) {
      auto* image = target.Acquire();
      Check(image && image->timestampNs == 99, "RGBA attached delivery retains timestamp");
      if (image) ReleaseImage(target.reader,image);
    }
  }
  reader_vm = JavaVM{};
  std::weak_ptr<Reader> expired;
  {
    Target target(16,8,1);
    expired = target.reader;
  }
  QueueRgba(expired,input.data(),16,8,100);
  Check(reader_vm.lookups == 0, "RGBA expired weak owner never attaches");
  {
    Target target(16,8,1);
    target.env.event_hook = [&] { target.Close(); };
    RgbaPush(target,input.data(),16,8,101);
    Check(target.reader->closed && target.reader->pending.empty(),
          "RGBA callback may close after publication");
  }
  {
    Target target(16,8,1);
    bool nested = false;
    target.env.event_hook = [&] {
      if (!nested) { nested=true; RgbaPush(target,input.data(),16,8,103); }
    };
    RgbaPush(target,input.data(),16,8,102);
    auto* first = target.Acquire(); auto* second = target.Acquire();
    Check(nested && first && second && first->timestampNs == 102 && second->timestampNs == 103,
          "RGBA callback reentry retains both independently owned frames");
    if (first) ReleaseImage(target.reader,first);
    if (second) ReleaseImage(target.reader,second);
  }
}

#if defined(IMAGE_READER_RGBA_TIMING) && !defined(_WIN32)
struct RgbaWorkload { const char* name; int w, h, tw, th, iterations; };
constexpr RgbaWorkload rgba_workloads[] = {
  {"single_pixel",1,1,1,1,60000}, {"tiny",16,8,16,8,20000},
  {"odd",257,129,257,129,256}, {"720p",1280,720,1280,720,32},
  {"1080p",1920,1080,1920,1080,16}, {"4k",3840,2160,3840,2160,4},
  {"scaled_control",257,129,129,67,512}, {"empty_control",0,5,0,5,60000}
};
uint64_t RgbaClock(clockid_t clock) {
  timespec value{};
  if (clock_gettime(clock,&value)) std::abort();
  return static_cast<uint64_t>(value.tv_sec)*1000000000u + value.tv_nsec;
}
uint64_t RgbaCycle(const RgbaWorkload& work, int frames,
                   const std::array<std::vector<uint32_t>,2>& input,
                   const std::array<std::vector<uint32_t>,2>& expected) {
  uint64_t checksum = 0;
  {
    Target target(work.tw,work.th,1);
    for (int i=0; i<frames; ++i) {
      const auto& pixels = input[i%2]; const auto& oracle = expected[i%2];
      RgbaPush(target,pixels.data(),work.w,work.h,i+1);
      Image* image = target.Acquire();
      Check(image != nullptr,"timed RGBA image acquired");
      if (!image) continue;
      Check(image->data.size() == oracle.size()*sizeof(uint32_t) &&
            (oracle.empty() || !std::memcmp(image->data.data(),oracle.data(),image->data.size())),
            "timed RGBA full pixel oracle");
      Check(image->width == work.tw && image->height == work.th && image->halFormat == 1 &&
            image->timestampNs == i+1 && image->dataSpace == target.reader->dataSpace &&
            image->planes.size() == 1 && image->planes[0].offset == 0 &&
            image->planes[0].size == oracle.size()*sizeof(uint32_t) &&
            image->planes[0].rowStride == work.tw*4 && image->planes[0].pixelStride == 4,
            "timed RGBA public metadata");
      checksum += image->timestampNs + image->data.size();
      if (!image->data.empty()) checksum += image->data.front()+image->data.back();
      ReleaseImage(target.reader,image);
    }
    Check(target.env.events == frames && target.reader->acquired == 0 &&
          target.reader->pending.empty(),"timed RGBA full queue retirement");
  }
  Check(gReaders.empty(),"timed RGBA native reader retirement");
  return checksum;
}
int RgbaBenchmark(const char* name) {
  const RgbaWorkload* work = nullptr;
  for (const auto& candidate : rgba_workloads)
    if (!std::strcmp(candidate.name,name)) work = &candidate;
  if (!work) return 2;
  // Inputs and an independent scaling oracle are prepared outside measurement.
  // Both observed phases include native owner creation, full pixel comparison,
  // every queue/acquire/release, and owner close/destruction. JNI is the declared
  // fixture adapter; this does not time ART, Canvas, IPC or a physical producer.
  std::array<std::vector<uint32_t>,2> input{
    RgbaPixels(work->w,work->h,0x31415926), RgbaPixels(work->w,work->h,0x27182818)};
  std::array<std::vector<uint32_t>,2> expected{
    RgbaOracle(input[0],work->w,work->h,work->tw,work->th),
    RgbaOracle(input[1],work->w,work->h,work->tw,work->th)};
  reader_vm = JavaVM{};
  for (int phase=0; phase<2; ++phase) {
    const int frames = phase ? work->iterations : 1;
    const int checks_before = checks;
    const uint64_t wall_start = RgbaClock(CLOCK_MONOTONIC);
    const uint64_t cpu_start = RgbaClock(CLOCK_PROCESS_CPUTIME_ID);
    const uint64_t checksum = RgbaCycle(*work,frames,input,expected);
    const uint64_t cpu_ns = RgbaClock(CLOCK_PROCESS_CPUTIME_ID)-cpu_start;
    const uint64_t wall_ns = RgbaClock(CLOCK_MONOTONIC)-wall_start;
    std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"frames\":%d,"
                "\"cpu_ns\":%llu,\"wall_ns\":%llu,\"checksum\":%llu,"
                "\"checks\":%d,\"failures\":%d}\n",name,phase?"steady":"first",frames,
                (unsigned long long)cpu_ns,(unsigned long long)wall_ns,
                (unsigned long long)checksum,checks-checks_before,failures);
  }
  return failures ? 1 : 0;
}
#endif

int main(int argc, char** argv) {
#if defined(IMAGE_READER_RGBA_TIMING) && !defined(_WIN32)
  if (argc == 3 && !std::strcmp(argv[1],"--bench")) return RgbaBenchmark(argv[2]);
#endif
  baseline = argc > 1 && std::strcmp(argv[1],"--baseline") == 0;
  RgbaCloseRace(); RgbaPixelCases(); RgbaLifetimeCases(); RgbaCallbackCases();
  Check(gReaders.empty(), "RGBA all native reader owners retired");
  std::printf("RGBA_READER_PASS checks=%d failures=%d\n",checks,failures);
  return failures ? 1 : 0;
}
