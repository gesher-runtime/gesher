// Gesher (surfaces track, ADR 0056): android.graphics.Bitmap across Parcels.
//
// hwui's Bitmap_writeToParcel and Bitmap_createFromParcel are Linux-only
// (they write through the NDK's AParcel and ashmem) and throw "Cannot use
// parcels outside of Linux" on the host build. These replace them with the
// same layout written through android.os.Parcel's Java methods:
//
//   int32 mutable, int32 SkColorType, int32 SkAlphaType,
//   int32 color space present, [byte[] SkColorSpace::serialize()],
//   int32 width, int32 height, int32 rowBytes, int32 density,
//   int64 source id, int64 parcel id,
//   int32 blob type: 0 in place, then byte[] pixels;
//                    1 shared memory, then int32 size and an android.os.SharedMemory
//                    (a memfd travelling as a file descriptor, ashmem's role).
//
// Pixels go in place up to 12 KiB, and always when the Parcel may not carry
// file descriptors (an Intent's extras, e.g. ACTION_IMAGE_CAPTURE's
// thumbnail in setResult), as Android writes its blobs; larger ones go
// through a sealed memfd with the same SharedMemory descriptor layout. The
// reader owns its own mapped Bitmap; immutable mapped storage is forwarded by
// descriptor even for small Bitmaps. Natives are JNICALL (System V).

#include "shared_bitmap.h"

#include <errno.h>
#include <jni.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <vector>

#include <SkBitmap.h>
#include <SkColorSpace.h>
#include <SkData.h>
#include <SkImageInfo.h>

#include <GraphicsJNI.h>  // libs/hwui/jni (and its Bitmap.h: createBitmap, toBitmap)
#include <hwui/Bitmap.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/JNIPlatformHelp.h>

namespace {

using android::Bitmap;
using gesher::hwui::SharedBitmapRegion;

constexpr int32_t kBlobInPlace = 0;
constexpr int32_t kBlobSharedMemory = 1;
constexpr size_t kInPlaceLimit = 12 * 1024;  // hwui's BLOB_INPLACE_LIMIT

struct {
  jclass parcel;
  jmethodID write_int, write_long, write_byte_array, read_int, read_long, create_byte_array;
  jmethodID push_allow_fds, restore_allow_fds, write_fd;
  jclass shared_memory;
  jmethodID sm_get_fd, sm_close;
  jobject sm_creator;  // SharedMemory.CREATOR
  jmethodID creator_create;
} g;

bool Failed(JNIEnv* env) {
  return env->ExceptionCheck() == JNI_TRUE;
}

// SharedMemory owns the descriptor read from the Parcel. The native Bitmap
// acquires an independent duplicate/mapping before this Java owner is closed.
// Preserve the first exception while retiring the temporary Java owner.
struct SharedMemoryMapping {
  JNIEnv* env;
  jobject memory;

  void Close() {
    jthrowable failure = env->ExceptionOccurred();
    if (failure != nullptr) env->ExceptionClear();
    auto retain_failure = [&] {
      jthrowable next = env->ExceptionOccurred();
      if (next == nullptr) return;
      env->ExceptionClear();
      if (failure == nullptr) failure = next;
      else env->DeleteLocalRef(next);
    };
    if (memory != nullptr) {
      env->CallVoidMethod(memory, g.sm_close);
      retain_failure();
      env->DeleteLocalRef(memory);
      memory = nullptr;
    }
    if (failure != nullptr) {
      env->Throw(failure);
      env->DeleteLocalRef(failure);
    }
  }

  ~SharedMemoryMapping() { Close(); }
};

// Whether `parcel` may carry file descriptors (pushAllowFds answers the old value).
bool AllowsFds(JNIEnv* env, jobject parcel) {
  jboolean old = env->CallBooleanMethod(parcel, g.push_allow_fds, JNI_FALSE);
  if (Failed(env)) {
    env->ExceptionClear();
    return false;
  }
  env->CallVoidMethod(parcel, g.restore_allow_fds, old);
  return old == JNI_TRUE;
}

void WriteBytes(JNIEnv* env, jobject parcel, const void* data, size_t size,
                size_t copied) {
  if (Failed(env)) return;
  jbyteArray a = env->NewByteArray(static_cast<jsize>(size));
  if (a == nullptr) return;
  env->SetByteArrayRegion(a, 0, static_cast<jsize>(copied), static_cast<const jbyte*>(data));
  if (!Failed(env)) env->CallVoidMethod(parcel, g.write_byte_array, a);
  env->DeleteLocalRef(a);
}

uint64_t NextParcelId() {
  static std::atomic<uint32_t> counter{0};
  return (static_cast<uint64_t>(getpid()) << 32) | counter.fetch_add(1);
}

// Same bytes as SharedMemory.writeToParcel: Parcel.writeFileDescriptor duplicates
// the descriptor. The temporary Java FileDescriptor is only a borrowed wrapper;
// the native Bitmap/region remains alive throughout this call.
bool WriteSharedFd(JNIEnv* env, jobject parcel, int fd, size_t size) {
  jobject descriptor = jniCreateFileDescriptor(env, fd);
  if (descriptor == nullptr) return false;
  env->CallVoidMethod(parcel, g.write_int, kBlobSharedMemory);
  if (!Failed(env)) env->CallVoidMethod(parcel, g.write_int, static_cast<jint>(size));
  if (!Failed(env)) env->CallVoidMethod(parcel, g.write_fd, descriptor);
  env->DeleteLocalRef(descriptor);
  return !Failed(env);
}

jboolean JNICALL Bitmap_writeToParcel(JNIEnv* env, jobject, jlong bitmap_handle, jint density,
                                      jobject parcel) {
  if (parcel == nullptr) return JNI_FALSE;
  Bitmap& native = android::bitmap::toBitmap(bitmap_handle);
  SkBitmap bitmap;
  native.getSkBitmap(&bitmap);
  const size_t size = bitmap.computeByteSize();
  size_t allocation = 0;
  if (size == 0 || bitmap.getPixels() == nullptr ||
      !Bitmap::computeAllocationSize(bitmap.rowBytes(), bitmap.height(), &allocation) ||
      size > allocation) {
    jniThrowRuntimeException(env, "Could not copy bitmap to parcel blob.");
    return JNI_FALSE;
  }
  const int fd = native.getAshmemFd();
  const bool fds = (size > kInPlaceLimit || fd >= 0) && AllowsFds(env, parcel);
  const bool shared_copy = size > kInPlaceLimit && fds;
  // As hwui: a copy into shared memory is written as immutable.
  const bool as_mutable = !bitmap.isImmutable() && !shared_copy;
  auto write_int = [&](jint value) {
    if (!Failed(env)) env->CallVoidMethod(parcel, g.write_int, value);
  };
  auto write_long = [&](jlong value) {
    if (!Failed(env)) env->CallVoidMethod(parcel, g.write_long, value);
  };

  write_int(as_mutable ? 1 : 0);
  write_int(static_cast<jint>(bitmap.colorType()));
  write_int(static_cast<jint>(bitmap.alphaType()));
  if (SkColorSpace* cs = bitmap.colorSpace()) {
    sk_sp<SkData> data = cs->serialize();
    write_int(1);
    WriteBytes(env, parcel, data->data(), data->size(), data->size());
  } else {
    write_int(0);
  }
  write_int(bitmap.width());
  write_int(bitmap.height());
  write_int(static_cast<jint>(bitmap.rowBytes()));
  write_int(density);
  write_long(static_cast<jlong>(native.getId()));
  write_long(static_cast<jlong>(NextParcelId()));
  if (Failed(env)) return JNI_FALSE;

  if (fd >= 0 && fds && bitmap.isImmutable()) {
    const size_t mapped_size = native.getAllocationByteCount();
    if (mapped_size < allocation || mapped_size > INT_MAX) {
      jniThrowRuntimeException(env, "Could not write bitmap blob file descriptor.");
      return JNI_FALSE;
    }
    return WriteSharedFd(env, parcel, fd, mapped_size) ? JNI_TRUE : JNI_FALSE;
  }
  if (shared_copy) {
    auto region = SharedBitmapRegion::Allocate(allocation);
    if (!region) {
      jniThrowRuntimeException(env, "Could not allocate bitmap shared memory.");
      return JNI_FALSE;
    }
    // Skia's readable byte size excludes the final row's padding. The new memfd
    // is zero-filled; preserve the row stride without reading beyond those bytes.
    memcpy(region.pixels(), bitmap.getPixels(), size);
    return WriteSharedFd(env, parcel, region.fd(), allocation) ? JNI_TRUE : JNI_FALSE;
  }
  write_int(kBlobInPlace);
  WriteBytes(env, parcel, bitmap.getPixels(), allocation, size);
  return Failed(env) ? JNI_FALSE : JNI_TRUE;
}

void ThrowBadParcelable(JNIEnv* env, const char* what) {
  jniThrowException(env, "android/os/BadParcelableException", what);
}

jobject JNICALL Bitmap_createFromParcel(JNIEnv* env, jobject, jobject parcel) {
  if (parcel == nullptr) {
    jniThrowNullPointerException(env, "parcel cannot be null");
    return nullptr;
  }
  auto read_int = [&] {
    if (Failed(env)) return jint{0};
    const jint value = env->CallIntMethod(parcel, g.read_int);
    return Failed(env) ? jint{0} : value;
  };
  auto read_long = [&] {
    if (Failed(env)) return jlong{0};
    const jlong value = env->CallLongMethod(parcel, g.read_long);
    return Failed(env) ? jlong{0} : value;
  };
  const bool is_mutable = read_int() != 0;
  const auto color_type = static_cast<SkColorType>(read_int());
  const auto alpha_type = static_cast<SkAlphaType>(read_int());
  sk_sp<SkColorSpace> color_space;
  if (read_int() != 0) {
    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(parcel, g.create_byte_array));
    if (Failed(env)) return nullptr;
    if (bytes != nullptr) {
      jsize n = env->GetArrayLength(bytes);
      std::vector<uint8_t> data(static_cast<size_t>(n));
      env->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(data.data()));
      env->DeleteLocalRef(bytes);
      color_space = SkColorSpace::Deserialize(data.data(), data.size());
    }
  }
  const int32_t width = read_int();
  const int32_t height = read_int();
  const int32_t row_bytes = read_int();
  const int32_t density = read_int();
  const int64_t source_id = read_long();
  read_long();  // the parcel id (tracing)
  const int32_t blob = read_int();
  if (Failed(env)) return nullptr;

  if (color_type != kN32_SkColorType && color_type != kRGBA_F16_SkColorType &&
      color_type != kRGB_565_SkColorType && color_type != kARGB_4444_SkColorType &&
      color_type != kAlpha_8_SkColorType) {
    ThrowBadParcelable(env, "Bitmap_createFromParcel unknown colortype");
    return nullptr;
  }
  SkImageInfo info = SkImageInfo::Make(width, height, color_type, alpha_type, color_space);
  size_t allocation = 0;
  if (width <= 0 || height <= 0 || row_bytes <= 0 || !SkBitmap().setInfo(info, row_bytes) ||
      !Bitmap::computeAllocationSize(row_bytes, height, &allocation)) {
    ThrowBadParcelable(env, "Received bad bitmap size");
    return nullptr;
  }
  sk_sp<Bitmap> native;

  if (blob == kBlobInPlace) {
    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(parcel, g.create_byte_array));
    if (Failed(env)) return nullptr;
    if (bytes == nullptr || static_cast<size_t>(env->GetArrayLength(bytes)) < allocation) {
      if (bytes != nullptr) env->DeleteLocalRef(bytes);
      ThrowBadParcelable(env, "Bitmap_createFromParcel: the pixels are missing or short");
      return nullptr;
    }
    native = Bitmap::allocateHeapBitmap(allocation, info, row_bytes);
    if (native == nullptr) {
      env->DeleteLocalRef(bytes);
      jniThrowRuntimeException(env, "Could not allocate bitmap data.");
      return nullptr;
    }
    env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(allocation),
                            static_cast<jbyte*>(native->pixels()));
    env->DeleteLocalRef(bytes);
  } else if (blob == kBlobSharedMemory) {
    const int32_t size = read_int();
    if (Failed(env)) return nullptr;
    if (size < 0 || static_cast<size_t>(size) < allocation) {
      ThrowBadParcelable(env, "Bitmap_createFromParcel: bad shared memory size");
      return nullptr;
    }
    jobject sm = env->CallObjectMethod(g.sm_creator, g.creator_create, parcel);
    if (Failed(env)) return nullptr;
    if (sm == nullptr) {
      ThrowBadParcelable(env, "Bitmap_createFromParcel: bad shared memory");
      return nullptr;
    }
    SharedMemoryMapping mapping{env, sm};
    jobject descriptor = env->CallObjectMethod(sm, g.sm_get_fd);
    if (Failed(env)) return nullptr;
    const int fd = jniGetFDFromFileDescriptor(env, descriptor);
    if (descriptor != nullptr) env->DeleteLocalRef(descriptor);
    if (Failed(env)) return nullptr;
    auto region = SharedBitmapRegion::Map(fd, static_cast<size_t>(size), !is_mutable);
    if (!region) {
      if (errno == EOVERFLOW) {
        ThrowBadParcelable(env, "Bitmap_createFromParcel: shared memory mapping is too short");
      } else {
        jniThrowRuntimeException(env, "Could not map bitmap shared memory.");
      }
      return nullptr;
    }
    native = region.TakeBitmap(info, row_bytes, !is_mutable);
    if (native == nullptr) {
      jniThrowRuntimeException(env, "Could not allocate bitmap data.");
      return nullptr;
    }
    mapping.Close();
    if (Failed(env)) return nullptr;
  } else {
    ThrowBadParcelable(env, "Bitmap_createFromParcel: unknown blob type");
    return nullptr;
  }
  if (Failed(env)) return nullptr;
  native->setSourceId(static_cast<uint64_t>(source_id));
  int flags = android::bitmap::kBitmapCreateFlag_Premultiplied;
  if (is_mutable) flags |= android::bitmap::kBitmapCreateFlag_Mutable;
  return android::bitmap::createBitmap(env, native.release(), flags, nullptr, nullptr, density);
}

const JNINativeMethod kMethods[] = {
    {"nativeCreateFromParcel", "(Landroid/os/Parcel;)Landroid/graphics/Bitmap;",
     reinterpret_cast<void*>(&Bitmap_createFromParcel)},
    {"nativeWriteToParcel", "(JILandroid/os/Parcel;)Z",
     reinterpret_cast<void*>(&Bitmap_writeToParcel)},
};

bool Lookup(JNIEnv* env) {
  jclass parcel = env->FindClass("android/os/Parcel");
  jclass sm = env->FindClass("android/os/SharedMemory");
  jclass creator = env->FindClass("android/os/Parcelable$Creator");
  if (parcel == nullptr || sm == nullptr || creator == nullptr) {
    env->ExceptionClear();
    return false;
  }
  g.parcel = static_cast<jclass>(env->NewGlobalRef(parcel));
  g.write_int = env->GetMethodID(parcel, "writeInt", "(I)V");
  g.write_long = env->GetMethodID(parcel, "writeLong", "(J)V");
  g.write_byte_array = env->GetMethodID(parcel, "writeByteArray", "([B)V");
  g.read_int = env->GetMethodID(parcel, "readInt", "()I");
  g.read_long = env->GetMethodID(parcel, "readLong", "()J");
  g.create_byte_array = env->GetMethodID(parcel, "createByteArray", "()[B");
  g.push_allow_fds = env->GetMethodID(parcel, "pushAllowFds", "(Z)Z");
  g.restore_allow_fds = env->GetMethodID(parcel, "restoreAllowFds", "(Z)V");
  g.write_fd = env->GetMethodID(parcel, "writeFileDescriptor", "(Ljava/io/FileDescriptor;)V");
  g.shared_memory = static_cast<jclass>(env->NewGlobalRef(sm));
  g.sm_get_fd = env->GetMethodID(sm, "getFileDescriptor", "()Ljava/io/FileDescriptor;");
  g.sm_close = env->GetMethodID(sm, "close", "()V");
  jfieldID creator_field = env->GetStaticFieldID(sm, "CREATOR", "Landroid/os/Parcelable$Creator;");
  g.creator_create = env->GetMethodID(creator, "createFromParcel", "(Landroid/os/Parcel;)Ljava/lang/Object;");
  if (Failed(env) || creator_field == nullptr) {
    env->ExceptionClear();
    return false;
  }
  jobject c = env->GetStaticObjectField(sm, creator_field);
  if (Failed(env) || c == nullptr) {
    env->ExceptionClear();
    return false;
  }
  g.sm_creator = env->NewGlobalRef(c);
  env->DeleteLocalRef(c);
  env->DeleteLocalRef(parcel);
  env->DeleteLocalRef(sm);
  env->DeleteLocalRef(creator);
  return true;
}

}  // namespace

// Called by register_gesher_surfaces (surfaces_jni.cc), after hwui's Bitmap table.
int register_gesher_bitmap_parcel(JNIEnv* env) {
  if (!Lookup(env)) {
    ALOGW("surfaces: android.os.Parcel or SharedMemory is missing; Bitmaps do not parcel");
    return 0;
  }
  return jniRegisterNativeMethods(env, "android/graphics/Bitmap", kMethods,
                                  sizeof(kMethods) / sizeof(kMethods[0]));
}
