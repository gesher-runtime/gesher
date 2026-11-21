// An ordinary Windows/UCRT child, deliberately not linked to gesher_posix.
#include <windows.h>
#include <io.h>
#include <cstring>
#include <initializer_list>

int main(int argc, char** argv) {
  for (DWORD which : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
    HANDLE handle = GetStdHandle(which);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) return 10;
  }
  if (argc != 2) return 11;
  if (std::strcmp(argv[1], "--empty") == 0) return 0;
  if (std::strcmp(argv[1], "--pipe") == 0) {
    char byte = 0;
    if (_read(37, &byte, 1) != 1 || byte != 'P') return 16;
    if (_read(37, &byte, 1) != 0) return 17;
    return _write(38, "N", 1) == 1 ? 0 : 18;
  }
  if (std::strcmp(argv[1], "--file") != 0) return 12;
  HANDLE file = reinterpret_cast<HANDLE>(_get_osfhandle(37));
  if (!file || file == INVALID_HANDLE_VALUE) return 13;
  char byte = 0;
  DWORD count = 0;
  if (!ReadFile(file, &byte, 1, &count, nullptr) || count != 1 || byte != 'B') return 14;
  return WriteFile(file, "Z", 1, &count, nullptr) && count == 1 ? 0 : 15;
}
