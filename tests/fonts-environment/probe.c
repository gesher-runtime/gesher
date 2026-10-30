// Diagnostic-only targeted environment observation. Does not qualify fallback.
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char **environ;
typedef void *(*open_fn)(void);
typedef void *(*next_fn)(void *);
typedef void (*drop_fn)(void *);
typedef const char *(*path_fn)(void *);
static const char *lookup(char **values,const char *name) {
  if(!values) return NULL;size_t n=strlen(name);
  for(unsigned i=0;i<4096&&values[i];i++) if(!strncmp(values[i],name,n)&&values[i][n]=='=') return values[i]+n+1;
  return NULL;
}
static bool equal(const char *actual,const char *wanted) {
  return !strcmp(wanted,"-unset-")?actual==NULL:actual&&!strcmp(actual,wanted);
}
static void bytes(const char *view,const char *phase,const char *key,const char *value,const char *wanted) {
  size_t length=value?strlen(value):0;
  printf("FONT_ENV_BYTES view=%s phase=%s key=%s present=%d bytes=%zu equals_expected=%d hex=",view,phase,key,value!=NULL,length,equal(value,wanted));
  for(size_t i=0;i<length&&i<256;i++) printf("%02x",(unsigned char)value[i]);
  if(length>256)printf("...TRUNCATED");putchar('\n');
}
static void snapshot(const char *phase,char **initial,const char *root,const char *override) {
  bytes("getenv",phase,"ANDROID_ROOT",getenv("ANDROID_ROOT"),root);
  bytes("getenv",phase,"GESHER_WINDOWS_FONTS",getenv("GESHER_WINDOWS_FONTS"),override);
  // Reload the guest environ pointer: JNI uses the live CRT slot; the ELF
  // executable overrides it with its separate initial UTF8 array (ADR0074).
  // Never retain/traverse an earlier CRT array across mutation.
  bytes("environ",phase,"ANDROID_ROOT",lookup(environ,"ANDROID_ROOT"),root);
  bytes("environ",phase,"GESHER_WINDOWS_FONTS",lookup(environ,"GESHER_WINDOWS_FONTS"),override);
  if(initial){bytes("initial_envp",phase,"ANDROID_ROOT",lookup(initial,"ANDROID_ROOT"),root);bytes("initial_envp",phase,"GESHER_WINDOWS_FONTS",lookup(initial,"GESHER_WINDOWS_FONTS"),override);}
  else puts("FONT_ENV_INITIAL_ENVP not_applicable=JNI shared_environ_live_slot=1");
}
int font_environment_probe(int argc,char **argv,char **initial) {
  if(argc!=9)return 2;bool dex=!strcmp(argv[1],"dex"),post=!strcmp(argv[3],"post-setenv");
  if(!dex&&strcmp(argv[1],"elf"))return 2;
  if(!post&&strcmp(argv[3],"launch-wide"))return 2;
  printf("FONT_ENV_NATIVE_ENTER context=%s case=%s mode=%s initial_envp_available=%d observation_only=1\n",argv[1],argv[2],argv[3],initial!=NULL);
  snapshot("initial",initial,argv[6],argv[7]);
  if(post){
    if((!strcmp(argv[4],"-unset-")?unsetenv("ANDROID_ROOT"):setenv("ANDROID_ROOT",argv[4],1))||(!strcmp(argv[5],"-unset-")?unsetenv("GESHER_WINDOWS_FONTS"):setenv("GESHER_WINDOWS_FONTS",argv[5],1)))return 1;
  }
  snapshot("api-entry",initial,argv[4],argv[5]);
  bool root_equal=equal(getenv("ANDROID_ROOT"),argv[4]),override_equal=equal(getenv("GESHER_WINDOWS_FONTS"),argv[5]);
  void *library=dlopen("libandroid.so",RTLD_NOW);if(!library){printf("FONT_ENV_FAIL libandroid=%s\n",dlerror());return 1;}
  open_fn open=(open_fn)dlsym(library,"ASystemFontIterator_open");next_fn next=(next_fn)dlsym(library,"ASystemFontIterator_next");
  drop_fn close=(drop_fn)dlsym(library,"ASystemFontIterator_close"),drop=(drop_fn)dlsym(library,"AFont_close");path_fn path=(path_fn)dlsym(library,"AFont_getFontFilePath");
  if(!open||!next||!close||!drop||!path||(dlsym(library,"AAssetManager_open")!=NULL)!=dex)return 1;
  void *iterator=open();if(!iterator)return 1;unsigned count=0;bool prefix=true,readable=true;void *font;
  while((font=next(iterator))!=NULL){
    if(++count>4096){drop(font);close(iterator);return 1;}
    const char *file=path(font);size_t n=strlen(argv[8]);if(!file||strncmp(file,argv[8],n)||file[n]!='/')prefix=false;
    FILE *stream=file?fopen(file,"rb"):NULL;unsigned char magic[4];if(!stream||fread(magic,1,4,stream)!=4)readable=false;if(stream)fclose(stream);
    drop(font);
  }
  close(iterator);dlclose(library);
  printf("FONT_ENV_FONT_OBSERVATION context=%s case=%s getenv_root_equal=%d getenv_override_equal=%d iterator=%u all_prefix=%d files_readable=%d fallback_functionality_qualified=0\n",argv[1],argv[2],root_equal,override_equal,count,prefix,readable);
  if((!strcmp(argv[2],"ascii-launch")||post)&&(!root_equal||!override_equal||count<10||!prefix||!readable))return 1;
  printf("FONT_ENV_OBSERVED context=%s case=%s observation_only=1 unicode_environment_conformance=false\n",argv[1],argv[2]);return 0;
}
#ifndef FONT_ENV_SHARED
int main(int argc,char **argv,char **initial){return font_environment_probe(argc,argv,initial);}
#endif
