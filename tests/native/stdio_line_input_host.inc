// Real files, pipes and the complete production shared adapter. Both public
// native getdelim and the Bionic entry's body run against that same owner.
static int64_t Delimited(bool guest, char** bytes, size_t* size, int delimiter, FILE* stream) {
  return guest ? guest_line::Getdelim(bytes, size, delimiter, stream) :
      adapter_getdelim(bytes, size, delimiter, stream);
}
static FILE* LineFile(const std::string& bytes) {
  char path[] = "stdio-line-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) std::abort();
  unlink(path);
  if (write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()) ||
      lseek(fd, 0, SEEK_SET) != 0) std::abort();
  FILE* stream = adapter_fdopen(fd, "r+");
  if (!stream) std::abort();
  return stream;
}
static void DelimitedBytes(bool guest) {
  char* bytes = nullptr;
  size_t size = 999;
  FILE* empty = LineFile("");
  check("empty EOF does not allocate an unused record", Delimited(guest, &bytes, &size, '\n', empty) == -1 &&
        bytes == nullptr && size == 0 && adapter_feof(empty) && !adapter_ferror(empty));
  adapter_fclose(empty);
  std::string input;
  for (int i = 0; i < 256; ++i) input += static_cast<char>(i);
  for (int delimiter = 0; delimiter < 256; ++delimiter) {
    FILE* stream = LineFile(input);
    int64_t length = Delimited(guest, &bytes, &size, delimiter, stream);
    check("all byte delimiters include the actual delimiter", length == delimiter + 1 &&
          std::memcmp(bytes, input.data(), static_cast<size_t>(length)) == 0 && bytes[length] == 0);
    check("delimiter leaves the next byte in the same stream", adapter_fgetc(stream) ==
          (delimiter == 255 ? EOF : delimiter + 1));
    adapter_fclose(stream);
  }
  FILE* high = LineFile(std::string("x\xffy\0z", 5));
  check("negative delimiter is converted to unsigned byte", Delimited(guest, &bytes, &size, -1, high) == 2 &&
        static_cast<unsigned char>(bytes[1]) == 255 && bytes[2] == 0);
  check("NUL delimiter includes NUL and a separate terminator", Delimited(guest, &bytes, &size, 256, high) == 2 &&
        bytes[0] == 'y' && bytes[1] == 0 && bytes[2] == 0);
  check("final partial record succeeds at EOF", Delimited(guest, &bytes, &size, '\n', high) == 1 && bytes[0] == 'z');
  char* retained = bytes;
  size_t retained_size = size;
  check("repeated EOF retains caller-owned allocation", Delimited(guest, &bytes, &size, '\n', high) == -1 &&
        bytes == retained && size == retained_size);
  adapter_fclose(high);
  std::free(bytes);
}
static void DelimitedPushback(bool guest) {
  FILE* stream = LineFile("file\nnext\n");
  for (int i = 0; i < 70; ++i) check("pushback grows beyond its inline buffer", adapter_ungetc('A' + i % 20, stream) >= 0);
  check("pushback contains a delimiter", adapter_ungetc('\n', stream) == '\n');
  check("pushback contains a leading byte", adapter_ungetc('!', stream) == '!');
  char* bytes = nullptr;
  size_t size = 0;
  check("delimiter in reverse-order pushback ends the record", Delimited(guest, &bytes, &size, '\n', stream) == 2 &&
        std::memcmp(bytes, "!\n", 3) == 0);
  int64_t length = Delimited(guest, &bytes, &size, '\n', stream);
  check("remaining pushback precedes buffered descriptor input", length == 75);
  if (length == 75) {
    for (int i = 0; i < 70; ++i) check("every pushed-back byte retains LIFO order", bytes[i] == 'A' + (69 - i) % 20);
    check("descriptor prefix is unaltered after pushback", std::memcmp(bytes + 70, "file\n", 6) == 0);
  }
  check("logical offset excludes read-ahead", adapter_ftell(stream) == 5);
  check("next record survives the span boundary", Delimited(guest, &bytes, &size, '\n', stream) == 5 &&
        std::memcmp(bytes, "next\n", 6) == 0);
  std::free(bytes);
  adapter_fclose(stream);
}
static void DelimitedFailures(bool guest) {
  FILE* stream = LineFile("not-consumed\n");
  char* bytes = nullptr;
  size_t size = 0;
  line_fail_realloc = true;
  const auto result = Delimited(guest, &bytes, &size, '\n', stream);
  const int error = errno;
  line_fail_realloc = false;
  check("allocation error sets ferror and preserves ownership", result == -1 && error == ENOMEM &&
        bytes == nullptr && size == 0 && adapter_ferror(stream));
  check("allocation error does not discard the current input span", adapter_ftell(stream) == 0);
  adapter_clearerr(stream);
  check("retry after allocation failure returns the full record", Delimited(guest, &bytes, &size, '\n', stream) == 13 &&
        std::memcmp(bytes, "not-consumed\n", 14) == 0);
  adapter_fclose(stream);
  std::free(bytes);

  stream = LineFile("pending\n");
  errno = 0;
  check("null line pointer sets EINVAL and stream error", Delimited(guest, nullptr, &size, '\n', stream) == -1 &&
        errno == EINVAL && adapter_ferror(stream) && gesher::stdio::Orient(stream, 0, true) == 0);
  adapter_clearerr(stream);
  bytes = nullptr;
  check("null size pointer sets stream error without consuming input", Delimited(guest, &bytes, nullptr, '\n', stream) == -1 &&
        errno == EINVAL && adapter_ferror(stream) && adapter_ftell(stream) == 0);
  adapter_fclose(stream);

  int pipes[2];
  if (pipe2(pipes, O_NONBLOCK | O_CLOEXEC) != 0) std::abort();
  stream = adapter_fdopen(pipes[0], "r");
  check("nonblocking prefix is written", write(pipes[1], "part", 4) == 4);
  bytes = nullptr; size = 0;
  errno = 0;
  check("partial EAGAIN is an error rather than a completed record", Delimited(guest, &bytes, &size, '\n', stream) == -1 &&
        errno == EAGAIN && adapter_ferror(stream) && !adapter_feof(stream));
  check("partial-error buffer retains transferred input", bytes && std::memcmp(bytes, "part", 4) == 0);
  check("recovery record is written", write(pipes[1], "new\n", 4) == 4);
  adapter_clearerr(stream);
  check("explicit error recovery resumes at the actual descriptor", Delimited(guest, &bytes, &size, '\n', stream) == 4 &&
        std::memcmp(bytes, "new\n", 5) == 0 && !adapter_ferror(stream));
  close(pipes[1]);
  check("pipe EOF is not an input error", Delimited(guest, &bytes, &size, '\n', stream) == -1 &&
        adapter_feof(stream) && !adapter_ferror(stream));
  std::free(bytes);
  adapter_fclose(stream);
}
static void BoundedLines() {
  for (int mode : {_IONBF, _IOLBF, _IOFBF}) for (int capacity : {1, 2, 7, 32}) {
    std::string source = "abcdef\n" + std::string("nul\0tail\nfinal", 14);
    FILE* stream = LineFile(source);
    char storage[32];
    check("bounded line buffering configures", adapter_setvbuf(stream, storage, mode, capacity) == 0);
    char bytes[40];
    std::memset(bytes, 0x5a, sizeof(bytes));
    check("count-one fgets terminates without input", adapter_fgets(bytes, 1, stream) == bytes && bytes[0] == 0 &&
          bytes[1] == 0x5a && adapter_ftell(stream) == 0);
    check("bounded fgets consumes only requested bytes", adapter_fgets(bytes, 4, stream) == bytes &&
          std::memcmp(bytes, "abc", 4) == 0 && adapter_ftell(stream) == 3);
    check("guest unlocked fgets uses same read-ahead", guest_line::FgetsUnlocked(bytes, 30, stream) == bytes &&
          std::memcmp(bytes, "def\n", 5) == 0);
    check("NUL inside fgets is ordinary input", adapter_fgets(bytes, 30, stream) == bytes &&
          std::memcmp(bytes, "nul\0tail\n", 10) == 0);
    check("unterminated fgets returns remaining text", adapter_fgets(bytes, 30, stream) == bytes &&
          std::memcmp(bytes, "final", 6) == 0);
    bytes[0] = 'x';
    check("empty fgets leaves output untouched", adapter_fgets(bytes, 30, stream) == nullptr && bytes[0] == 'x');
    adapter_fclose(stream);
  }
}
static void ConcurrentLines() {
  std::string source;
  for (int i = 0; i < 500; ++i) source += "record:" + std::to_string(i) + "\n";
  FILE* stream = LineFile(source);
  std::vector<std::string> outputs[2];
  auto read = [&](int lane) {
    char* bytes = nullptr; size_t size = 0; int64_t length;
    while ((length = Delimited(lane != 0, &bytes, &size, '\n', stream)) >= 0)
      outputs[lane].emplace_back(bytes, static_cast<size_t>(length));
    std::free(bytes);
  };
  std::thread first(read, 0), second(read, 1);
  first.join(); second.join();
  outputs[0].insert(outputs[0].end(), outputs[1].begin(), outputs[1].end());
  std::sort(outputs[0].begin(), outputs[0].end());
  check("native and guest readers serialize complete records", outputs[0].size() == 500 &&
        std::adjacent_find(outputs[0].begin(), outputs[0].end()) == outputs[0].end());
  adapter_fclose(stream);
}
static void TestLines() {
  for (bool guest : {false, true}) {
    DelimitedBytes(guest);
    DelimitedPushback(guest);
    DelimitedFailures(guest);
  }
  BoundedLines();
  ConcurrentLines();
}
static int BenchmarkLines() {
  constexpr size_t length = 65536, records = 64;
  std::string source(records * length, 'q');
  for (size_t i = length - 1; i < source.size(); i += length) source[i] = '\n';
  for (int operation = 0; operation < 4; ++operation) {
    FILE* stream = LineFile(source);
    char* bytes = nullptr; size_t size = 0;
    if (operation >= 2) { size = length + 1; bytes = static_cast<char*>(std::malloc(size)); }
    for (int sample = 0; sample < 5; ++sample) {
      adapter_fseek(stream, 0, SEEK_SET);
      registry_visits = line_reallocations = 0;
      uint64_t checksum = 0;
      const auto start = std::chrono::steady_clock::now();
      for (size_t row = 0; row < records; ++row) {
        int64_t result;
        if (operation < 2) result = Delimited(operation == 1, &bytes, &size, '\n', stream);
        else result = (operation == 2 ? adapter_fgets(bytes, static_cast<int>(size), stream) :
                       guest_line::FgetsUnlocked(bytes, static_cast<int>(size), stream)) ? length : -1;
        if (result != static_cast<int64_t>(length) || bytes[length] != 0 ||
            std::memcmp(bytes, source.data() + row * length, length) != 0) return 2;
        checksum += static_cast<unsigned char>(bytes[row % (length - 1)]);
      }
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start).count();
      char report[256];
      const int count = snprintf(report, sizeof(report),
          "line-benchmark operation=%d sample=%d bytes=%zu registry_visits=%llu reallocations=%zu microseconds=%lld checksum=%llu\n",
          operation, sample, source.size(), static_cast<unsigned long long>(registry_visits), line_reallocations,
          static_cast<long long>(elapsed), static_cast<unsigned long long>(checksum));
      write(STDOUT_FILENO, report, static_cast<size_t>(count));
    }
    std::free(bytes);
    adapter_fclose(stream);
  }
  return 0;
}
