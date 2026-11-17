#!/usr/bin/env python3
"""Execute exact production trim methods with isolated cache ownership boundaries."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--source', type=Path)
    a = p.parse_args()
    source = (a.source or ROOT / 'compat/hwui/gl/RenderThread.cpp').read_text(encoding='utf-8')
    start = source.index('void RenderThread::trimMemory(')
    end = source.index('} /* namespace renderthread */', start)
    methods = source[start:end]
    fixture = r'''
#include <cstddef>
#include <cstdio>
#include <vector>
using std::size_t;
enum class TrimLevel { RUNNING_LOW=10, UI_HIDDEN=20, BACKGROUND=40, COMPLETE=80 };
enum class CacheTrimLevel { ALL_CACHES=0, FONT_CACHE=1, RESOURCE_CACHE=2 };
enum class GrSyncCpu { kYes };
enum class GrPurgeResourceOptions { kScratchResourcesOnly, kAllResources };
struct Context {
 int flushed=0,freed=0,purged=0; size_t limit=1000;
 std::vector<size_t> limits;
 GrPurgeResourceOptions mode=GrPurgeResourceOptions::kAllResources;
 void flushAndSubmit(GrSyncCpu){++flushed;}
 void freeGpuResources(){++freed;}
 size_t getResourceCacheLimit(){return limit;}
 void setResourceCacheLimit(size_t n){limit=n;limits.push_back(n);}
 void purgeUnlockedResources(GrPurgeResourceOptions m){++purged;mode=m;}
};
struct SkGraphics {
 static inline int all=0,font=0,resource=0;
 static inline size_t fontLimit=200;
 static inline std::vector<size_t> limits;
 static void PurgeAllCaches(){++all;}
 static void PurgeFontCache(){++font;}
 static void PurgeResourceCache(){++resource;}
 static size_t GetFontCacheLimit(){return fontLimit;}
 static void SetFontCacheLimit(size_t n){fontLimit=n;limits.push_back(n);}
};
struct CacheManager { struct Policy {float backgroundRetentionPercent=.5f;bool purgeScratchOnly=true;} mMemoryPolicy; };
struct RenderThread {
 Context* mGrContext=nullptr;CacheManager cache;CacheManager* mCacheManager=&cache;int destroyed=0;
 void destroyRenderingContext(){++destroyed;mGrContext=nullptr;}
 void trimMemory(TrimLevel);
 void trimCaches(CacheTrimLevel);
};
'''
    checks = r'''
int main(){
 int fail=0;
 auto check=[&](bool ok,const char* label){if(!ok){++fail;std::printf("FAIL %s\n",label);}};
 RenderThread r;Context c;
 r.trimMemory(TrimLevel::COMPLETE);check(r.destroyed==0,"cold trim must not initialize GL");
 r.mGrContext=&c;r.trimMemory(TrimLevel::RUNNING_LOW);
 check(c.flushed==1&&c.freed==0&&r.destroyed==0,"running pressure only synchronizes");
 r.trimMemory(TrimLevel::UI_HIDDEN);
 check(c.limit==1000&&c.limits==std::vector<size_t>({500,1000}),"restore real GPU budget");
 check(SkGraphics::fontLimit==200&&SkGraphics::limits==std::vector<size_t>({100,200}),"restore CPU font budget");
 check(c.purged==1&&c.mode==GrPurgeResourceOptions::kScratchResourcesOnly,"UI hidden scratch policy");
 r.cache.mMemoryPolicy.purgeScratchOnly=false;r.trimMemory(TrimLevel::UI_HIDDEN);
 check(c.mode==GrPurgeResourceOptions::kAllResources,"UI hidden full-resource policy");
 r.trimMemory(TrimLevel::BACKGROUND);
 check(c.flushed==4&&c.freed==1&&SkGraphics::all==1&&r.destroyed==1&&!r.mGrContext,"background retires context");
 r.mGrContext=&c;r.trimMemory(TrimLevel::COMPLETE);
 check(c.freed==2&&r.destroyed==2,"complete also retires context");
 r.trimCaches(CacheTrimLevel::FONT_CACHE);check(SkGraphics::font==1,"font cache");
 r.trimCaches(CacheTrimLevel::RESOURCE_CACHE);check(SkGraphics::resource==1,"CPU resource cache");
 r.mGrContext=&c;r.trimCaches(CacheTrimLevel::ALL_CACHES);
 check(SkGraphics::all==3&&c.purged==3&&c.mode==GrPurgeResourceOptions::kAllResources,"all CPU and GPU caches");
 int before=SkGraphics::all;r.trimCaches(static_cast<CacheTrimLevel>(999));check(SkGraphics::all==before,"unknown mode ignored");
 std::printf("hwui trim: %d failures\n",fail);return fail?1:0;
}
'''
    with tempfile.TemporaryDirectory(prefix='gesher-hwui-trim-') as d:
        cpp = Path(d) / 'trim.cc'
        exe = Path(d) / ('trim.exe' if os.name == 'nt' else 'trim')
        cpp.write_text(fixture + methods + checks, encoding='utf-8')
        cmd = shlex.split(a.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', str(cpp), '-o', str(exe)]
        if a.sanitize:
            cmd += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=30)

if __name__ == '__main__':
    main()
