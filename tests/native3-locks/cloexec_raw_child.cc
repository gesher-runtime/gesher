// CI-only raw kernel HANDLE oracle. No CRT, Gesher, static constructors,
// console API, descriptor translation, or process creation belongs here.
// GUI subsystem plus raw entry avoids automatic console startup; output is
// solely the explicit STARTF_USESTDHANDLES pipe supplied by the parent.
#include <windows.h>

static_assert(sizeof(void*) == 8, "Win64-only raw inheritance oracle");

namespace {
bool Decimal(const wchar_t** cursor, ULONGLONG* result) {
  const wchar_t* p = *cursor;
  if (*p < L'0' || *p > L'9') return false;
  ULONGLONG value = 0;
  while (*p >= L'0' && *p <= L'9') {
    unsigned digit = static_cast<unsigned>(*p++ - L'0');
    if (value > (~ULONGLONG{0} - digit) / 10) return false;
    value = value * 10 + digit;
  }
  if (*p && *p != L' ') return false;
  *result = value; *cursor = p;
  return true;
}
bool Space(const wchar_t** cursor) {
  if (**cursor != L' ') return false;
  ++*cursor;
  return **cursor && **cursor != L' ';
}
bool Hex16(const wchar_t** cursor, unsigned char* bytes) {
  const wchar_t* p = *cursor;
  for (unsigned i = 0; i < 16; ++i) {
    unsigned value = 0;
    for (unsigned nibble = 0; nibble < 2; ++nibble) {
      wchar_t c = *p++;
      unsigned digit;
      if (c >= L'0' && c <= L'9') digit = c - L'0';
      else if (c >= L'a' && c <= L'f') digit = c - L'a' + 10;
      else return false;
      value = value * 16 + digit;
    }
    bytes[i] = static_cast<unsigned char>(value);
  }
  if (*p && *p != L' ') return false;
  *cursor = p;
  return true;
}
bool SameFile(HANDLE handle, ULONGLONG volume, const unsigned char* id,
              const unsigned char* nonce) {
  FILE_ID_INFO actual;
  if (!GetFileInformationByHandleEx(handle, FileIdInfo, &actual, sizeof(actual)) ||
      actual.VolumeSerialNumber != volume) return false;
  for (unsigned i = 0; i < 16; ++i)
    if (actual.FileId.Identifier[i] != id[i]) return false;
  LARGE_INTEGER zero; zero.QuadPart = 0;
  unsigned char bytes[16]; DWORD read;
  if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) ||
      !ReadFile(handle, bytes, sizeof(bytes), &read, nullptr) || read != sizeof(bytes)) return false;
  for (unsigned i = 0; i < 16; ++i) if (bytes[i] != nonce[i]) return false;
  return true;
}
bool Text(HANDLE output, const char* text) {
  DWORD length = 0, written;
  while (text[length]) ++length;
  return WriteFile(output, text, length, &written, nullptr) && written == length;
}
DWORD Child() {
  const wchar_t* p = GetCommandLineW();
  if (!p || *p++ != L'"') return 92;
  while (*p && *p != L'"') ++p;
  if (*p++ != L'"' || !Space(&p)) return 92;
  ULONGLONG candidate, positive, negative, volume;
  unsigned char nonce[16], id[16];
  if (!Decimal(&p, &candidate) || !Space(&p) || !Decimal(&p, &positive) || !Space(&p) ||
      !Decimal(&p, &negative) || !Space(&p) || !Hex16(&p, nonce) || !Space(&p) ||
      !Decimal(&p, &volume) || !Space(&p) || !Hex16(&p, id) || *p) return 92;
  bool inherited = SameFile(reinterpret_cast<HANDLE>(positive), volume, id, nonce);
  bool excluded = !SameFile(reinterpret_cast<HANDLE>(negative), volume, id, nonce);
  bool visible = SameFile(reinterpret_cast<HANDLE>(candidate), volume, id, nonce);
  HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);  // Explicit inherited pipe, never console I/O.
  if (!Text(output, "CLOEXEC_CHILD intended-inherited=") || !Text(output, inherited ? "1" : "0") ||
      !Text(output, " intended-noninherited-excluded=") || !Text(output, excluded ? "1" : "0") ||
      !Text(output, " candidate-visible=") || !Text(output, visible ? "1" : "0") ||
      !Text(output, " exact-file-id-and-nonce=true\n")) return 94;
  if (!inherited || !excluded) return 94;
  return visible ? 23 : 0;
}
}  // namespace

extern "C" void WINAPI cloexec_child_entry() {
  ExitProcess(Child());
}
