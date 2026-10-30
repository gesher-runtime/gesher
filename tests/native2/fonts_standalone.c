// Standalone NDK font contract. Each invocation is a fresh process, without ART.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

typedef void *(*make_fn)(void);
typedef void *(*next_fn)(void *);
typedef void (*drop_fn)(void *);
typedef const char *(*path_fn)(const void *);
typedef void *(*match_fn)(const void *, const char *, const uint16_t *, uint32_t, uint32_t *);
typedef size_t (*count_fn)(const void *);
typedef uint16_t (*weight_fn)(const void *);
typedef bool (*italic_fn)(const void *);
typedef uint32_t (*tag_fn)(const void *, uint32_t);
typedef float (*axis_fn)(const void *, uint32_t);
typedef void (*style_fn)(void *, uint16_t, bool);
typedef void (*locales_fn)(void *, const char *);
typedef void (*variant_fn)(void *, uint32_t);
static uint16_t be16(const unsigned char *p) { return (p[0]<<8)|p[1]; }
static uint32_t be32(const unsigned char *p) { return ((uint32_t)be16(p)<<16)|be16(p+2); }
static bool same_bytes(const char *a, const char *b) {
  FILE *x=fopen(a,"rb"),*y=fopen(b,"rb"); bool same=x&&y;
  if(same) for(;;) { unsigned char p[4096],q[4096]; size_t n=fread(p,1,sizeof p,x),m=fread(q,1,sizeof q,y); if(n!=m||memcmp(p,q,n)||ferror(x)||ferror(y)) { same=false; break; } if(!n) break; }
  if(x) fclose(x); if(y) fclose(y); return same;
}
static bool rooted(const char *path,const char *directory) { size_t n=strlen(directory); return strncmp(path,directory,n)==0&&path[n]=='/'; }
// Independent bounded SFNT cmap oracle: require a nonzero actual glyph, not
// only a successful matcher or a readable file. Shipped fonts use format 4/12.
static bool glyph(const char *path, uint32_t cp) {
  FILE *f=fopen(path,"rb"); if(!f) return false;
  if(fseek(f,0,SEEK_END)) { fclose(f); return false; }
  long length=ftell(f); if(length<12 || length>64*1024*1024) { fclose(f); return false; }
  rewind(f); unsigned char *b=malloc((size_t)length); if(!b) { fclose(f); return false; }
  bool ok=false;
  if(fread(b,1,(size_t)length,f)!=(size_t)length) goto done;
  unsigned n=be16(b+4);
  if(12u+16u*n>(unsigned long)length) goto done;
  for(unsigned i=0;i<n;i++) {
    unsigned char *t=b+12+16*i; if(memcmp(t,"cmap",4)) continue;
    uint32_t off=be32(t+8),size=be32(t+12);
    if(off>(unsigned long)length || size>(unsigned long)length-off || size<4) continue;
    unsigned char *c=b+off; unsigned tables=be16(c+2); if(4u+8u*tables>size) continue;
    for(unsigned j=0;j<tables;j++) {
      uint32_t sub=be32(c+4+8*j+4); if(sub>size || size-sub<4) continue;
      unsigned char *s=c+sub; unsigned fmt=be16(s); uint32_t available=size-sub;
      if(fmt==12 && available>=16) {
        uint32_t bytes=be32(s+4),groups=be32(s+12);
        if(bytes>available || bytes<16 || groups>(bytes-16)/12) continue;
        for(uint32_t k=0;k<groups;k++) { unsigned char *g=s+16+12*k; uint32_t a=be32(g),z=be32(g+4); if(cp>=a&&cp<=z&&be32(g+8)+cp-a!=0) ok=true; }
      } else if(fmt==4 && available>=16 && cp<=0xffff) {
        unsigned bytes=be16(s+2),segs=be16(s+6)/2;
        if(bytes>available || bytes<16 || segs>(bytes-16)/8) continue;
        unsigned char *ends=s+14,*starts=ends+2*segs+2,*deltas=starts+2*segs,*ranges=deltas+2*segs;
        for(unsigned k=0;k<segs;k++) if(cp>=be16(starts+2*k)&&cp<=be16(ends+2*k)) {
          uint16_t gid=0,delta=be16(deltas+2*k),range=be16(ranges+2*k);
          if(!range) gid=(uint16_t)(cp+delta);
          else { size_t pos=(size_t)(ranges+2*k-s)+range+2*(cp-be16(starts+2*k)); if(pos+2<=bytes) { gid=be16(s+pos); if(gid) gid=(uint16_t)(gid+delta); } }
          if(gid) ok=true;
        }
      }
    }
  }
done: free(b); fclose(f); return ok;
}
int main(int argc, char **argv) {
  if(argc!=5) return 2;
  bool positive=!strcmp(argv[1],"set");
  if(!strcmp(argv[1],"unset")) unsetenv("ANDROID_ROOT");
  else if(positive) setenv("ANDROID_ROOT",argv[2],1);
  else if(!strcmp(argv[1],"wrong")) setenv("ANDROID_ROOT","/gesher-font-root-does-not-exist",1);
  else return 2;
  unsetenv("ANDROID_PRODUCT_ROOT");
  void *lib=dlopen("libandroid.so",RTLD_NOW);
  if(!lib) { printf("FONT_FAIL logical-library %s\n",dlerror()); return 1; }
#define LOAD(type,var,name) type var=(type)dlsym(lib,name)
  LOAD(make_fn,open_iter,"ASystemFontIterator_open"); LOAD(next_fn,next,"ASystemFontIterator_next"); LOAD(drop_fn,close_iter,"ASystemFontIterator_close");
  LOAD(drop_fn,close_font,"AFont_close"); LOAD(path_fn,path,"AFont_getFontFilePath"); LOAD(make_fn,create,"AFontMatcher_create"); LOAD(drop_fn,destroy,"AFontMatcher_destroy"); LOAD(match_fn,match,"AFontMatcher_match");
  LOAD(weight_fn,weight,"AFont_getWeight"); LOAD(italic_fn,italic,"AFont_isItalic"); LOAD(path_fn,locale,"AFont_getLocale"); LOAD(count_fn,index,"AFont_getCollectionIndex");
  LOAD(count_fn,axes,"AFont_getAxisCount"); LOAD(tag_fn,tag,"AFont_getAxisTag"); LOAD(axis_fn,axis,"AFont_getAxisValue");
  LOAD(style_fn,style,"AFontMatcher_setStyle"); LOAD(locales_fn,locales,"AFontMatcher_setLocales"); LOAD(variant_fn,variant,"AFontMatcher_setFamilyVariant");
#undef LOAD
  if(!open_iter||!next||!close_iter||!close_font||!path||!create||!destroy||!match||!weight||!italic||!locale||!index||!axes||!tag||!axis||!style||!locales||!variant) { puts("FONT_FAIL exports"); return 1; }
  void *absolute=dlopen("/system/lib64/libandroid.so",RTLD_NOW);
  if(!absolute || dlsym(absolute,"AFontMatcher_match")!=(void*)match || dlsym(lib,"AAssetManager_open")!=NULL) { puts("FONT_FAIL logical-provider-identity"); return 1; }
  dlclose(absolute);
  FILE *direct=fopen("/system/etc/fonts.xml","rb"); bool direct_readable=direct!=NULL; if(direct) fclose(direct);
  bool root_identity=!direct_readable||same_bytes("/system/etc/fonts.xml",argv[4]);
  FILE *canary=fopen("font-unrelated-canary.txt","rb"); char canary_bytes[32]={0};
  bool unrelated=canary&&fread(canary_bytes,1,16,canary)==16&&!memcmp(canary_bytes,"unrelated-canary",16); if(canary) fclose(canary);
  unsigned count=0,axis_count=0,locales_count=0; bool valid=true;
  void *it=open_iter(),*font; if(!it) return 1;
  while(count<4096 && (font=next(it))) {
    const char *p=path(font); FILE *f=p?fopen(p,"rb"):NULL; unsigned char h[12]={0};
    valid=valid&&f&&rooted(p,argv[3])&&fread(h,1,12,f)==12&&(memcmp(h,"\0\1\0\0",4)==0||memcmp(h,"OTTO",4)==0)&&index(font)==0&&weight(font)>=1&&weight(font)<=1000;
    if(f) fclose(f);
    const char *l=locale(font); if(l&&*l) locales_count++;
    size_t a=axes(font); if(a>128) valid=false;
    for(size_t j=0;j<a&&j<128;j++) { if(!tag(font,j)||!isfinite(axis(font,j))) valid=false; axis_count++; }
    close_font(font); count++;
  }
  close_iter(it);
  void *m=create(); if(!m) return 1; locales(m,"en,he"); variant(m,2);
  uint16_t text[]={'A',0x05d0,0x05d1}; uint32_t offset=0,runs=0;
  while(offset<3) {
    uint32_t run=0; font=match(m,"sans-serif",text+offset,3-offset,&run);
    if(!font) break;
    bool good=run>=1&&run<=3-offset&&rooted(path(font),argv[3])&&glyph(path(font),text[offset]);
    if(good) for(uint32_t j=0;j<run;j++) if(!glyph(path(font),text[offset+j])) good=false;
    const char *base=strrchr(path(font),'/'); char logical[1024];
    int written=snprintf(logical,sizeof logical,"/system/fonts/%s",base?base+1:path(font));
    good=good&&written>0&&(size_t)written<sizeof logical&&glyph(logical,text[offset])&&same_bytes(path(font),logical);
    close_font(font); if(!good) { valid=false; break; } offset+=run; runs++;
  }
  style(m,700,true); uint16_t latin[]={'H','i'}; uint32_t run=0;
  font=match(m,"sans-serif",latin,2,&run); destroy(m);
  bool styled=font&&run==2&&weight(font)==700&&italic(font)&&glyph(path(font),'H');
  if(font) close_font(font);
  bool expected=strcmp(argv[1],"wrong")!=0;
  const char *invalid[]={"/system/fonts/../fontscanary.txt","/system/fonts/.. /fontscanary.txt","/system/fonts/.../fontscanary.txt","/system/fonts/..\\fonts.xml","/system/fonts/C:fonts.xml","/system/fonts/NUL.ttf","/system/etc/fonts.xml:stream"};
  bool boundaries=true;
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) { FILE *bad=fopen(invalid[i],"rb"); if(bad) { boundaries=false; fclose(bad); } }
  bool good=valid&&unrelated&&root_identity&&boundaries&&direct_readable==expected&&count<4096&&(expected?(count>10&&axis_count>0&&locales_count>0&&offset==3&&runs>0&&styled):(count==0&&offset==0&&!styled));
  printf("FONT_%s case=%s exports=18 direct=%d iterator=%u axes=%u locales=%u covered=%u runs=%u styled=%d\n",good?"PASS":"FAIL",argv[1],direct_readable,count,axis_count,locales_count,offset,runs,styled);
  dlclose(lib); return good?0:1;
}
