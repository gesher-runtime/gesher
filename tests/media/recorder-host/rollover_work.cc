// This file follows the existing host backend declarations in the generated
// translation unit. Both modes use the actual production MP4 implementation.
}  // namespace gesher::media

namespace {
bool count_allocations = false;
uint64_t allocated_bytes = 0, allocation_calls = 0;
}
void* operator new(size_t size) {
  if (count_allocations) { allocated_bytes += size; ++allocation_calls; }
  if (void* result = malloc(size == 0 ? 1 : size)) return result;
  std::abort();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

int main(int argc, char** argv) {
  using namespace gesher::media;
  assert(argc == 4);
  const bool reuse = std::string(argv[1]) == "reuse";
  std::ifstream audio(argv[2], std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(audio)), {});
  assert(bytes.size() > 7 && bytes[0] == 0xff);
  const size_t length = ((bytes[3] & 3u) << 11) | (bytes[4] << 3) | (bytes[5] >> 5);
  const std::vector<uint8_t> packet(bytes.begin() + 7, bytes.begin() + length);
  const std::string directory = argv[3];
  constexpr unsigned kSegments = 32, kSamples = 2048;
  const uint8_t config[] = {0x11, 0x88};  // actual AAC-LC 48 kHz mono access unit
  Format format;
  AMediaFormat_setString(format.get(), "mime", "audio/mp4a-latm");
  AMediaFormat_setInt32(format.get(), "sample-rate", 48000);
  AMediaFormat_setInt32(format.get(), "channel-count", 1);
  std::unique_ptr<Muxer> writer;
  std::string error;
  uint64_t checksum = 14695981039346656037ull;
  uint64_t output_bytes = 0;
  for (unsigned segment = 0; segment < kSegments; ++segment) {
    const std::string path = directory + "/" + std::to_string(segment) + ".mp4";
    const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    assert(fd >= 0);
    count_allocations = true;
    if (!reuse || segment == 0) {
      writer = Muxer::Create(fd, MuxerFormat::kMpeg4, &error);
      assert(writer != nullptr);
      assert(writer->AddTrack(format, &error) == 0);
      writer->SetOrientationHint(90);
      writer->SetLocation(319550, 352200);
      assert(writer->SetFileSizeLimit(2000000));
      assert(writer->Start(&error));
    }
#if HAS_SWITCH_FILE
    else {
      assert(writer->SwitchFile(fd, &error));
    }
#else
    else { std::abort(); }
#endif
    close(fd);
    assert(writer->WriteSample(0, config, sizeof(config), 0, 2, &error));
    for (unsigned sample = 0; sample < kSamples; ++sample) {
      const int64_t pts = (static_cast<int64_t>(segment) * kSamples + sample) * 20000;
      assert(writer->WriteSample(0, packet.data(), packet.size(), pts, 1, &error));
      assert(!writer->FileSizeLimitReached());
    }
    // Both modes include all finalization work: at the segment's end for
    // recreate, at the real SwitchFile boundary for reuse, and Stop at the end.
    if (!reuse || segment + 1 == kSegments) {
      assert(writer->Stop(&error));
      if (!reuse) writer.reset();
    }
    count_allocations = false;
  }
  for (unsigned segment = 0; segment < kSegments; ++segment) {
    std::ifstream file(directory + "/" + std::to_string(segment) + ".mp4", std::ios::binary);
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() != 0) {
      output_bytes += file.gcount();
      for (std::streamsize i = 0; i < file.gcount(); ++i)
        checksum = (checksum ^ static_cast<uint8_t>(buffer[i])) * 1099511628211ull;
    }
  }
  std::cout << "{\"mode\":\"" << (reuse ? "reuse" : "recreate") << "\",\"segments\":" << kSegments
            << ",\"samples_per_segment\":" << kSamples << ",\"allocation_calls\":" << allocation_calls
            << ",\"allocated_bytes\":" << allocated_bytes << ",\"output_bytes\":" << output_bytes
            << ",\"checksum\":" << checksum << "}\n";
}
