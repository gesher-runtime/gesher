// Include inside the fixture's gesher namespace. These adapters exercise
// uncontained paths without a private storage lease.
namespace rooted_storage_wire {
inline constexpr uint32_t kMetadataWrite = 4;
}

namespace storage_root {
struct FixtureMatch {
  int error = 0;
  explicit operator bool() const { return false; }
};

inline FixtureMatch Lookup(const std::wstring&) {
  return {};
}

inline int Rename(const FixtureMatch&, const FixtureMatch&, bool) {
  std::abort();
}

template <typename Handle>
int Open(const FixtureMatch&, uint32_t, Handle*) {
  std::abort();
}
}  // namespace storage_root
