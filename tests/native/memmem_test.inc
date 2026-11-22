// Shared by the imported x86-64/ARM64 libc fixture and the production host
// fixture. The volatile import prevents the compiler substituting host libc.
static void* (*volatile memmem_call)(const void*, size_t, const void*, size_t) = memmem;

static const void* memmem_oracle(const void* h, size_t hn, const void* n, size_t nn) {
  const unsigned char* haystack = (const unsigned char*)h;
  const unsigned char* needle = (const unsigned char*)n;
  if (nn == 0) return h;
  if (nn > hn) return NULL;
  for (size_t start = 0; start <= hn - nn; ++start) {
    size_t matched = 0;
    while (matched < nn && haystack[start + matched] == needle[matched]) ++matched;
    if (matched == nn) return haystack + start;
  }
  return NULL;
}

static uint32_t memmem_random(uint32_t* state) {
  *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
  return *state;
}

static void test_memmem_contracts(void) {
  const unsigned char binary[] = {0, 255, 0, 128, 255, 0, 128, 0};
  const unsigned char needle[] = {255, 0, 128};
  check("memmem binary bytes and first match",
        memmem_call(binary, sizeof(binary), needle, sizeof(needle)) == binary + 1);
  check("memmem empty needle returns original pointer",
        memmem_call(binary, 0, binary, 0) == binary &&
        memmem_call(binary + 3, 4, binary, 0) == binary + 3);
  check("memmem empty haystack and longer needle",
        memmem_call(binary, 0, needle, 1) == NULL &&
        memmem_call(binary, 2, needle, sizeof(needle)) == NULL);
  unsigned char all_bytes[256];
  for (size_t i = 0; i < sizeof(all_bytes); ++i) all_bytes[i] = (unsigned char)i;
  int all_found = 1;
  for (size_t i = 0; i < sizeof(all_bytes); ++i) {
    all_found &= memmem_call(all_bytes, sizeof(all_bytes), all_bytes + i, 1) == all_bytes + i;
  }
  check("memmem single-byte search covers all 256 values", all_found);

  unsigned char haystack[1031], pattern[519];
  uint32_t state = UINT32_C(0x1bd5184d);
  int exact = 1;
  for (int round = 0; round < 4096 && exact; ++round) {
    const size_t hn = memmem_random(&state) % 1025;
    const size_t nn = memmem_random(&state) % 513;
    const unsigned alphabet = 2 + memmem_random(&state) % 255;
    const size_t hoff = memmem_random(&state) % 7;
    const size_t noff = memmem_random(&state) % 7;
    unsigned char* h = haystack + hoff;
    unsigned char* n = pattern + noff;
    for (size_t i = 0; i < hn; ++i) h[i] = (unsigned char)(memmem_random(&state) % alphabet);
    for (size_t i = 0; i < nn; ++i) n[i] = (unsigned char)(memmem_random(&state) % alphabet);
    if (nn <= hn && (round % 3) != 0) {
      const size_t at = memmem_random(&state) % (hn - nn + 1);
      memcpy(h + at, n, nn);
    }
    errno = EDOM;
    exact = memmem_call(h, hn, n, nn) == memmem_oracle(h, hn, n, nn) && errno == EDOM;
  }
  check("memmem 4096 binary/unaligned differential cases preserve errno", exact);

  const size_t lengths[] = {2, 3, 15, 16, 17, 18, 31, 32, 63, 64, 127, 255, 256, 257, 512};
  for (size_t k = 0; k < sizeof(lengths) / sizeof(lengths[0]); ++k) {
    size_t nn = lengths[k];
    memset(haystack, 0xa5, sizeof(haystack));
    memset(pattern, 0xa5, nn);
    pattern[nn - 1] = 0x5a;
    check("memmem repeated-prefix miss", memmem_call(haystack, sizeof(haystack), pattern, nn) == NULL);
    haystack[sizeof(haystack) - 1] = 0x5a;
    check("memmem repeated-prefix final match",
          memmem_call(haystack, sizeof(haystack), pattern, nn) == haystack + sizeof(haystack) - nn);
    for (size_t i = 0; i < sizeof(haystack); ++i) haystack[i] = (unsigned char)(i % 3);
    for (size_t i = 0; i < nn; ++i) pattern[i] = (unsigned char)((i + 2) % 3);
    check("memmem periodic first match",
          memmem_call(haystack, sizeof(haystack), pattern, nn) == haystack + 2);
  }
}
