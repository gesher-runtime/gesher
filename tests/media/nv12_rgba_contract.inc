// Actual camera NV12 -> reader/window delivery, with an independent integer
// color oracle. No synthetic input reaches ordinary application behavior.
#include <array>
#include <ctime>
#include <string>

Array Nv12Input(int w, int h, uint32_t seed) {
  Array input;
  // Odd source extents are internal fallback probes only: include the complete
  // addressable old chroma rows. Real NV12 camera extents are positive and even.
  input.bytes.resize(static_cast<size_t>(w)*h + static_cast<size_t>(w)*((h+1)/2) + (w%2));
  for (auto& byte : input.bytes) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    byte = static_cast<jbyte>(seed & 255);
  }
  return input;
}

// Division is explicitly floored, independently of the implementation's signed
// right shift. All 16,777,216 Y/U/V triples are checked in ExhaustiveColors.
uint8_t Nv12Channel(int64_t numerator) {
  const int64_t rounded = numerator >= 0 ? numerator / 256 : -((-numerator + 255) / 256);
  return static_cast<uint8_t>(std::max<int64_t>(0, std::min<int64_t>(255, rounded)));
}
std::array<uint8_t,4> Nv12Color(int y, int u, int v) {
  const int64_t luma = static_cast<int64_t>(y) * 298 - 4768;
  return {Nv12Channel(luma + static_cast<int64_t>(v)*409 - 52224),
          Nv12Channel(luma - static_cast<int64_t>(u)*100 - static_cast<int64_t>(v)*208 + 39552),
          Nv12Channel(luma + static_cast<int64_t>(u)*516 - 65920), 255};
}
std::vector<uint8_t> Nv12Expected(const Array& input, int w, int h, int tw, int th,
                                int stride, size_t guard = 0) {
  std::vector<uint8_t> output(guard + static_cast<size_t>(stride)*th*4 + guard, 0xcd);
  const auto* src = reinterpret_cast<const uint8_t*>(input.bytes.data());
  for (int y=0; y<th; ++y) {
    const size_t sy = static_cast<size_t>(static_cast<int64_t>(y)*h/th);
    for (int x=0; x<tw; ++x) {
      const size_t sx = static_cast<size_t>(static_cast<int64_t>(x)*w/tw);
      const size_t chroma = static_cast<size_t>(w)*h + (sy/2)*w + (sx/2)*2;
      const auto pixel = Nv12Color(src[sy*w+sx],src[chroma],src[chroma+1]);
      std::copy(pixel.begin(),pixel.end(),output.begin()+guard+(static_cast<size_t>(y)*stride+x)*4);
    }
  }
  return output;
}
int Nv12Push(Target& target, Array& input, int w, int h, int64_t timestamp, jbyteArray jpeg=nullptr) {
  return CameraFrames_deliver(&target.env,0,&target.surface,&input,w,h,timestamp,jpeg);
}
void Nv12Metadata(const Target& target, const Image& image, int64_t timestamp) {
  Check(image.width == target.reader->width && image.height == target.reader->height &&
        image.timestampNs == timestamp && image.dataSpace == target.reader->dataSpace &&
        image.halFormat == 1, "NV12 reader dimensions timestamp and dataspace");
  Check(image.planes.size() == 1 && image.planes[0].offset == 0 &&
        image.planes[0].size == image.data.size() && image.planes[0].rowStride == image.width*4 &&
        image.planes[0].pixelStride == 4, "NV12 reader exact RGBA plane layout");
}
void Nv12ReaderCases() {
  const std::array<int,4> cases[] = {
    {2,2,2,2}, {2,6,2,6}, {6,2,6,2}, {16,8,16,8}, {258,130,258,130},
    {4,8,4,4}, {8,4,4,4}, {4,4,8,8}, {16,8,7,5}, {16,8,1,1},
    {16,8,17,9}, {16,8,16,7}, {16,8,15,8}, {2,2,0,0}
  };
  for (const auto& dims : cases) {
    const auto [w,h,tw,th] = dims;
    auto input = Nv12Input(w,h,0x98765432);
    const auto saved = input.bytes;
    const auto expected = Nv12Expected(input,w,h,tw,th,tw);
    Target target(tw,th,1);
    Check(Nv12Push(target,input,w,h,1234567) == OK, "NV12 valid reader delivery status");
    Image* image = target.Acquire();
    Check(image != nullptr, "NV12 delivered image can be acquired");
    if (!image) continue;
    Check(image->data == expected, "NV12 complete RGBA pixel oracle");
    Nv12Metadata(target,*image,1234567);
    Check(input.bytes == saved, "NV12 input remains unchanged");
    if (!image->data.empty()) image->data[0] ^= 0xff;
    Check(input.bytes == saved, "NV12 delivered RGBA storage is independently writable");
    ReleaseImage(target.reader,image);
    Check(target.env.events == 1 && target.env.pins == 1 && target.env.releases == 1 &&
          target.reader->acquired == 0 && target.env.local_refs == 0,
          "NV12 reader callback pins and acquisitions retire");
  }
}

struct Nv12WindowTarget {
  JNIEnv env;
  Object surface;
  ANativeWindow window;
  Nv12WindowTarget(int w, int h, int padding) : window(w,h,padding) {
    Check(nv12_window == nullptr, "NV12 fixture window has one owner");
    nv12_window = &window; nv12_window_surface = &surface;
  }
  ~Nv12WindowTarget() {
    Check(window.references == 0 && env.pins == env.releases, "NV12 window pins and native references retire");
    nv12_window = nullptr; nv12_window_surface = nullptr;
  }
  int Push(Array& input, int w, int h) {
    return CameraFrames_deliver(&env,0,&surface,&input,w,h,7654321,nullptr);
  }
};
void Nv12WindowCases() {
  const std::array<int,5> cases[] = {
    {2,2,2,2,1}, {16,8,16,8,13}, {258,130,258,130,7},
    {16,8,7,5,3}, {8,4,16,8,5}, {16,8,16,7,1}
  };
  for (const auto& dims : cases) {
    const auto [w,h,tw,th,padding] = dims;
    auto input = Nv12Input(w,h,0x12345678);
    const auto saved = input.bytes;
    Nv12WindowTarget target(tw,th,padding);
    const auto expected = Nv12Expected(input,w,h,tw,th,target.window.stride,target.window.guard);
    Check(target.Push(input,w,h) == OK, "NV12 valid window delivery status");
    Check(target.window.storage == expected, "NV12 padded window pixel and guard oracle");
    Check(input.bytes == saved, "NV12 window delivery retains producer bytes");
    Check(target.window.geometries == 1 && target.window.locks == 1 && target.window.posts == 1,
          "NV12 window is posted exactly once");
  }
  for (int failure=0; failure<3; ++failure) {
    auto input = Nv12Input(16,8,31);
    Nv12WindowTarget target(16,8,7);
    const int code = -120-failure;
    if (failure == 0) target.window.geometry_error = code;
    if (failure == 1) target.window.lock_error = code;
    if (failure == 2) target.window.post_error = code;
    Check(target.Push(input,16,8) == code, "NV12 original window failure is returned");
    Check(target.window.locks == (failure ? 1 : 0) && target.window.posts == (failure==2 ? 1 : 0),
          "NV12 failure cannot invent window publication");
    if (failure != 2)
      Check(std::all_of(target.window.storage.begin(),target.window.storage.end(),
                       [](uint8_t byte) { return byte == 0xcd; }), "NV12 failed window keeps backing storage");
  }
  // The dispatcher clips to the acquired native buffer's extent.
  {
    auto input = Nv12Input(16,8,41);
    Nv12WindowTarget target(16,8,5);
    target.window.buffer_width = 12; target.window.buffer_height = 6;
    auto expected = Nv12Expected(input,16,8,12,6,target.window.stride,target.window.guard);
    expected.resize(target.window.storage.size(),0xcd);
    Check(target.Push(input,16,8) == OK && target.window.storage == expected,
          "NV12 window buffer extent remains bounded");
  }
  // Odd source extents only exercise the pre-existing internal scalar fallback.
  for (const auto& dims : {std::array<int,2>{1,1}, {3,5}, {7,9}}) {
    const auto [w,h] = dims;
    auto input = Nv12Input(w,h,13);
    auto expected = Nv12Expected(input,w,h,w,h,w+3,17);
    std::vector<uint8_t> actual(expected.size(),0xcd);
    Nv12ToRgba(reinterpret_cast<const uint8_t*>(input.bytes.data()),w,h,actual.data()+17,w+3,w,h);
    Check(actual == expected, "NV12 odd source internal fallback remains byte identical");
  }
}

void Nv12CloseRace() {
#ifndef NV12_RGBA_TIMING
  Target target(16,8,1);
  auto input = Nv12Input(16,8,71);
  std::promise<void> converting, closed;
  auto started = converting.get_future(); auto finished = closed.get_future();
  std::thread closer([&] { started.wait(); target.Close(); closed.set_value(); });
  bool fired = false;
  allocation_hook = [&](size_t bytes) {
    if (!fired && bytes == 16*8*4) { fired = true; converting.set_value(); finished.wait(); }
  };
  const int result = Nv12Push(target,input,16,8,1);
  allocation_hook = nullptr; closer.join();
  Check(fired && result == NO_INIT && target.reader->pending.empty() && target.env.events == 0,
        "NV12 close racing conversion cannot publish");
  // Release an injected mutant's ownership only after the intended assertion.
  for (Image* image : target.reader->pending) delete image;
  target.reader->pending.clear();
#endif
}
void Nv12LifetimeCases() {
  Target target(16,8,1,2);
  auto input = Nv12Input(16,8,91);
  Check(Nv12Push(target,input,16,8,1) == OK, "NV12 initial lifetime delivery");
  auto* held = target.Acquire();
  Check(held != nullptr, "NV12 held image exists");
  if (!held) return;
  const auto saved = held->data;
  for (int n=2; n<=7; ++n) {
    input = Nv12Input(16,8,n);
    Check(Nv12Push(target,input,16,8,n) == OK, "NV12 producer reuse delivers");
  }
  Check(held->data == saved && target.reader->pending.size() == 2,
        "NV12 acquired bytes survive bounded queue replacement");
  auto* next = target.Acquire();
  Check(next && next->timestampNs == 6, "NV12 pending FIFO retains the last two frames");
  Object blocked;
  Check(ImageReader_imageSetup(&target.env,&target.java_reader,&blocked) == 2,
        "NV12 maxImages remains enforced");
  if (next) ReleaseImage(target.reader,next);
  auto* last = target.Acquire();
  Check(last && last->timestampNs == 7, "NV12 last surviving frame has original timestamp");
  if (last) ReleaseImage(target.reader,last);
  Check(held->data == saved, "NV12 separate release cannot alter acquired image");
  ReleaseImage(target.reader,held);
  target.Close();
  Check(Nv12Push(target,input,16,8,8) == NO_INIT && target.reader->pending.empty(),
        "NV12 closed surface cannot receive another frame");
  for (int mode=0; mode<3; ++mode) {
    Target callback(16,8,1);
    bool nested = false;
    if (mode == 0) callback.env.event_hook = [&] { callback.Close(); };
    if (mode == 1) callback.env.fail_event = true;
    if (mode == 2) callback.env.event_hook = [&] {
      if (!nested) { nested = true; Check(Nv12Push(callback,input,16,8,12) == OK,"NV12 recursive callback delivery"); }
    };
    const int result = Nv12Push(callback,input,16,8,11);
    Check(result == OK, "NV12 callback delivery retains original native status");
    if (mode == 0) Check(callback.reader->closed && callback.reader->pending.empty(),"NV12 callback may close reader");
    if (mode == 1) Check(callback.env.exception,"NV12 callback exception remains pending for Java");
    if (mode == 2) {
      auto* first = callback.Acquire(); auto* second = callback.Acquire();
      Check(first && second && first->timestampNs == 11 && second->timestampNs == 12,
            "NV12 callback reentry retains distinct timestamped images");
      if (first) ReleaseImage(callback.reader,first);
      if (second) ReleaseImage(callback.reader,second);
    }
  }
}
void Nv12OtherFormatsAndErrors() {
  auto input = Nv12Input(16,8,97);
  {
    Target target(16,8,1);
    Array short_frame;
    short_frame.bytes.resize(16*8*3/2-1);
    Check(CameraFrames_deliver(&target.env,0,nullptr,&input,16,8,1,nullptr) == -1 &&
          CameraFrames_deliver(&target.env,0,&target.surface,nullptr,16,8,1,nullptr) == -1 &&
          Nv12Push(target,short_frame,16,8,1) == -1 && target.env.pins == 0,
          "NV12 absent or short input fails before array pinning");
  }
  {
    Target jpeg(16,8,0x21);
    Array encoded; encoded.bytes = {1,2,3,4,5};
    Check(Nv12Push(jpeg,input,16,8,1) == 1 && jpeg.reader->pending.empty(),"NV12 absent JPEG remains unsupported");
    Check(Nv12Push(jpeg,input,16,8,2,&encoded) == OK,"NV12 explicit JPEG is delivered");
    auto* image = jpeg.Acquire();
    Check(image && image->data == std::vector<uint8_t>({1,2,3,4,5}) &&
          image->planes.size() == 1 && image->planes[0].rowStride == 0,
          "NV12 JPEG path preserves supplied encoded bytes");
    if (image) ReleaseImage(jpeg.reader,image);
  }
  for (int format : {0x22,0x23}) {
    Target planar(16,8,format);
    Check(Nv12Push(planar,input,16,8,3) == OK,"NV12 planar/PRIVATE fallback delivers");
    auto* image = planar.Acquire();
    Check(image && image->data.size() == input.bytes.size() && image->planes.size() == 3,
          "NV12 planar/PRIVATE retains I420 layout");
    if (image) {
      const auto* src = reinterpret_cast<const uint8_t*>(input.bytes.data());
      bool equal = std::memcmp(image->data.data(),src,128) == 0;
      for (int n=0; n<32; ++n) equal &= image->data[128+n] == src[128+2*n] && image->data[160+n] == src[129+2*n];
      Check(equal,"NV12 planar/PRIVATE chroma remains exact");
      ReleaseImage(planar.reader,image);
    }
  }
}

void Nv12ExhaustiveColors() {
  constexpr int width = 4096, height = 4096;
  Array input; input.bytes.resize(static_cast<size_t>(width)*height*3/2);
  auto* src = reinterpret_cast<uint8_t*>(input.bytes.data());
  auto* uv = src + static_cast<size_t>(width)*height;
  for (uint32_t block=0; block<(1u<<22); ++block) {
    const int x = static_cast<int>(block % (width/2))*2;
    const int y = static_cast<int>(block / (width/2))*2;
    const uint32_t chroma = block/64;
    uv[static_cast<size_t>(y/2)*width+x] = static_cast<uint8_t>(chroma >> 8);
    uv[static_cast<size_t>(y/2)*width+x+1] = static_cast<uint8_t>(chroma);
    const uint8_t luma = static_cast<uint8_t>((block%64)*4);
    const size_t offset = static_cast<size_t>(y)*width+x;
    src[offset] = luma; src[offset+1] = luma+1;
    src[offset+width] = luma+2; src[offset+width+1] = luma+3;
  }
  Target target(width,height,1);
  Check(Nv12Push(target,input,width,height,123) == OK,"NV12 exhaustive delivery status");
  auto* image = target.Acquire();
  Check(image != nullptr,"NV12 exhaustive image acquired");
  if (!image) return;
  Nv12Metadata(target,*image,123);
  std::vector<uint8_t> seen(1u<<21,0);
  uint32_t unique = 0;
  for (int y=0; y<height; ++y) for (int x=0; x<width; ++x) {
    const size_t at = static_cast<size_t>(y)*width+x;
    const size_t chroma = static_cast<size_t>(y/2)*width+(x/2)*2;
    const uint8_t luma = src[at], u = uv[chroma], v = uv[chroma+1];
    const uint32_t triple = (static_cast<uint32_t>(luma)<<16) | (static_cast<uint32_t>(u)<<8) | v;
    const uint8_t mask = static_cast<uint8_t>(1u<<(triple&7));
    if (!(seen[triple/8]&mask)) { seen[triple/8] |= mask; ++unique; }
    const auto expected = Nv12Color(luma,u,v);
    Check(std::memcmp(image->data.data()+at*4,expected.data(),4) == 0,
          "NV12 exhaustive independent color oracle");
  }
  Check(unique == (1u<<24) && std::all_of(seen.begin(),seen.end(),[](uint8_t b){return b==255;}),
        "NV12 corpus includes every Y U V triple exactly once");
  ReleaseImage(target.reader,image);
  std::printf("NV12_RGBA_EXHAUSTIVE triples=%u\n",unique);
}

#if defined(NV12_RGBA_TIMING) && !defined(_WIN32)
struct Nv12Workload { const char* name; int w,h,tw,th,frames,padding; bool window; };
constexpr Nv12Workload nv12_workloads[] = {
  {"minimum",2,2,2,2,30000,0,false}, {"tiny",16,8,16,8,12000,0,false},
  {"720p",1280,720,1280,720,24,0,false}, {"1080p",1920,1080,1920,1080,12,0,false},
  {"4k",3840,2160,3840,2160,3,0,false}, {"window_1080p",1920,1080,1920,1080,12,7,true},
  {"scaled_down_control",258,130,129,65,512,0,false}, {"scaled_up_control",128,64,256,128,128,0,false},
  {"odd_target_control",258,130,257,129,128,0,false}, {"empty_control",2,2,0,0,30000,0,false}
};
uint64_t Nv12Clock(clockid_t clock) {
  timespec value{}; if (clock_gettime(clock,&value)) std::abort();
  return static_cast<uint64_t>(value.tv_sec)*1000000000u+value.tv_nsec;
}
uint64_t Nv12Cycle(const Nv12Workload& work, int frames, std::array<Array,2>& input,
                   const std::array<std::vector<uint8_t>,2>& expected) {
  uint64_t checksum = 0;
  if (work.window) {
    Nv12WindowTarget target(work.tw,work.th,work.padding);
    for (int n=0; n<frames; ++n) {
      Check(target.Push(input[n%2],work.w,work.h) == OK,"timed NV12 native window delivery");
      Check(target.window.storage == expected[n%2],"timed NV12 complete padded window oracle");
      checksum += target.window.storage.size()+target.window.storage[17]+n+1;
    }
    Check(target.window.geometries == frames && target.window.locks == frames && target.window.posts == frames,
          "timed NV12 all native window publications retire");
  } else {
    Target target(work.tw,work.th,1);
    for (int n=0; n<frames; ++n) {
      Check(Nv12Push(target,input[n%2],work.w,work.h,n+1) == OK,"timed NV12 complete reader delivery");
      auto* image = target.Acquire();
      Check(image != nullptr,"timed NV12 image acquired");
      if (!image) continue;
      Check(image->data == expected[n%2],"timed NV12 full pixel oracle");
      Nv12Metadata(target,*image,n+1);
      checksum += image->data.size()+image->timestampNs;
      if (!image->data.empty()) checksum += image->data.front()+image->data.back();
      ReleaseImage(target.reader,image);
    }
    Check(target.env.events == frames && target.env.pins == frames && target.env.releases == frames &&
          target.reader->pending.empty() && target.reader->acquired == 0 && target.env.local_refs == 0,
          "timed NV12 complete reader retirement");
  }
  Check(gReaders.empty() && nv12_window == nullptr,"timed NV12 native owners destroyed");
  return checksum;
}
int Nv12Benchmark(const char* name) {
  const Nv12Workload* work = nullptr;
  for (const auto& candidate : nv12_workloads) if (!std::strcmp(candidate.name,name)) work=&candidate;
  if (!work) return 2;
  std::array<Array,2> input{Nv12Input(work->w,work->h,0x31415926),Nv12Input(work->w,work->h,0x27182818)};
  const size_t guard = work->window ? 17 : 0;
  const int stride = work->tw+work->padding;
  std::array<std::vector<uint8_t>,2> expected{
    Nv12Expected(input[0],work->w,work->h,work->tw,work->th,stride,guard),
    Nv12Expected(input[1],work->w,work->h,work->tw,work->th,stride,guard)};
  for (int phase=0; phase<2; ++phase) {
    const int frames = phase ? work->frames : 1, previous = checks;
    const uint64_t wall_start=Nv12Clock(CLOCK_MONOTONIC), cpu_start=Nv12Clock(CLOCK_PROCESS_CPUTIME_ID);
    const uint64_t checksum=Nv12Cycle(*work,frames,input,expected);
    const uint64_t cpu_ns=Nv12Clock(CLOCK_PROCESS_CPUTIME_ID)-cpu_start, wall_ns=Nv12Clock(CLOCK_MONOTONIC)-wall_start;
    std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"frames\":%d,\"cpu_ns\":%llu,\"wall_ns\":%llu,"
                "\"checksum\":%llu,\"checks\":%d,\"failures\":%d}\n",name,phase?"steady":"first",frames,
                (unsigned long long)cpu_ns,(unsigned long long)wall_ns,(unsigned long long)checksum,
                checks-previous,failures);
  }
  return failures ? 1 : 0;
}
#endif
int main(int argc, char** argv) {
#if defined(NV12_RGBA_TIMING) && !defined(_WIN32)
  if (argc == 3 && !std::strcmp(argv[1],"--bench")) return Nv12Benchmark(argv[2]);
#endif
  Nv12CloseRace(); Nv12WindowCases(); Nv12ReaderCases(); Nv12LifetimeCases(); Nv12OtherFormatsAndErrors();
  if (argc != 2 || std::strcmp(argv[1],"--quick")) Nv12ExhaustiveColors();
  Check(gReaders.empty() && nv12_window == nullptr,"NV12 all native owners retire");
  std::printf("NV12_RGBA_PASS checks=%d failures=%d\n",checks,failures);
  return failures ? 1 : 0;
}
