// Gesher: libandroid's NDK font APIs over the same Minikin collections that
// Android's Typeface registers. Font metadata and itemization follow pinned
// frameworks/base/native/android/system_fonts.cpp (Apache-2.0, AOSP 2018).

#include <gesher/bionic_shim.h>
#include <gesher/font_paths.h>
#include <hwui/MinikinSkia.h>
#include <minikin/FontCollection.h>
#include <minikin/FontFamily.h>
#include <minikin/LocaleList.h>
#include <minikin/SystemFonts.h>
#include <tinyxml2.h>
#include <windows.h>
#include <include/core/SkData.h>
#include <include/core/SkFontArguments.h>
#include <include/core/SkStream.h>
#include <include/core/SkTypeface.h>
#include <include/ports/SkTypeface_fontations.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "abi_types.h"

namespace gesher::bionic {
namespace {

struct Font {
  std::string path;
  std::optional<std::string> locale;
  uint16_t weight = 400;
  bool italic = false;
  uint32_t index = 0;
  std::vector<std::pair<uint32_t, float>> axes;
  // XML family identity is only used by the standalone collection builder.
  size_t family_id = 0;
  std::string family_name;
  std::string fallback_for;
  minikin::FamilyVariant variant = minikin::FamilyVariant::DEFAULT;
  bool operator<(const Font& other) const {
    return std::tie(path, locale, weight, italic, index, axes) <
           std::tie(other.path, other.locale, other.weight, other.italic,
                    other.index, other.axes);
  }
};

struct Matcher {
  minikin::FontStyle style;
  uint32_t locales = 0;
  uint32_t variant = 0;
};

struct Iterator {
  std::vector<Font> fonts;
  size_t next = 0;
};

std::string TrimFont(const char* value) {
  std::string text = value != nullptr ? value : "";
  size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool ReadFontXml(const std::string& path, tinyxml2::XMLDocument* document) {
  FILE* file = _wfopen(gesher::NativeFontWindowsPath(path).c_str(), L"rb");
  if (file == nullptr) return false;
  std::string text;
  char buffer[8192];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
    text.append(buffer, count);
  bool read = !ferror(file);
  fclose(file);
  return read &&
         document->Parse(text.data(), text.size()) == tinyxml2::XML_SUCCESS;
}

void ReadFontFamilies(const std::string& xml, const std::string& directory,
                      Iterator* iterator) {
  tinyxml2::XMLDocument document;
  if (!ReadFontXml(xml, &document)) return;
  const auto* root = document.RootElement();
  if (root == nullptr) return;
  for (const auto* family = root->FirstChildElement("family");
       family != nullptr; family = family->NextSiblingElement("family")) {
    size_t family_id = iterator->fonts.size();
    for (const auto* item = family->FirstChildElement("font"); item != nullptr;
         item = item->NextSiblingElement("font")) {
      std::string name = TrimFont(item->GetText());
      if (name.empty()) continue;
      Font font;
      font.family_id = family_id;
      if (const char* name = family->Attribute("name")) font.family_name = name;
      if (const char* target = item->Attribute("fallbackFor")) font.fallback_for = target;
      if (const char* variant = family->Attribute("variant")) {
        if (std::string(variant) == "compact") font.variant = minikin::FamilyVariant::COMPACT;
        if (std::string(variant) == "elegant") font.variant = minikin::FamilyVariant::ELEGANT;
      }
      font.path = directory + "/" + name;
      if (const char* locale = family->Attribute("lang")) font.locale = locale;
      font.weight =
          static_cast<uint16_t>(item->UnsignedAttribute("weight", 400));
      font.index = item->UnsignedAttribute("index", 0);
      const char* style = item->Attribute("style");
      font.italic = style != nullptr && std::string(style) == "italic";
      for (const auto* axis = item->FirstChildElement("axis"); axis != nullptr;
           axis = axis->NextSiblingElement("axis")) {
        std::string tag = TrimFont(axis->Attribute("tag"));
        if (tag.size() != 4 || axis->Attribute("stylevalue") == nullptr) continue;
        uint32_t value = 0;
        for (unsigned char c : tag) value = (value << 8) | c;
        font.axes.emplace_back(value, axis->FloatAttribute("stylevalue", 0));
      }
      iterator->fonts.push_back(std::move(font));
    }
  }
}

// Keep byte storage alive for Minikin's HarfBuzz face. This map is private to
// native matching and never publishes into SystemFonts: later framework/Java
// registration remains authoritative and is checked before this fallback.
class StandaloneTypeface : public android::MinikinFontSkia {
 public:
  StandaloneTypeface(sk_sp<SkTypeface> typeface, sk_sp<SkData> data,
                     const Font& font, const minikin::VariationSettings& axes)
      : MinikinFontSkia(std::move(typeface), 0, data->data(), data->size(),
                        font.path, font.index, axes), data_(std::move(data)) {}
 private:
  sk_sp<SkData> data_;
};

std::shared_ptr<minikin::Font> LoadStandaloneFont(const Font& font) {
  FILE* file = _wfopen(gesher::NativeFontWindowsPath(font.path).c_str(), L"rb");
  if (file == nullptr) return nullptr;
  auto data = SkData::MakeFromFILE(file);
  fclose(file);
  if (!data) return nullptr;
  std::vector<SkFontArguments::VariationPosition::Coordinate> coordinates;
  std::vector<minikin::FontVariation> axes;
  for (const auto& [tag, value] : font.axes) {
    coordinates.push_back({tag, value});
    axes.push_back({tag, value});
  }
  SkFontArguments args;
  args.setCollectionIndex(font.index);
  args.setVariationDesignPosition({coordinates.data(), static_cast<int>(coordinates.size())});
  auto typeface = SkTypeface_Make_Fontations(data, args);
  if (!typeface) return nullptr;
  auto native = std::make_shared<StandaloneTypeface>(std::move(typeface), data, font,
                                                    minikin::VariationSettings(axes));
  return minikin::Font::Builder(native)
      .setStyle(minikin::FontStyle(font.weight, static_cast<minikin::FontStyle::Slant>(font.italic)))
      .setLocaleListId(font.locale ? minikin::registerLocaleList(font.locale->c_str()) : 0)
      .build();
}

struct StandaloneFonts {
  std::map<std::string, std::shared_ptr<minikin::FontCollection>> collections;
  StandaloneFonts() {
    const auto paths = gesher::SystemFontFiles();
    Iterator parsed;
    ReadFontFamilies(paths.xml, paths.directory, &parsed);
    struct Family {
      std::string name, fallback_for;
      std::shared_ptr<minikin::FontFamily> native;
    };
    std::vector<Family> families;
    for (size_t i = 0; i < parsed.fonts.size();) {
      const Font& first = parsed.fonts[i];
      size_t next = i;
      std::map<std::string, std::vector<std::shared_ptr<minikin::Font>>> groups;
      while (next < parsed.fonts.size() && parsed.fonts[next].family_id == first.family_id) {
        const auto& source = parsed.fonts[next++];
        auto font = LoadStandaloneFont(source);
        if (font) groups[source.fallback_for].push_back(std::move(font));
      }
      for (auto& [target, fonts] : groups) {
        uint32_t locale = first.locale ? minikin::registerLocaleList(first.locale->c_str()) : 0;
        auto family = minikin::FontFamily::create(locale, first.variant, std::move(fonts),
            false, first.family_name.empty(), minikin::VariationFamilyType::None);
        families.push_back({first.family_name, target, std::move(family)});
      }
      i = next;
    }
    for (const auto& primary : families) {
      if (primary.name.empty()) continue;
      std::vector<std::shared_ptr<minikin::FontFamily>> chain{primary.native};
      for (const auto& fallback : families)
        if (fallback.name.empty() &&
            (fallback.fallback_for.empty() || fallback.fallback_for == primary.name))
          chain.push_back(fallback.native);
      collections.emplace(primary.name, minikin::FontCollection::create(chain));
    }
    tinyxml2::XMLDocument document;
    if (ReadFontXml(paths.xml, &document) && document.RootElement()) {
      for (const auto* alias = document.RootElement()->FirstChildElement("alias");
           alias; alias = alias->NextSiblingElement("alias")) {
        const char* name = alias->Attribute("name");
        const char* target = alias->Attribute("to");
        if (!name || !target) continue;
        auto found = collections.find(target);
        if (found == collections.end()) continue;
        // NDK matcher style comes from AFontMatcher_setStyle. Typeface's
        // alias base weight belongs to the Java Typeface, not the collection.
        collections.emplace(name, found->second);
      }
    }
  }
};

std::shared_ptr<minikin::FontCollection> StandaloneCollection(const char* family) {
  static const StandaloneFonts fonts;
  auto found = fonts.collections.find(family != nullptr ? family : "sans-serif");
  if (found == fonts.collections.end()) found = fonts.collections.find("sans-serif");
  return found != fonts.collections.end() ? found->second : nullptr;
}

Iterator* BIONIC_SYSV OpenIterator() {
  auto iterator = std::make_unique<Iterator>();
  std::set<Font> registered;
  minikin::SystemFonts::getFontSet([&registered](const auto& fonts) {
    for (const auto& source : fonts) {
      Font font;
      font.path = source->baseTypeface()->GetFontPath();
      uint32_t locales = source->getLocaleListId();
      if (locales != minikin::kEmptyLocaleListId)
        font.locale = minikin::getLocaleString(locales);
      font.weight = source->style().weight();
      font.italic =
          source->style().slant() == minikin::FontStyle::Slant::ITALIC;
      font.index = source->baseTypeface()->GetFontIndex();
      for (const auto& [tag, value] : source->baseTypeface()->GetAxes())
        font.axes.emplace_back(tag, value);
      registered.insert(std::move(font));
    }
  });
  if (!registered.empty()) {
    iterator->fonts.assign(registered.begin(), registered.end());
    return iterator.release();
  }
  const auto paths = gesher::SystemFontFiles();
  ReadFontFamilies(paths.xml, paths.directory, iterator.get());
  const char* product_root = std::getenv("ANDROID_PRODUCT_ROOT");
  std::string product = product_root != nullptr ? product_root : "/product";
  ReadFontFamilies(product + "/etc/fonts_customization.xml", product + "/fonts",
                   iterator.get());
  return iterator.release();
}

Font* BIONIC_SYSV NextFont(Iterator* iterator) {
  if (iterator == nullptr) return nullptr;
  while (iterator->next < iterator->fonts.size()) {
    const Font& font = iterator->fonts[iterator->next++];
    DWORD attributes =
        GetFileAttributesW(gesher::NativeFontWindowsPath(font.path).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & FILE_ATTRIBUTE_DIRECTORY))
      return new Font(font);
  }
  return nullptr;
}

void BIONIC_SYSV CloseIterator(Iterator* iterator) { delete iterator; }
Matcher* BIONIC_SYSV CreateMatcher() { return new Matcher(); }
void BIONIC_SYSV DestroyMatcher(Matcher* matcher) { delete matcher; }
void BIONIC_SYSV SetStyle(Matcher* matcher, uint16_t weight, bool italic) {
  matcher->style = minikin::FontStyle(
      weight, static_cast<minikin::FontStyle::Slant>(italic));
}
void BIONIC_SYSV SetLocales(Matcher* matcher, const char* locales) {
  matcher->locales = minikin::registerLocaleList(locales);
}
void BIONIC_SYSV SetVariant(Matcher* matcher, uint32_t variant) {
  matcher->variant = variant;
}

Font* BIONIC_SYSV Match(const Matcher* matcher, const char* family,
                        const uint16_t* text, uint32_t length,
                        uint32_t* run_length) {
  auto collection = minikin::SystemFonts::findFontCollection(family);
  if (collection == nullptr) collection = StandaloneCollection(family);
  if (collection == nullptr || length == 0) return nullptr;
  minikin::U16StringPiece input(text, length);
  auto runs = collection->itemize(
      input, matcher->style, matcher->locales,
      static_cast<minikin::FamilyVariant>(matcher->variant), 1);
  if (runs.empty()) return nullptr;
  const auto& font =
      collection->getBestFont(input, runs[0], matcher->style).font;
  const auto* skia =
      static_cast<android::MinikinFontSkia*>(font->baseTypeface().get());
  auto result = std::make_unique<Font>();
  result->path = skia->getFilePath();
  result->weight = font->style().weight();
  result->italic = font->style().slant() == minikin::FontStyle::Slant::ITALIC;
  result->index = skia->GetFontIndex();
  for (const auto& axis : skia->GetAxes())
    result->axes.emplace_back(axis.axisTag, axis.value);
  if (run_length != nullptr) *run_length = runs[0].end;
  return result.release();
}

void BIONIC_SYSV CloseFont(Font* font) { delete font; }
const char* BIONIC_SYSV FontPath(const Font* font) {
  return font->path.c_str();
}
uint16_t BIONIC_SYSV FontWeight(const Font* font) { return font->weight; }
bool BIONIC_SYSV FontItalic(const Font* font) { return font->italic; }
const char* BIONIC_SYSV FontLocale(const Font* font) {
  return font->locale.has_value() ? font->locale->c_str() : nullptr;
}
size_t BIONIC_SYSV FontIndex(const Font* font) { return font->index; }
size_t BIONIC_SYSV AxisCount(const Font* font) { return font->axes.size(); }
uint32_t BIONIC_SYSV AxisTag(const Font* font, uint32_t index) {
  return font->axes.at(index).first;
}
float BIONIC_SYSV AxisValue(const Font* font, uint32_t index) {
  return font->axes.at(index).second;
}

[[maybe_unused]] const bool registered = [] {
#define E(name, function) \
  AddBionicSymbol(name, reinterpret_cast<void*>(&function))
  E("ASystemFontIterator_open", OpenIterator);
  E("ASystemFontIterator_next", NextFont);
  E("ASystemFontIterator_close", CloseIterator);
  E("AFontMatcher_create", CreateMatcher);
  E("AFontMatcher_destroy", DestroyMatcher);
  E("AFontMatcher_setStyle", SetStyle);
  E("AFontMatcher_setLocales", SetLocales);
  E("AFontMatcher_setFamilyVariant", SetVariant);
  E("AFontMatcher_match", Match);
  E("AFont_close", CloseFont);
  E("AFont_getFontFilePath", FontPath);
  E("AFont_getWeight", FontWeight);
  E("AFont_isItalic", FontItalic);
  E("AFont_getLocale", FontLocale);
  E("AFont_getCollectionIndex", FontIndex);
  E("AFont_getAxisCount", AxisCount);
  E("AFont_getAxisTag", AxisTag);
  E("AFont_getAxisValue", AxisValue);
#undef E
  return true;
}();

}  // namespace
}  // namespace gesher::bionic
