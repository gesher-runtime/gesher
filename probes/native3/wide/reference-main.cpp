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
  auto check = [&](bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
      ++failures;
      std::printf("FAIL pinned wide: %s (errno %d)\n", what.c_str(), errno);
    }
  };
  WideContracts(api, check);
  WideComparisonContracts<wchar_t>(pinned::llvm_signed::wcscmp, pinned::llvm_signed::wcsncmp,
      pinned::llvm_signed::wmemcmp, check);
  WideComparisonContracts<uint32_t>(pinned::llvm_unsigned::wcscmp, pinned::llvm_unsigned::wcsncmp,
      pinned::llvm_unsigned::wmemcmp, check);
  for (uint32_t high : {UINT32_C(0x80000000), UINT32_C(0xffffffff)}) {
    const uint32_t text[] = {high, '1', 0};
    uint32_t* end;
    errno = 0;
    check(pinned::StrToI<int64_t, INT64_MIN, INT64_MAX, uint32_t>(text, &end, 10) == 0 &&
              end == text && errno == 0,
          "pinned unsigned wchar numeric instantiation rejects high-bit input");
  }
  WideFloatingContracts(static_cast<double (*)(const wchar_t*, wchar_t**)>(pinned::wcstod),
      pinned::wcstof, [&](bool ok, const std::string& what) {
        ++checks;
        if (!ok) {
          ++failures;
          std::printf("FAIL pinned float grammar: %s (errno %d)\n", what.c_str(), errno);
        }
      });
  std::printf("%s pinned wide: %u conversion/state/boundary/LP64 assertions, %u failures\n",
              failures ? "FAIL" : "PASS", checks, failures);
  return failures != 0;
}
