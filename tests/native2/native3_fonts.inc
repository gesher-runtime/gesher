// NDK font interfaces dynamically resolved to keep the test's API-26 imports
// independent of when the public font entry points were introduced.
jstring JNICALL NativeFonts(JNIEnv* env, jclass) {
  Part p{"fonts"};
  void* library = dlopen("libandroid.so", RTLD_NOW);
  using Make = void* (*)();
  using Drop = void (*)(void*);
  using Style = void (*)(void*, uint16_t, bool);
  using Locales = void (*)(void*, const char*);
  using Variant = void (*)(void*, uint32_t);
  using MatchFont =
      void* (*)(const void*, const char*, const uint16_t*, uint32_t, uint32_t*);
  using String = const char* (*)(const void*);
  using Weight = uint16_t (*)(const void*);
  using Italic = bool (*)(const void*);
  using Count = size_t (*)(const void*);
  using Tag = uint32_t (*)(const void*, uint32_t);
  using Axis = float (*)(const void*, uint32_t);
  using Next = void* (*)(void*);
#define F(type, name) auto name = reinterpret_cast<type>(dlsym(library, #name))
  F(Make, AFontMatcher_create);
  F(Drop, AFontMatcher_destroy);
  F(Style, AFontMatcher_setStyle);
  F(Locales, AFontMatcher_setLocales);
  F(Variant, AFontMatcher_setFamilyVariant);
  F(MatchFont, AFontMatcher_match);
  F(Drop, AFont_close);
  F(String, AFont_getFontFilePath);
  F(Weight, AFont_getWeight);
  F(Italic, AFont_isItalic);
  F(String, AFont_getLocale);
  F(Count, AFont_getCollectionIndex);
  F(Count, AFont_getAxisCount);
  F(Tag, AFont_getAxisTag);
  F(Axis, AFont_getAxisValue);
  F(Make, ASystemFontIterator_open);
  F(Next, ASystemFontIterator_next);
  F(Drop, ASystemFontIterator_close);
#undef F
  bool complete =
      AFontMatcher_create && AFontMatcher_destroy && AFontMatcher_setStyle &&
      AFontMatcher_setLocales && AFontMatcher_setFamilyVariant &&
      AFontMatcher_match && AFont_close && AFont_getFontFilePath &&
      AFont_getWeight && AFont_isItalic && AFont_getLocale &&
      AFont_getCollectionIndex && AFont_getAxisCount && AFont_getAxisTag &&
      AFont_getAxisValue && ASystemFontIterator_open &&
      ASystemFontIterator_next && ASystemFontIterator_close;
  p.Check(library != nullptr && complete, "all 18 font entry points");
  if (!complete || library == nullptr)
    return env->NewStringUTF(p.Line().c_str());

  size_t axes = 0;
  auto valid = [&](void* font) {
    if (font == nullptr) return false;
    const char* path = AFont_getFontFilePath(font);
    bool good = path != nullptr && path[0] != 0;
    FILE* file = good ? fopen(path, "rb") : nullptr;
    unsigned char header[12]{};
    if (file == nullptr ||
        fread(header, 1, sizeof(header), file) != sizeof(header))
      good = false;
    if (file != nullptr) fclose(file);
    size_t index = AFont_getCollectionIndex(font);
    if (memcmp(header, "ttcf", 4) == 0) {
      uint32_t count = (uint32_t(header[8]) << 24) |
                       (uint32_t(header[9]) << 16) |
                       (uint32_t(header[10]) << 8) | header[11];
      good = good && index < count;
    } else {
      good = good && index == 0 &&
             (memcmp(header, "\0\1\0\0", 4) == 0 ||
              memcmp(header, "OTTO", 4) == 0);
    }
    uint16_t weight = AFont_getWeight(font);
    good = good && weight >= 1 && weight <= 1000;
    size_t count = AFont_getAxisCount(font);
    good = good && count <= 128;
    for (size_t i = 0; i < count && i < 128; ++i) {
      uint32_t tag = AFont_getAxisTag(font, static_cast<uint32_t>(i));
      float value = AFont_getAxisValue(font, static_cast<uint32_t>(i));
      good = good && tag != 0 && std::isfinite(value);
      ++axes;
    }
    return good;
  };

  void* iterator = ASystemFontIterator_open();
  p.Check(iterator != nullptr, "open font iterator");
  void* retained =
      iterator != nullptr ? ASystemFontIterator_next(iterator) : nullptr;
  std::string retained_path =
      retained != nullptr ? AFont_getFontFilePath(retained) : "";
  size_t count = retained != nullptr ? 1 : 0;
  bool all_files = retained != nullptr && valid(retained), has_locale = false;
  while (iterator != nullptr && count < 4096) {
    void* font = ASystemFontIterator_next(iterator);
    if (font == nullptr) break;
    all_files = valid(font) && all_files;
    const char* locale = AFont_getLocale(font);
    has_locale = has_locale || (locale != nullptr && locale[0] != 0);
    ++count;
    AFont_close(font);
  }
  ASystemFontIterator_close(iterator);
  p.Check(count > 10 && count < 4096 && all_files && has_locale,
          "enumerated real shipped font files and locale metadata");
  p.Check(retained != nullptr &&
              retained_path == AFont_getFontFilePath(retained) &&
              valid(retained),
          "font metadata survives iterator close");
  AFont_close(retained);

  void* matcher = AFontMatcher_create();
  p.Check(matcher != nullptr, "create matcher");
  if (matcher != nullptr) {
    AFontMatcher_setLocales(matcher, "en,he");
    AFontMatcher_setFamilyVariant(matcher, 2);  // AFAMILY_VARIANT_ELEGANT
    const uint16_t text[] = {'A', 0x05d0, 0x05d1};
    uint32_t offset = 0, runs = 0;
    while (offset < 3) {
      uint32_t length = 0;
      void* font = AFontMatcher_match(matcher, "sans-serif", text + offset,
                                      3 - offset, &length);
      bool good = valid(font) && length >= 1 && length <= 3 - offset;
      p.Check(good, "mixed Latin/Hebrew match and UTF-16 run bounds");
      AFont_close(font);
      if (!good) break;
      offset += length;
      ++runs;
    }
    p.Check(offset == 3, "mixed-script itemization covers all input");
    AFontMatcher_setStyle(matcher, 700, true);
    const uint16_t latin[] = {'H', 'e', 'l', 'l', 'o'};
    uint32_t length = 0;
    void* styled = AFontMatcher_match(matcher, "sans-serif", latin, 5, &length);
    AFontMatcher_destroy(matcher);
    p.Check(valid(styled) && length == 5 && AFont_isItalic(styled),
            "styled font survives matcher destruction");
    AFont_close(styled);
    p.notes.push_back(std::to_string(runs) + " mixed-script run(s)");
  }
  dlclose(library);
  p.notes.push_back(std::to_string(count) + " shipped font(s), " +
                    std::to_string(axes) + " axis value(s)");
  return env->NewStringUTF(p.Line().c_str());
}
