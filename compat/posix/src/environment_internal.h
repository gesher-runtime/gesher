// The storage/transaction part of the POSIX environment owner. Allocation
// failures and a failed native shadow write publish no table or string change.
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gesher::environment {
class Table {
 public:
  using Writer = int (*)(const char*, const char*, void*);
  void Initialize(char** entries, size_t capacity) {
    entries_ = owned_table_ = entries;
    capacity_ = capacity;
  }
  char*** Slot() { return &entries_; }
  char* Get(const char* name) const {
    if (!name || !*name || std::strchr(name, '=')) return nullptr;
    const size_t length = std::strlen(name);
    for (char** entry = entries_; entry && *entry; ++entry)
      if (Matches(*entry, name, length)) return *entry + length + 1;
    return nullptr;
  }
  char* GetNative(const char* name) const {
    if (char* exact = Get(name)) return exact;
    if (!name || !*name || std::strchr(name, '=')) return nullptr;
    const size_t length = std::strlen(name);
    for (char** entry = entries_; entry && *entry; ++entry)
      if (NativeMatches(*entry, name, length)) return *entry + length + 1;
    return nullptr;
  }
  bool BorrowedProjection(const char* name, const char** value) const {
    const size_t length = std::strlen(name);
    // Font keys retain their existing native case-alias policy. The latest
    // successful mutation of that key selects its owner; an unrelated putenv
    // must not capture a previously untracked native font key.
    Name* selected = nullptr;
    for (Name* item = names_; item; item = item->next) {
      if (item->length == length && NativeEqual(item->bytes, name, length)) {
        selected = item; break;
      }
    }
    bool borrowed = selected && selected->borrowed_name;
    uint64_t version = selected ? selected->version : 0;
    *value = nullptr;  // A selected borrowed name renamed away is absent.
    if (!borrowed_) return borrowed;  // Ordinary owned reads never scan environ.
    // A caller may rename an unrelated borrowed entry into a font key. Only
    // currently live entries are dereferenced; retired caller storage is never
    // inspected. A later explicit font write still selects its own value.
    for (char** entry = entries_; entry && *entry; ++entry) {
      if (!NativeMatches(*entry, name, length)) continue;
      for (Borrowed* item = borrowed_; item; item = item->next) {
        if (item->assignment == *entry && item->version >= version) {
          version = item->version;
          borrowed = true;
          *value = *entry + length + 1;
          break;
        }
      }
    }
    return borrowed;
  }
  int Set(const char* name, const char* value, int overwrite, Writer writer, void* context) {
    if (!name || !value) return errno = EINVAL, -1;
    return Change(name, std::strlen(name), value, nullptr, overwrite, writer, context);
  }
  int Put(char* assignment, Writer writer, void* context) {
    if (!assignment) return errno = EINVAL, -1;
    char* equals = std::strchr(assignment, '=');
    if (!equals) return Unset(assignment, writer, context);
    return Change(assignment, static_cast<size_t>(equals - assignment), equals + 1,
                  assignment, 1, writer, context);
  }
  int Unset(const char* name, Writer writer, void* context) {
    if (!name) return errno = EINVAL, -1;
    return Change(name, std::strlen(name), nullptr, nullptr, 1, writer, context);
  }
  int SetNative(const char* name, const char* value, Writer writer, void* context) {
    if (!name || !*name || std::strchr(name, '=')) return errno = EINVAL, -1;
    const size_t length = std::strlen(name);
    const char* canonical = name;
    bool exact = false;
    for (char** entry = entries_; entry && *entry; ++entry) {
      if (Matches(*entry, name, length)) { canonical = *entry; exact = true; break; }
      if (canonical == name && NativeMatches(*entry, name, length)) canonical = *entry;
    }
    if (!exact && canonical == name) {
      for (Name* item = names_; item; item = item->next)
        if (item->length == length && NativeEqual(item->bytes, name, length)) { canonical = item->bytes; break; }
    }
    return Change(canonical, length, value, nullptr, 1, writer, context);
  }

 private:
  struct Name { Name* next; size_t length; uint64_t version; bool borrowed_name; char bytes[1]; };
  struct Borrowed { Borrowed* next; char* assignment; uint64_t version; };
  static bool Matches(const char* entry, const char* name, size_t length) {
    return std::strncmp(entry, name, length) == 0 && entry[length] == '=';
  }
  static bool NativeEqual(const char* left, const char* right, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      unsigned char a = left[i], b = right[i];
      if (!a || !b) return false;
      if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
      if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
      if (a != b) return false;
    }
    return true;
  }
  static bool NativeMatches(const char* entry, const char* name, size_t length) {
    return NativeEqual(entry, name, length) && entry[length] == '=';
  }
  Name* FindName(const char* name, size_t length) const {
    for (Name* item = names_; item; item = item->next)
      if (item->length == length && std::memcmp(item->bytes, name, length) == 0) return item;
    return nullptr;
  }
  void RemoveBorrowed(char* assignment) {
    for (Borrowed** item = &borrowed_; *item; item = &(*item)->next) {
      if ((*item)->assignment != assignment) continue;
      Borrowed* retired = *item;
      *item = retired->next;
      std::free(retired);  // Metadata only; the caller owns assignment.
      return;
    }
  }
  int Change(const char* name, size_t length, const char* value, char* borrowed,
             int overwrite, Writer writer, void* context) {
    if (!length || std::memchr(name, '=', length)) return errno = EINVAL, -1;
    size_t count = 0, first = SIZE_MAX;
    while (entries_ && entries_[count]) {
      if (first == SIZE_MAX && Matches(entries_[count], name, length)) first = count;
      ++count;
    }
    if (value && !overwrite && first != SIZE_MAX) return 0;

    Name* recorded = FindName(name, length);
    Name* prepared_name = nullptr;
    if (!recorded) {
      if (length > SIZE_MAX - sizeof(Name)) return errno = ENOMEM, -1;
      prepared_name = static_cast<Name*>(std::malloc(sizeof(Name) + length));
      if (!prepared_name) return errno = ENOMEM, -1;
      prepared_name->length = length;
      prepared_name->version = 0;
      prepared_name->borrowed_name = false;
      std::memcpy(prepared_name->bytes, name, length);
      prepared_name->bytes[length] = '\0';
      recorded = prepared_name;
    }
    Borrowed* prepared_borrowed = nullptr;
    Borrowed* borrowed_record = nullptr;
    if (borrowed) {
      for (Borrowed* item = borrowed_; item; item = item->next)
        if (item->assignment == borrowed) { borrowed_record = item; break; }
      if (!borrowed_record) {
        prepared_borrowed = static_cast<Borrowed*>(std::malloc(sizeof(Borrowed)));
        if (!prepared_borrowed) { std::free(prepared_name); return errno = ENOMEM, -1; }
        prepared_borrowed->assignment = borrowed;
        borrowed_record = prepared_borrowed;
      }
    }
    // putenv replaces the pointer even when its bytes equal the old value;
    // setenv always detaches from borrowed storage, including equal values.
    char* assignment = borrowed;
    if (value && !assignment) {
      const size_t size = std::strlen(value);
      if (size > SIZE_MAX - length - 2) {
        std::free(prepared_borrowed); std::free(prepared_name); return errno = ENOMEM, -1;
      }
      assignment = static_cast<char*>(std::malloc(length + size + 2));
      if (!assignment) { std::free(prepared_borrowed); std::free(prepared_name); return errno = ENOMEM, -1; }
      std::memcpy(assignment, name, length);
      assignment[length] = '=';
      std::memcpy(assignment + length + 1, value, size + 1);
    }
    char** prepared_table = nullptr;
    size_t prepared_capacity = capacity_;
    const size_t required = count + (value && first == SIZE_MAX ? 2 : 1);
    if (entries_ != owned_table_ || required > capacity_) {
      prepared_capacity = capacity_ > SIZE_MAX / 2 ? required : capacity_ * 2;
      if (prepared_capacity < required) prepared_capacity = required;
      if (prepared_capacity > SIZE_MAX / sizeof(char*)) prepared_capacity = 0;
      if (prepared_capacity)
        prepared_table = static_cast<char**>(std::malloc(prepared_capacity * sizeof(char*)));
      if (!prepared_table) {
        if (!borrowed) std::free(assignment);
        std::free(prepared_borrowed); std::free(prepared_name); return errno = ENOMEM, -1;
      }
      if (count) std::memcpy(prepared_table, entries_, count * sizeof(char*));
      prepared_table[count] = nullptr;
    }
    const int result = writer(recorded->bytes, value, context);
    if (result != 0) {
      std::free(prepared_table);
      if (!borrowed) std::free(assignment);
      std::free(prepared_borrowed);
      std::free(prepared_name);
      return errno = result, -1;
    }
    if (prepared_table) {
      std::free(owned_table_);
      entries_ = owned_table_ = prepared_table;
      capacity_ = prepared_capacity;
    }
    size_t output = 0;
    bool inserted = false;
    for (size_t index = 0; index < count; ++index) {
      if (Matches(entries_[index], recorded->bytes, length)) {
        if (entries_[index] != borrowed) RemoveBorrowed(entries_[index]);
        if (value && !inserted) { entries_[output++] = assignment; inserted = true; }
      } else entries_[output++] = entries_[index];
    }
    if (value && !inserted) entries_[output++] = assignment;
    entries_[output] = nullptr;
    if (prepared_name) { prepared_name->next = names_; names_ = prepared_name; }
    else if (names_ != recorded) {
      Name* previous = names_;
      while (previous->next != recorded) previous = previous->next;
      previous->next = recorded->next;
      recorded->next = names_;
      names_ = recorded;
    }
    recorded->version = ++version_;
    recorded->borrowed_name = borrowed != nullptr;
    if (prepared_borrowed) { prepared_borrowed->next = borrowed_; borrowed_ = prepared_borrowed; }
    if (borrowed_record) borrowed_record->version = version_;
    // Replaced owned strings remain valid; replaced borrowed strings remain
    // solely the caller's property. Only pointer-array storage is reclaimed.
    return 0;
  }
  char** entries_ = nullptr;
  char** owned_table_ = nullptr;
  size_t capacity_ = 0;
  Name* names_ = nullptr;
  Borrowed* borrowed_ = nullptr;
  uint64_t version_ = 0;
};
}  // namespace gesher::environment
