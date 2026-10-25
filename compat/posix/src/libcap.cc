// gesher-base: libcap's cap_t interface (<sys/capability.h>). A Windows
// process holds no Linux capabilities: cap_get_proc returns empty sets and
// cap_set_proc accepts only empty ones, as for an unprivileged Linux process.
#include <sys/capability.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct gesher_cap {
  unsigned long long sets[3];  // effective, permitted, inheritable
};

extern "C" {

cap_t cap_init(void) { return static_cast<cap_t>(calloc(1, sizeof(gesher_cap))); }

cap_t cap_get_proc(void) { return cap_init(); }

cap_t cap_dup(cap_t cap) {
  if (cap == nullptr) return errno = EINVAL, nullptr;
  cap_t copy = cap_init();
  if (copy != nullptr) memcpy(copy, cap, sizeof(*cap));
  return copy;
}

int cap_free(void* object) {
  free(object);
  return 0;
}

int cap_clear(cap_t cap) {
  if (cap == nullptr) return errno = EINVAL, -1;
  memset(cap, 0, sizeof(*cap));
  return 0;
}

int cap_clear_flag(cap_t cap, cap_flag_t flag) {
  if (cap == nullptr || flag < CAP_EFFECTIVE || flag > CAP_INHERITABLE) return errno = EINVAL, -1;
  cap->sets[flag] = 0;
  return 0;
}

int cap_get_flag(cap_t cap, cap_value_t value, cap_flag_t flag, cap_flag_value_t* out) {
  if (cap == nullptr || out == nullptr || value < 0 || value > CAP_LAST_CAP ||
      flag < CAP_EFFECTIVE || flag > CAP_INHERITABLE) {
    return errno = EINVAL, -1;
  }
  *out = (cap->sets[flag] >> value) & 1 ? CAP_SET : CAP_CLEAR;
  return 0;
}

int cap_set_flag(cap_t cap, cap_flag_t flag, int count, const cap_value_t* values,
                 cap_flag_value_t set) {
  if (cap == nullptr || flag < CAP_EFFECTIVE || flag > CAP_INHERITABLE || count < 0) {
    return errno = EINVAL, -1;
  }
  for (int i = 0; i < count; ++i) {
    if (values[i] < 0 || values[i] > CAP_LAST_CAP) return errno = EINVAL, -1;
    unsigned long long bit = 1ULL << values[i];
    cap->sets[flag] = set == CAP_SET ? cap->sets[flag] | bit : cap->sets[flag] & ~bit;
  }
  return 0;
}

int cap_set_proc(cap_t cap) {
  if (cap == nullptr) return errno = EINVAL, -1;
  return cap->sets[0] == 0 && cap->sets[1] == 0 && cap->sets[2] == 0 ? 0 : (errno = EPERM, -1);
}

}  // extern "C"
