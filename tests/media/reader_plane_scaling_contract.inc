// The complete actual delivery methods precede this fixture. Timing builds
// use ordinary allocation; explicit host adapters do not claim ART or decoding.
#include <ctime>
#include <string>

namespace {
struct ScaleCase {
  const char* name;
  int width, height, target_width, target_height, crop, padding, format, frames;
  bool closed = false;
};
constexpr ScaleCase kScaleCases[] = {
  {"tiny_scaled",8,8,3,5,1,3,0x23,4000},
  {"down_half_1080",1920,1080,960,540,0,0,0x23,16},
  {"down_noninteger_1080",1920,1080,1280,720,1,7,0x23,16},
  {"up_double_720",640,360,1280,720,0,0,0x23,16},
  {"up_noninteger_720",1280,720,1920,1080,3,7,0x23,12},
  {"down_4k",3840,2160,1920,1080,0,0,0x23,12},
  {"odd_target",1280,720,641,361,3,7,0x22,24},
  {"identity_720",1280,720,1280,720,0,0,0x23,24},
  {"identity_padded",1280,720,1280,720,1,7,0x23,24},
  {"rgba_control",640,360,320,180,1,7,1,24},
  {"closed_control",16,8,7,5,1,3,0x23,4000,true},
};

std::vector<uint8_t> ExpectedPlanes(const VideoFrame& frame, int width, int height, int format) {
  const int source_width = frame.visible_width() & ~1;
  const int source_height = frame.visible_height() & ~1;
  if (IsRgbaFormat(format)) {
    std::vector<uint8_t> result(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
      const int sy = static_cast<int>(static_cast<int64_t>(y) * source_height / height);
      for (int x = 0; x < width; ++x) {
        const int sx = static_cast<int>(static_cast<int64_t>(x) * source_width / width);
        const int c = frame.y()[static_cast<size_t>(frame.crop_top + sy) * frame.stride_y + frame.crop_left + sx] - 16;
        const size_t chroma = static_cast<size_t>(frame.crop_top / 2 + sy / 2) * frame.stride_uv + frame.crop_left / 2 + sx / 2;
        const int d = frame.u()[chroma] - 128, e = frame.v()[chroma] - 128;
        auto* out = result.data() + (static_cast<size_t>(y) * width + x) * 4;
        out[0] = static_cast<uint8_t>(std::clamp((298*c+409*e+128)>>8,0,255));
        out[1] = static_cast<uint8_t>(std::clamp((298*c-100*d-208*e+128)>>8,0,255));
        out[2] = static_cast<uint8_t>(std::clamp((298*c+516*d+128)>>8,0,255));
        out[3] = 255;
      }
    }
    return result;
  }
  const size_t y_bytes = static_cast<size_t>(width) * height;
  const int cw = (width + 1) / 2, ch = (height + 1) / 2;
  const size_t c_bytes = static_cast<size_t>(cw) * ch;
  std::vector<uint8_t> result(y_bytes + 2 * c_bytes);
  for (int p = 0; p < 3; ++p) {
    const int dw = p ? cw : width, dh = p ? ch : height;
    const int sw = p ? source_width / 2 : source_width;
    const int sh = p ? source_height / 2 : source_height;
    const int stride = p ? frame.stride_uv : frame.stride_y;
    const int left = p ? frame.crop_left / 2 : frame.crop_left;
    const int top = p ? frame.crop_top / 2 : frame.crop_top;
    const auto* source = (p == 0 ? frame.y() : p == 1 ? frame.u() : frame.v()) + static_cast<size_t>(top) * stride + left;
    const size_t offset = p == 0 ? 0 : y_bytes + (p - 1) * c_bytes;
    for (int y = 0; y < dh; ++y)
      for (int x = 0; x < dw; ++x)
        result[offset + static_cast<size_t>(y) * dw + x] =
            source[static_cast<size_t>(static_cast<int64_t>(y) * sh / dh) * stride +
                   static_cast<int64_t>(x) * sw / dw];
  }
  return result;
}

void PlaneCoordinates(bool quick) {
  const int limit = quick ? 17 : 128;
  for (int width = 1; width <= limit; ++width) {
    const int height = 7, stride = width + 11;
    std::vector<uint8_t> source(static_cast<size_t>(stride) * height + 16,0xcd);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        source[static_cast<size_t>(y) * stride + x] = Pattern(x,y,0,11);
    const auto before = source;
    for (int target_width = 1; target_width <= limit; ++target_width) {
      for (int target_height : {1,3,7,11}) {
        const size_t size = static_cast<size_t>(target_width) * target_height;
        std::vector<uint8_t> target(size + 32,0xa7);
        CopyReaderPlane(source.data(),stride,width,height,target.data()+16,target_width,target_height);
        bool same = true;
        for (int y = 0; y < target_height; ++y)
          for (int x = 0; x < target_width; ++x) {
            const size_t row = static_cast<size_t>(static_cast<int64_t>(y)*height/target_height)*stride;
            same &= target[16+static_cast<size_t>(y)*target_width+x] ==
                source[row+static_cast<int64_t>(x)*width/target_width];
          }
        Check(same,"plane scaler exact floor-coordinate oracle");
        Check(std::all_of(target.begin(),target.begin()+16,[](uint8_t x){return x==0xa7;}) &&
              std::all_of(target.end()-16,target.end(),[](uint8_t x){return x==0xa7;}),
              "plane scaler writes only the owned output extent");
        Check(source==before,"plane scaler never changes input or its row padding");
        if (failures) return;
      }
    }
  }
  for (int target_width : {0,-1}) {
    uint8_t source[8]{}, output[16]; std::memset(output,0xa7,sizeof(output));
    CopyReaderPlane(source,4,4,2,output,target_width,2);
    Check(std::all_of(std::begin(output),std::end(output),[](uint8_t x){return x==0xa7;}),
          "empty horizontal output retains the old no-write behavior");
  }
  for (int target_height : {0,-1}) {
    uint8_t source[8]{}, output[16]; std::memset(output,0xa7,sizeof(output));
    CopyReaderPlane(source,4,4,2,output,3,target_height);
    Check(std::all_of(std::begin(output),std::end(output),[](uint8_t x){return x==0xa7;}),
          "empty vertical output retains the old no-write behavior");
  }
}

uint64_t OutputChecksum(std::span<const uint8_t> bytes) {
  uint64_t sum = 0;
  size_t at = 0;
  for (; at + sizeof(uint64_t) <= bytes.size(); at += sizeof(uint64_t)) {
    uint64_t word;
    std::memcpy(&word,bytes.data()+at,sizeof(word));
    sum += word;
  }
  for (; at < bytes.size(); ++at) sum = sum * 131 + bytes[at];
  return sum;
}

void CheckDelivered(Image& image, const ScaleCase& sample,
                    const std::vector<uint8_t>& expected, int64_t timestamp) {
  Check(image.width==sample.target_width && image.height==sample.target_height &&
        image.timestampNs==timestamp && image.dataSpace==0x8c20000 && image.halFormat==sample.format,
        "scaled delivery retains public metadata");
  Check(ImageStorageMatches(image,expected),"scaled delivery complete bytes match original coordinate oracle");
  const int planes = IsRgbaFormat(sample.format) ? 1 : 3;
  Check(static_cast<int>(image.planes.size())==planes,"scaled delivery retains plane count");
  if (static_cast<int>(image.planes.size()) != planes) return;
  size_t offset = 0;
  for (int p = 0; p < planes; ++p) {
    const int width = p ? (sample.target_width+1)/2 : sample.target_width;
    const int height = p ? (sample.target_height+1)/2 : sample.target_height;
    const int pixel_stride = IsRgbaFormat(sample.format) ? 4 : 1;
    const auto& plane = image.planes[p];
    const size_t size = static_cast<size_t>(width)*height*pixel_stride;
    Check(plane.offset==offset && plane.size==size && plane.rowStride==width*pixel_stride &&
          plane.pixelStride==pixel_stride,"scaled delivery retains exact independent plane extents");
    offset += size;
  }
}

uint64_t Transfer(const ScaleCase& sample, const VideoFrame& frame,
                  const std::vector<uint8_t>& expected, int count) {
  uint64_t checksum = 0;
  {
    Target target(sample.target_width,sample.target_height,sample.format);
    if (sample.closed) target.Close();
    for (int n = 0; n < count; ++n) {
      const int result = target.Deliver(frame,n);
      Check(result==(sample.closed ? NO_INIT : OK),"whole scaled delivery preserves result");
      if (sample.closed) continue;
      Image* image = target.Acquire();
      Check(image!=nullptr,"whole scaled delivery acquires actual published image");
      if (!image) continue;
      CheckDelivered(*image,sample,expected,n);
      checksum += OutputChecksum(ImageStorage(*image)) ^ static_cast<uint64_t>(n);
      ReleaseImage(target.reader,image);
    }
    Check(target.reader->acquired==0 && target.reader->pending.empty(),
          "whole scaled delivery retires every image before owner close");
    target.Close();
    Check(target.reader->closed,"whole scaled delivery closes its owner");
  }
  Check(gReaders.empty(),"whole scaled delivery destroys all native reader owners");
  return checksum;
}

void DeliveryCases() {
  for (const auto& sample : kScaleCases) {
    const auto frame = Frame(sample.width,sample.height,sample.crop,sample.crop,sample.padding,19);
    const auto before = frame.data;
    const auto expected = ExpectedPlanes(frame,sample.target_width,sample.target_height,sample.format);
    Transfer(sample,frame,expected,2);
    Check(frame.data==before,"whole scaled delivery leaves decoder storage unchanged");
  }
}

#ifdef READER_PLANE_TIMING
uint64_t Clock(clockid_t id) {
  timespec value{};
  if (clock_gettime(id,&value)!=0) std::abort();
  return static_cast<uint64_t>(value.tv_sec)*1000000000ULL+value.tv_nsec;
}
int Benchmark(const char* name) {
  const ScaleCase* sample = nullptr;
  for (const auto& item : kScaleCases) if (std::strcmp(item.name,name)==0) sample=&item;
  if (!sample) return 2;
  const auto frame = Frame(sample->width,sample->height,sample->crop,sample->crop,sample->padding,19);
  const auto expected = ExpectedPlanes(frame,sample->target_width,sample->target_height,sample->format);
  for (int phase = 0; phase < 2; ++phase) {
    const int frames = phase==0 ? 1 : sample->frames;
    const int checks_before = checks;
    const uint64_t cpu = Clock(CLOCK_PROCESS_CPUTIME_ID), wall = Clock(CLOCK_MONOTONIC);
    const uint64_t checksum = Transfer(*sample,frame,expected,frames);
    const uint64_t elapsed_wall = Clock(CLOCK_MONOTONIC)-wall;
    const uint64_t elapsed_cpu = Clock(CLOCK_PROCESS_CPUTIME_ID)-cpu;
    std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"frames\":%d,\"checks\":%d,\"failures\":%d,"
                "\"checksum\":%llu,\"cpu_ns\":%llu,\"wall_ns\":%llu}\n",name,phase==0?"first":"steady",
                frames,checks-checks_before,failures,static_cast<unsigned long long>(checksum),
                static_cast<unsigned long long>(elapsed_cpu),static_cast<unsigned long long>(elapsed_wall));
  }
  return failures ? 1 : 0;
}
#endif
}  // namespace

int main(int argc, char** argv) {
#ifdef READER_PLANE_TIMING
  if (argc==3 && std::strcmp(argv[1],"--bench")==0) return Benchmark(argv[2]);
#endif
  const bool quick = argc==2 && std::strcmp(argv[1],"--quick")==0;
  PlaneCoordinates(quick);
  if (!failures) {
    PixelCases();
#ifndef READER_PLANE_TIMING
    Lifetimes();  // The controlled close race uses the allocation observer.
#endif
    DeliveryCases();
  }
  Check(gReaders.empty(),"all plane-scaling reader owners retired");
  std::printf("READER_PLANE_SCALE_CONTRACTS %d checks; %d failures\n",checks,failures);
  return failures ? 1 : 0;
}
