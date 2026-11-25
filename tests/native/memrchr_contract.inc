// Shared by the imported Android libc probe and the actual-source host gate.
// The oracle deliberately scans forwards and remembers the last occurrence.
typedef void* (*memrchr_function)(const void*, int, size_t);
typedef void* (*memrchr_checked_function)(const void*, int, size_t, size_t);

static const void* memrchr_expected(const unsigned char* bytes, int value, size_t count) {
  const void* result = NULL;
  for (size_t i = 0; i < count; ++i) {
    if (bytes[i] == (unsigned char)value) result = bytes + i;
  }
  return result;
}

static uint32_t memrchr_random(uint32_t* state) {
  *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
  return *state;
}

static size_t test_memrchr_contracts(memrchr_function call, memrchr_checked_function checked) {
  size_t cases = 0;
  unsigned char storage[320], saved[320];
  check("memrchr and checked export are present", call != NULL && checked != NULL);
  if (!call || !checked) return 0;

  const unsigned char repeated[] = {3, 7, 3, 9, 3, 7, 3};
  check("memrchr returns the last occurrence",
        call(repeated, 3, sizeof(repeated)) == repeated + 6 &&
        call(repeated, 3, sizeof(repeated) - 1) == repeated + 4);
  check("memrchr bounded miss and empty range",
        call(repeated, 11, sizeof(repeated)) == NULL && call(repeated, 3, 0) == NULL);
  cases += 4;

  memset(storage, 7, sizeof(storage));
  storage[17] = storage[20] = 3;
  check("memrchr returns the last occurrence within a word", call(storage, 3, 32) == storage + 20);
  ++cases;

  int exact = 1;
  for (unsigned value = 0; value != 256 && exact; ++value) {
    for (size_t position = 0; position != 32 && exact; ++position) {
      memset(storage, (unsigned char)(value ^ 1), sizeof(storage));
      storage[position] = (unsigned char)value;
      exact = call(storage, (int)value, 33) == storage + position;
      ++cases;
    }
  }
  check("memrchr borrow flags do not select a different byte", exact);

  // Every byte value, all 16 alignments, and every match position on both
  // sides of the word/tiny boundaries. Each input also has a full miss.
  const size_t lengths[] = {1, 2, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32, 33, 63, 64, 65};
  exact = 1;
  for (unsigned value = 0; value != 256 && exact; ++value) {
    for (size_t offset = 0; offset != 16 && exact; ++offset) {
      for (size_t li = 0; li < sizeof(lengths) / sizeof(lengths[0]) && exact; ++li) {
        const size_t length = lengths[li];
        unsigned char* bytes = storage + offset;
        memset(storage, (unsigned char)(value ^ 0x80), sizeof(storage));
        errno = EDOM;
        exact = call(bytes, (int)value, length) == NULL && errno == EDOM;
        ++cases;
        for (size_t position = 0; position < length && exact; ++position) {
          bytes[position] = (unsigned char)value;
          // Alternate negative and overflowing int arguments: the contract
          // compares the conversion to unsigned char, not the original int.
          const int argument = (int)value + ((position & 1) ? 256 : -256);
          errno = EDOM;
          exact = call(bytes, argument, length) == bytes + position && errno == EDOM;
          ++cases;
          bytes[position] = (unsigned char)(value ^ 0x80);
        }
      }
    }
  }
  check("memrchr all bytes, alignments and match positions", exact);
  uint32_t state = UINT32_C(0x715f09a3);
  exact = 1;
  for (size_t round = 0; round != 8192 && exact; ++round) {
    const size_t offset = memrchr_random(&state) % 16;
    const size_t length = memrchr_random(&state) % 289;
    const int value = (int)(memrchr_random(&state) % 1024) - 512;
    for (size_t i = 0; i < sizeof(storage); ++i) {
      storage[i] = (unsigned char)(memrchr_random(&state) >> 24);
    }
    memcpy(saved, storage, sizeof(storage));
    const void* expected = memrchr_expected(storage + offset, value, length);
    errno = ERANGE;
    exact = call(storage + offset, value, length) == expected && errno == ERANGE &&
            checked(storage + offset, value, length, sizeof(storage) - offset) == expected &&
            errno == ERANGE && memcmp(storage, saved, sizeof(storage)) == 0;
    cases += 2;
  }
  check("memrchr seeded differential and checked calls preserve input and errno", exact);
  check("memrchr checked exact bound includes its last byte",
        checked(repeated, 3, sizeof(repeated), sizeof(repeated)) == repeated + 6);
  ++cases;
  return cases;
}
