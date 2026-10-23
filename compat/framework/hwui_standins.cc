// Gesher: stand-ins for hwui's natives, until views are drawn.
//
// The tables are generated from hwui's JNI sources (gen_hwui_standins.py):
// every native answers zero, false, null or 0.0, except the ones below. They
// are System V functions, as ART calls natives
// (docs/decisions/0006-managed-abi.md), with C linkage so the generated tables
// can name them without their types; the shared answers take no parameters,
// which System V's caller-pops convention allows for any signature.
// register_gesher_hwui_standins registers each table whose class is on the
// class path.

#include <jni.h>
#include <math.h>
#include <stdlib.h>

#include <new>
#include <vector>

#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#define SYSV __attribute__((sysv_abi))

namespace gesher_hwui {

struct StandinTable {
  const char* className;
  const JNINativeMethod* methods;
  int count;
};
extern const StandinTable kStandinTables[];
extern const int kStandinTableCount;

// The objects are opaque to Java; a small zeroed block stands for each, freed
// by NativeAllocationRegistry with this function (Microsoft ABI, as libcore
// calls it).
static void FreeBlock(void* ptr) {
  free(ptr);
}

// --- Path ----------------------------------------------------------------------
// Path keeps its contours, as interpolators (PathInterpolator) build curves and
// flatten them with approximate(); hwui's SkPathGlue does the same over Skia.

struct Point {
  float x, y;
};

struct Path {
  enum Verb { kMove, kLine, kQuad, kCubic, kClose };
  std::vector<Verb> verbs;
  std::vector<Point> points;  // the points each verb adds (1, 1, 2, 3, 0)
  Point last{0, 0};
  Point contourStart{0, 0};

  void moveTo(float x, float y) {
    verbs.push_back(kMove);
    points.push_back({x, y});
    last = contourStart = {x, y};
  }
  void ensureMove() {
    if (verbs.empty() || verbs.back() == kClose) moveTo(last.x, last.y);
  }
  void lineTo(float x, float y) {
    ensureMove();
    verbs.push_back(kLine);
    points.push_back({x, y});
    last = {x, y};
  }
  void quadTo(float x1, float y1, float x2, float y2) {
    ensureMove();
    verbs.push_back(kQuad);
    points.push_back({x1, y1});
    points.push_back({x2, y2});
    last = {x2, y2};
  }
  void cubicTo(float x1, float y1, float x2, float y2, float x3, float y3) {
    ensureMove();
    verbs.push_back(kCubic);
    points.push_back({x1, y1});
    points.push_back({x2, y2});
    points.push_back({x3, y3});
    last = {x3, y3};
  }
  void close() {
    if (!verbs.empty() && verbs.back() != kClose) {
      verbs.push_back(kClose);
      last = contourStart;
    }
  }
};

static void FreePath(void* ptr) {
  delete static_cast<Path*>(ptr);
}

static Path* AsPath(jlong handle) {
  return reinterpret_cast<Path*>(handle);
}

// approximate(): the points along the path, each as (fraction of the length,
// x, y), as SkPathGlue::approximate returns them. Curves are subdivided until
// each piece's control points lie within the acceptable error of its chord.
class Approximation {
 public:
  explicit Approximation(float error) : error_(error) {}

  void move(Point p) {
    lengths_.push_back(points_.empty() ? 0 : lengths_.back());  // a move adds no length
    points_.push_back(p);
  }
  void line(Point p) {
    if (points_.empty()) {
      move(p);
      return;
    }
    Point q = points_.back();
    lengths_.push_back(lengths_.back() + hypotf(p.x - q.x, p.y - q.y));
    points_.push_back(p);
  }
  void cubic(Point p0, Point p1, Point p2, Point p3, int depth = 0) {
    if (depth >= 16 || (Distance(p1, p0, p3) <= error_ && Distance(p2, p0, p3) <= error_)) {
      line(p3);
      return;
    }
    Point a = Mid(p0, p1), b = Mid(p1, p2), c = Mid(p2, p3);
    Point d = Mid(a, b), e = Mid(b, c), m = Mid(d, e);
    cubic(p0, a, d, m, depth + 1);
    cubic(m, e, c, p3, depth + 1);
  }
  void quad(Point p0, Point p1, Point p2) {
    // The quadratic as the cubic with the same curve.
    Point c1{p0.x + 2.0f / 3 * (p1.x - p0.x), p0.y + 2.0f / 3 * (p1.y - p0.y)};
    Point c2{p2.x + 2.0f / 3 * (p1.x - p2.x), p2.y + 2.0f / 3 * (p1.y - p2.y)};
    cubic(p0, c1, c2, p2);
  }

  jfloatArray toArray(JNIEnv* env) {
    if (points_.empty()) move({0, 0});
    float total = lengths_.back();
    if (total == 0) {
      // A lone move still animates, at one value.
      points_.push_back(points_.back());
      lengths_.push_back(1);
      total = 1;
    }
    std::vector<float> out;
    out.reserve(points_.size() * 3);
    for (size_t i = 0; i < points_.size(); i++) {
      out.push_back(lengths_[i] / total);
      out.push_back(points_[i].x);
      out.push_back(points_[i].y);
    }
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(out.size()));
    if (result != nullptr) {
      env->SetFloatArrayRegion(result, 0, static_cast<jsize>(out.size()), out.data());
    }
    return result;
  }

 private:
  static Point Mid(Point a, Point b) { return {(a.x + b.x) / 2, (a.y + b.y) / 2}; }
  // The distance from p to the segment a-b.
  static float Distance(Point p, Point a, Point b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len2 = dx * dx + dy * dy;
    float t = len2 == 0 ? 0 : ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return hypotf(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
  }

  float error_;
  std::vector<Point> points_;
  std::vector<float> lengths_;
};

}  // namespace gesher_hwui

using gesher_hwui::AsPath;

extern "C" {

// --- shared answers ----------------------------------------------------------------

SYSV jlong gesher_hwui_Standin_zero() {
  return 0;
}

SYSV jdouble gesher_hwui_Standin_zeroFloat() {
  return 0.0;
}

SYSV jlong gesher_hwui_Standin_true() {
  return JNI_TRUE;
}

// A float result travels in the low 32 bits of xmm0, so this puts 1.0f's bits
// there.
SYSV jdouble gesher_hwui_Standin_oneFloat() {
  union {
    jdouble d;
    jfloat f[2];
  } u = {};
  u.f[0] = 1.0f;
  return u.d;
}

// RenderNode.nCreate, Paint.nInit, Matrix.nCreate and the like: the object,
// with no state (it lives in Java, or nowhere until views are drawn).
SYSV jlong gesher_hwui_Standin_create() {
  return reinterpret_cast<jlong>(calloc(1, 64));
}

SYSV jlong gesher_hwui_Standin_getFreeFunction() {
  return reinterpret_cast<jlong>(&gesher_hwui::FreeBlock);
}

// --- Path ----------------------------------------------------------------------

SYSV jlong gesher_hwui_Path_init(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(new (std::nothrow) gesher_hwui::Path());
}

SYSV jlong gesher_hwui_Path_initCopy(JNIEnv*, jclass, jlong src) {
  return reinterpret_cast<jlong>(new (std::nothrow) gesher_hwui::Path(*AsPath(src)));
}

SYSV jlong gesher_hwui_Path_getFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&gesher_hwui::FreePath);
}

SYSV void gesher_hwui_Path_set(JNIEnv*, jclass, jlong dst, jlong src) {
  *AsPath(dst) = *AsPath(src);
}

SYSV void gesher_hwui_Path_reset(jlong path) {
  *AsPath(path) = gesher_hwui::Path();
}

SYSV jboolean gesher_hwui_Path_isEmpty(jlong path) {
  return AsPath(path)->verbs.empty() ? JNI_TRUE : JNI_FALSE;
}

SYSV void gesher_hwui_Path_moveTo(JNIEnv*, jclass, jlong path, jfloat x, jfloat y) {
  AsPath(path)->moveTo(x, y);
}

SYSV void gesher_hwui_Path_rMoveTo(JNIEnv*, jclass, jlong path, jfloat dx, jfloat dy) {
  auto* p = AsPath(path);
  p->moveTo(p->last.x + dx, p->last.y + dy);
}

SYSV void gesher_hwui_Path_lineTo(JNIEnv*, jclass, jlong path, jfloat x, jfloat y) {
  AsPath(path)->lineTo(x, y);
}

SYSV void gesher_hwui_Path_rLineTo(JNIEnv*, jclass, jlong path, jfloat dx, jfloat dy) {
  auto* p = AsPath(path);
  p->lineTo(p->last.x + dx, p->last.y + dy);
}

SYSV void gesher_hwui_Path_quadTo(JNIEnv*, jclass, jlong path, jfloat x1, jfloat y1, jfloat x2,
                                  jfloat y2) {
  AsPath(path)->quadTo(x1, y1, x2, y2);
}

SYSV void gesher_hwui_Path_rQuadTo(JNIEnv*, jclass, jlong path, jfloat dx1, jfloat dy1,
                                   jfloat dx2, jfloat dy2) {
  auto* p = AsPath(path);
  float x = p->last.x, y = p->last.y;
  p->quadTo(x + dx1, y + dy1, x + dx2, y + dy2);
}

SYSV void gesher_hwui_Path_cubicTo(JNIEnv*, jclass, jlong path, jfloat x1, jfloat y1, jfloat x2,
                                   jfloat y2, jfloat x3, jfloat y3) {
  AsPath(path)->cubicTo(x1, y1, x2, y2, x3, y3);
}

SYSV void gesher_hwui_Path_rCubicTo(JNIEnv*, jclass, jlong path, jfloat dx1, jfloat dy1,
                                    jfloat dx2, jfloat dy2, jfloat dx3, jfloat dy3) {
  auto* p = AsPath(path);
  float x = p->last.x, y = p->last.y;
  p->cubicTo(x + dx1, y + dy1, x + dx2, y + dy2, x + dx3, y + dy3);
}

SYSV void gesher_hwui_Path_close(JNIEnv*, jclass, jlong path) {
  AsPath(path)->close();
}

SYSV jfloatArray gesher_hwui_Path_approximate(JNIEnv* env, jclass, jlong path,
                                              jfloat acceptableError) {
  using gesher_hwui::Path;
  const Path* p = AsPath(path);
  gesher_hwui::Approximation a(acceptableError);
  size_t pt = 0;
  gesher_hwui::Point current{0, 0}, start{0, 0};
  for (Path::Verb verb : p->verbs) {
    switch (verb) {
      case Path::kMove:
        current = start = p->points[pt++];
        a.move(current);
        break;
      case Path::kLine:
        current = p->points[pt++];
        a.line(current);
        break;
      case Path::kQuad:
        a.quad(current, p->points[pt], p->points[pt + 1]);
        current = p->points[pt + 1];
        pt += 2;
        break;
      case Path::kCubic:
        a.cubic(current, p->points[pt], p->points[pt + 1], p->points[pt + 2]);
        current = p->points[pt + 2];
        pt += 3;
        break;
      case Path::kClose:
        a.line(start);
        current = start;
        break;
    }
  }
  return a.toArray(env);
}

}  // extern "C"

int register_gesher_hwui_standins(JNIEnv* env) {
  for (int i = 0; i < gesher_hwui::kStandinTableCount; i++) {
    const auto& t = gesher_hwui::kStandinTables[i];
    jclass clazz = env->FindClass(t.className);
    if (clazz == nullptr) {
      env->ExceptionClear();
      ALOGI("libandroid_runtime: %s not on the class path; its hwui stand-ins skipped",
            t.className);
      continue;
    }
    env->DeleteLocalRef(clazz);
    if (jniRegisterNativeMethods(env, t.className, t.methods, t.count) < 0) {
      ALOGE("libandroid_runtime: registering hwui stand-ins for %s failed", t.className);
      return -1;
    }
  }
  return 0;
}
