// Standalone NDK font API observation. No VM or Java font bootstrap.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

typedef void *(*make_fn)(void);
typedef void *(*next_fn)(void *);
typedef void (*drop_fn)(void *);
typedef const char *(*path_fn)(const void *);
typedef void *(*match_fn)(const void *, const char *, const uint16_t *, uint32_t, uint32_t *);

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  void *lib = dlopen("libandroid.so", RTLD_NOW);
  if (!lib) { printf("fonts standalone: libandroid unavailable %s\n", dlerror()); return 1; }
#define LOAD(type, var, name) type var = (type)dlsym(lib, name)
  LOAD(make_fn, open_iter, "ASystemFontIterator_open");
  LOAD(next_fn, next, "ASystemFontIterator_next");
  LOAD(drop_fn, close_iter, "ASystemFontIterator_close");
  LOAD(drop_fn, close_font, "AFont_close");
  LOAD(path_fn, path, "AFont_getFontFilePath");
  LOAD(make_fn, create, "AFontMatcher_create");
  LOAD(drop_fn, destroy, "AFontMatcher_destroy");
  LOAD(match_fn, match, "AFontMatcher_match");
#undef LOAD
  printf("fonts standalone exports: iterator=%d matcher=%d\n", !!(open_iter && next && close_iter && close_font && path), !!(create && destroy && match));
  FILE *direct = fopen("/system/etc/fonts.xml", "rb");
  printf("fonts standalone direct system xml: %s\n", direct ? "readable" : "unavailable");
  if (direct) fclose(direct);
  if (!open_iter || !next || !close_iter || !close_font || !path || !create || !destroy || !match) return 1;
  const char *roots[] = {NULL, argv[1], "/gesher-font-root-does-not-exist"};
  for (unsigned i = 0; i < 3; ++i) {
    if (roots[i]) setenv("ANDROID_ROOT", roots[i], 1); else unsetenv("ANDROID_ROOT");
    void *it = open_iter(); unsigned count = 0, readable = 0;
    if (it) {
      void *font;
      while (count < 4096 && (font = next(it))) {
        const char *p = path(font); FILE *f = p ? fopen(p, "rb") : NULL;
        if (f) { unsigned char b[12]; if (fread(b, 1, sizeof b, f) == sizeof b) ++readable; fclose(f); }
        close_font(font); ++count;
      }
      close_iter(it);
    }
    void *m = create(); uint16_t text[] = {'A', 0x05d0}; uint32_t length = 0;
    void *font = m ? match(m, "sans-serif", text, 2, &length) : NULL;
    printf("fonts standalone root=%u iterator=%u readable=%u matched=%d run=%u\n", i, count, readable, font != NULL, length);
    if (font) close_font(font);
    if (m) destroy(m);
  }
  dlclose(lib); return 0;
}
