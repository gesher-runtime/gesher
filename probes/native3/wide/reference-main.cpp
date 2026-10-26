}  // namespace pinned

int main() {
  WideApi<pinned::mbstate_t> api{};
  api.decode = pinned::mbrtowc;
  api.decode32 = pinned::mbrtoc32;
  api.decode16 = pinned::mbrtoc16;
  api.encode = pinned::wcrtomb;
  api.encode32 = pinned::c32rtomb;
  api.encode16 = pinned::c16rtomb;
  api.length = pinned::mbrlen;
  api.initial = pinned::mbsinit;
  api.legacy_decode = pinned::mbtowc;
  api.legacy_length = pinned::mblen;
  api.legacy_encode = pinned::wctomb;
  api.decode_string = pinned::mbsrtowcs;
  api.decode_bounded = pinned::mbsnrtowcs;
  api.encode_string = pinned::wcsrtombs;
  api.encode_bounded = pinned::wcsnrtombs;
  api.legacy_decode_string = pinned::mbstowcs;
  api.legacy_encode_string = pinned::wcstombs;
  api.integer = pinned::wcstol;
  api.unsigned_integer = pinned::wcstoul;
  unsigned checks = 0;
  unsigned failures = 0;
  WideContracts(api, [&](bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
      ++failures;
      std::printf("FAIL pinned wide: %s (errno %d)\n", what.c_str(), errno);
    }
  });
  std::printf("%s pinned wide: %u conversion/state/boundary/LP64 assertions, %u failures\n",
              failures ? "FAIL" : "PASS", checks, failures);
  return failures != 0;
}
