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
// through SharedMemory. The reader copies them into a heap Bitmap either way
// (mutable as written). Natives are JNICALL (System V), as ART calls them.

#include <jni.h>
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

namespace {

using android::Bitmap;

constexpr int32_t kBlobInPlace = 0;
constexpr int32_t kBlobSharedMemory = 1;
constexpr size_t kInPlaceLimit = 12 * 1024;  // hwui's BLOB_INPLACE_LIMIT

struct {
  jclass parcel;
  jmethodID write_int, write_long, write_byte_array, read_int, read_long, create_byte_array;
  jmethodID push_allow_fds, restore_allow_fds;
  jclass shared_memory;
  jmethodID sm_create, sm_map_read_write, sm_map_read_only, sm_unmap, sm_write_to_parcel, sm_close;
  jobject sm_creator;  // SharedMemory.CREATOR
  jmethodID creator_create;
} g;

bool Failed(JNIEnv* env) {
  return env->ExceptionCheck() == JNI_TRUE;
}

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

void WriteBytes(JNIEnv* env, jobject parcel, const void* data, size_t size) {
  jbyteArray a = env->NewByteArray(static_cast<jsize>(size));
  if (a == nullptr) return;
  env->SetByteArrayRegion(a, 0, static_cast<jsize>(size), static_cast<const jbyte*>(data));
  env->CallVoidMethod(parcel, g.write_byte_array, a);
  env->DeleteLocalRef(a);
}

uint64_t NextParcelId() {
  static std::atomic<uint32_t> counter{0};
  return (static_cast<uint64_t>(getpid()) << 32) | counter.fetch_add(1);
}

// The pixels into a new SharedMemory written to the parcel; false (and an
// exception) if that fails.
bool WriteSharedMemory(JNIEnv* env, jobject parcel, const void* pixels, size_t size) {
  jstring name = env->NewStringUTF("bitmap");
  jobject sm = env->CallStaticObjectMethod(g.shared_memory, g.sm_create, name,
                                           static_cast<jint>(size));
  env->DeleteLocalRef(name);
  if (Failed(env) || sm == nullptr) return false;
  jobject buffer = env->CallObjectMethod(sm, g.sm_map_read_write);
  if (Failed(env) || buffer == nullptr) return false;
  void* address = env->GetDirectBufferAddress(buffer);
  if (address != nullptr) memcpy(address, pixels, size);
  env->CallStaticVoidMethod(g.shared_memory, g.sm_unmap, buffer);
  env->DeleteLocalRef(buffer);
  if (address == nullptr) {
    jniThrowRuntimeException(env, "Could not map the bitmap's shared memory");
    return false;
  }
  env->CallVoidMethod(parcel, g.write_int, kBlobSharedMemory);
  env->CallVoidMethod(parcel, g.write_int, static_cast<jint>(size));
  env->CallVoidMethod(sm, g.sm_write_to_parcel, parcel, 0);
  // The parcel holds its own duplicate of the descriptor.
  env->CallVoidMethod(sm, g.sm_close);
  env->DeleteLocalRef(sm);
  return !Failed(env);
}

jboolean JNICALL Bitmap_writeToParcel(JNIEnv* env, jobject, jlong bitmap_handle, jint density,
                                      jobject parcel) {
  if (parcel == nullptr) return JNI_FALSE;
  Bitmap& native = android::bitmap::toBitmap(bitmap_handle);
  SkBitmap bitmap;
  native.getSkBitmap(&bitmap);
  const size_t size = bitmap.computeByteSize();
  const bool fds = size > kInPlaceLimit && AllowsFds(env, parcel);
  // As hwui: a copy into shared memory is written as immutable.
  const bool as_mutable = !bitmap.isImmutable() && !fds;

  env->CallVoidMethod(parcel, g.write_int, as_mutable ? 1 : 0);
  env->CallVoidMethod(parcel, g.write_int, static_cast<jint>(bitmap.colorType()));
  env->CallVoidMethod(parcel, g.write_int, static_cast<jint>(bitmap.alphaType()));
  if (SkColorSpace* cs = bitmap.colorSpace()) {
    sk_sp<SkData> data = cs->serialize();
    env->CallVoidMethod(parcel, g.write_int, 1);
    WriteBytes(env, parcel, data->data(), data->size());
  } else {
    env->CallVoidMethod(parcel, g.write_int, 0);
  }
  env->CallVoidMethod(parcel, g.write_int, bitmap.width());
  env->CallVoidMethod(parcel, g.write_int, bitmap.height());
  env->CallVoidMethod(parcel, g.write_int, static_cast<jint>(bitmap.rowBytes()));
  env->CallVoidMethod(parcel, g.write_int, density);
  env->CallVoidMethod(parcel, g.write_long, static_cast<jlong>(native.getId()));
  env->CallVoidMethod(parcel, g.write_long, static_cast<jlong>(NextParcelId()));
  if (Failed(env)) return JNI_FALSE;

  if (fds) return WriteSharedMemory(env, parcel, bitmap.getPixels(), size) ? JNI_TRUE : JNI_FALSE;
  env->CallVoidMethod(parcel, g.write_int, kBlobInPlace);
  WriteBytes(env, parcel, bitmap.getPixels(), size);
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
  auto read_int = [&] { return env->CallIntMethod(parcel, g.read_int); };
  const bool is_mutable = read_int() != 0;
  const auto color_type = static_cast<SkColorType>(read_int());
  const auto alpha_type = static_cast<SkAlphaType>(read_int());
  sk_sp<SkColorSpace> color_space;
  if (read_int() != 0) {
    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(parcel, g.create_byte_array));
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
  const int64_t source_id = env->CallLongMethod(parcel, g.read_long);
  env->CallLongMethod(parcel, g.read_long);  // the parcel id (tracing)
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
  if (width <= 0 || height <= 0 || !SkBitmap().setInfo(info, row_bytes) ||
      !Bitmap::computeAllocationSize(row_bytes, height, &allocation)) {
    ThrowBadParcelable(env, "Received bad bitmap size");
    return nullptr;
  }
  sk_sp<Bitmap> native = Bitmap::allocateHeapBitmap(allocation, info, row_bytes);
  if (native == nullptr) {
    jniThrowRuntimeException(env, "Could not allocate bitmap data.");
    return nullptr;
  }

  if (blob == kBlobInPlace) {
    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(parcel, g.create_byte_array));
    if (Failed(env)) return nullptr;
    if (bytes == nullptr || static_cast<size_t>(env->GetArrayLength(bytes)) < allocation) {
      ThrowBadParcelable(env, "Bitmap_createFromParcel: the pixels are missing or short");
      return nullptr;
    }
    env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(allocation),
                            static_cast<jbyte*>(native->pixels()));
    env->DeleteLocalRef(bytes);
  } else if (blob == kBlobSharedMemory) {
    const int32_t size = read_int();
    jobject sm = env->CallObjectMethod(g.sm_creator, g.creator_create, parcel);
    if (Failed(env)) return nullptr;
    if (sm == nullptr || size < 0 || static_cast<size_t>(size) < allocation) {
      ThrowBadParcelable(env, "Bitmap_createFromParcel: bad shared memory");
      return nullptr;
    }
    jobject buffer = env->CallObjectMethod(sm, g.sm_map_read_only);
    if (Failed(env) || buffer == nullptr) return nullptr;
    const void* address = env->GetDirectBufferAddress(buffer);
    if (address != nullptr) memcpy(native->pixels(), address, allocation);
    env->CallStaticVoidMethod(g.shared_memory, g.sm_unmap, buffer);
    env->CallVoidMethod(sm, g.sm_close);
    env->DeleteLocalRef(buffer);
    env->DeleteLocalRef(sm);
    if (address == nullptr) {
      jniThrowRuntimeException(env, "Could not map the bitmap's shared memory");
      return nullptr;
    }
  } else {
    ThrowBadParcelable(env, "Bitmap_createFromParcel: unknown blob type");
    return nullptr;
  }
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
  g.shared_memory = static_cast<jclass>(env->NewGlobalRef(sm));
  g.sm_create = env->GetStaticMethodID(sm, "create", "(Ljava/lang/String;I)Landroid/os/SharedMemory;");
  g.sm_map_read_write = env->GetMethodID(sm, "mapReadWrite", "()Ljava/nio/ByteBuffer;");
  g.sm_map_read_only = env->GetMethodID(sm, "mapReadOnly", "()Ljava/nio/ByteBuffer;");
  g.sm_unmap = env->GetStaticMethodID(sm, "unmap", "(Ljava/nio/ByteBuffer;)V");
  g.sm_write_to_parcel = env->GetMethodID(sm, "writeToParcel", "(Landroid/os/Parcel;I)V");
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
