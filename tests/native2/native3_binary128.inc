Part Binary128Parsing() {
  Part p{"binary128"};
  binary128_contract::Api<long double, wchar_t, locale_t> api;
  auto check = [&](bool ok, const std::string& what) { p.Check(ok, what); };
  if (!api.Load([](const char* name) { return dlsym(RTLD_DEFAULT, name); }, check)) return p;
  const std::array<int, 4> modes{FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD};
  const std::array<int, 5> flags{FE_INVALID, FE_DIVBYZERO, FE_OVERFLOW, FE_UNDERFLOW, FE_INEXACT};
  binary128_contract::Run(api, check, "dlsym", modes, flags, LC_ALL_MASK);
  api.narrow = &strtold;
  api.narrow_l = &strtold_l;
  api.wide = &wcstold;
  api.wide_l = &wcstold_l;
  binary128_contract::Run(api, check, "imports", modes, flags, LC_ALL_MASK);
  p.notes.push_back("86 Android cases; four APIs and modes; binary128 bits/end/errno; sticky flags; imports and dlsym");
  return p;
}
