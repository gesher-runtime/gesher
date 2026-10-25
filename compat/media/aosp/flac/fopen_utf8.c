// libFLAC's share/compat.h maps fopen to fopen_utf8 on _WIN32; the real one
// lives in src/share/win_utf8_io, which Gesher does not build (the extractor
// decodes through callbacks only). This is the same contract: a UTF-8 path,
// opened with _wfopen.
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static wchar_t *gesher_flac_wide(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
  if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    free(w);
    return NULL;
  }
  return w;
}

FILE *fopen_utf8(const char *filename, const char *mode);

FILE *fopen_utf8(const char *filename, const char *mode) {
  wchar_t *wf = gesher_flac_wide(filename);
  wchar_t *wm = gesher_flac_wide(mode);
  FILE *f = (wf && wm) ? _wfopen(wf, wm) : NULL;
  free(wf);
  free(wm);
  return f;
}
