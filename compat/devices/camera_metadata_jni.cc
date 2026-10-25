// Gesher (devices track): CameraMetadataNative's natives over system/media's
// libcamera_metadata, in place of frameworks/base's (which go through
// libcamera_client's CameraMetadata and cameraserver for vendor tags). The
// camera service is Gesher's own (com.gesher.server.CameraService, in the app
// process: docs/devices.md), so there are no vendor tags, and metadata
// crosses no process: a Parcel carries the vendor id and the packed buffer.
// Also CameraDeviceImpl's result-queue natives: results come in the
// callbacks' CameraMetadataInfo, never through a fast message queue.

#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>
#include <nativehelper/scoped_utf_chars.h>
#include <binder/Parcel.h>
#include <camera_metadata_hidden.h>
#include <system/camera_metadata.h>

#include <stdlib.h>
#include <string.h>

#include <string>

#include "android_os_Parcel.h"
#include "core_jni_helpers.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

struct Meta {
  camera_metadata_t* buffer = nullptr;
  int64_t vendorId = 0;
};

Meta* MetaOf(JNIEnv* env, jlong ptr) {
  if (ptr == 0) {
    jniThrowNullPointerException(env, "metadata is closed");
    return nullptr;
  }
  return reinterpret_cast<Meta*>(ptr);
}

camera_metadata_t* NewBuffer(size_t entries, size_t data) {
  return allocate_camera_metadata(entries, data);
}

// Makes room for `entries` more entries and `data` more bytes of data.
bool Reserve(Meta* m, size_t entries, size_t data) {
  size_t entryCount = get_camera_metadata_entry_count(m->buffer);
  size_t entryCap = get_camera_metadata_entry_capacity(m->buffer);
  size_t dataCount = get_camera_metadata_data_count(m->buffer);
  size_t dataCap = get_camera_metadata_data_capacity(m->buffer);
  if (entryCount + entries <= entryCap && dataCount + data <= dataCap) return true;
  size_t newEntries = (entryCount + entries) * 2 + 8;
  size_t newData = (dataCount + data) * 2 + 64;
  camera_metadata_t* grown = NewBuffer(newEntries, newData);
  if (grown == nullptr) return false;
  if (append_camera_metadata(grown, m->buffer) != 0) {
    free_camera_metadata(grown);
    return false;
  }
  set_camera_metadata_vendor_id(grown, get_camera_metadata_vendor_id(m->buffer));
  free_camera_metadata(m->buffer);
  m->buffer = grown;
  return true;
}

// Sets `tag` to `count` values of its type (replacing what was there).
bool Write(Meta* m, uint32_t tag, int type, const void* data, size_t count) {
  camera_metadata_entry_t entry;
  if (find_camera_metadata_entry(m->buffer, tag, &entry) == 0) {
    if (delete_camera_metadata_entry(m->buffer, entry.index) != 0) return false;
  }
  size_t bytes = calculate_camera_metadata_entry_data_size(type, count);
  if (!Reserve(m, 1, bytes)) return false;
  return add_camera_metadata_entry(m->buffer, tag, data, count) == 0;
}

int TypeOf(uint32_t tag) { return get_camera_metadata_tag_type(tag); }

jint TagFromKey(JNIEnv* env, jstring keyName) {
  ScopedUtfChars key(env, keyName);
  if (key.c_str() == nullptr) return 0;
  std::string name(key.c_str());
  size_t dot = name.rfind('.');
  if (dot != std::string::npos) {
    std::string section = name.substr(0, dot);
    std::string tagName = name.substr(dot + 1);
    for (uint32_t s = 0; s < ANDROID_SECTION_COUNT; ++s) {
      if (section != camera_metadata_section_names[s]) continue;
      for (uint32_t t = camera_metadata_section_bounds[s][0];
           t < camera_metadata_section_bounds[s][1]; ++t) {
        const char* n = get_camera_metadata_tag_name(t);
        if (n != nullptr && tagName == n) return static_cast<jint>(t);
      }
    }
  }
  jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException",
                       "Could not find tag for key '%s'", key.c_str());
  return 0;
}

jint TypeFromTag(JNIEnv* env, jint tag) {
  int type = TypeOf(static_cast<uint32_t>(tag));
  if (type == -1) {
    jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException",
                         "Tag (%d) did not have a type", tag);
    return -1;
  }
  return type;
}

void CameraMetadata_setVendorId(JNIEnv* env, jclass, jlong ptr, jlong vendorId) {
  Meta* m = MetaOf(env, ptr);
  if (m == nullptr) return;
  m->vendorId = vendorId;
  set_camera_metadata_vendor_id(m->buffer, static_cast<metadata_vendor_id_t>(vendorId));
}

jint CameraMetadata_getTagFromKey(JNIEnv* env, jclass, jstring keyName, jlong) {
  return TagFromKey(env, keyName);
}

jint CameraMetadata_getTypeFromTag(JNIEnv* env, jclass, jint tag, jlong) {
  return TypeFromTag(env, tag);
}

jint CameraMetadata_setupGlobalVendorTagDescriptor(JNIEnv*, jclass) {
  return 0;  // no vendor tags
}

jlong CameraMetadata_allocate(JNIEnv*, jclass) {
  Meta* m = new Meta();
  m->buffer = NewBuffer(8, 64);
  if (m->buffer == nullptr) {
    delete m;
    return 0;
  }
  return reinterpret_cast<jlong>(m);
}

jlong CameraMetadata_allocateCopy(JNIEnv* env, jclass, jlong ptr) {
  Meta* other = MetaOf(env, ptr);
  if (other == nullptr) return 0;
  Meta* m = new Meta();
  m->vendorId = other->vendorId;
  m->buffer = clone_camera_metadata(other->buffer);
  if (m->buffer == nullptr) {
    delete m;
    return 0;
  }
  return reinterpret_cast<jlong>(m);
}

void CameraMetadata_update(JNIEnv* env, jclass, jlong dst, jlong src) {
  Meta* d = MetaOf(env, dst);
  Meta* s = MetaOf(env, src);
  if (d == nullptr || s == nullptr) return;
  size_t count = get_camera_metadata_entry_count(s->buffer);
  for (size_t i = 0; i < count; ++i) {
    camera_metadata_ro_entry_t e;
    if (get_camera_metadata_ro_entry(s->buffer, i, &e) != 0) continue;
    if (!Write(d, e.tag, e.type, e.data.u8, e.count)) {
      jniThrowException(env, "java/lang/IllegalStateException", "metadata update failed");
      return;
    }
  }
}

jboolean CameraMetadata_isEmpty(JNIEnv* env, jclass, jlong ptr) {
  Meta* m = MetaOf(env, ptr);
  return m == nullptr || get_camera_metadata_entry_count(m->buffer) == 0;
}

jint CameraMetadata_getEntryCount(JNIEnv* env, jclass, jlong ptr) {
  Meta* m = MetaOf(env, ptr);
  return m == nullptr ? 0 : static_cast<jint>(get_camera_metadata_entry_count(m->buffer));
}

jlong CameraMetadata_getBufferSize(JNIEnv* env, jclass, jlong ptr) {
  Meta* m = MetaOf(env, ptr);
  return m == nullptr ? 0 : static_cast<jlong>(get_camera_metadata_size(m->buffer));
}

void CameraMetadata_close(JNIEnv*, jclass, jlong ptr) {
  Meta* m = reinterpret_cast<Meta*>(ptr);
  if (m == nullptr) return;
  free_camera_metadata(m->buffer);
  delete m;
}

void CameraMetadata_swap(JNIEnv* env, jclass, jlong ptr, jlong other) {
  Meta* a = MetaOf(env, ptr);
  Meta* b = MetaOf(env, other);
  if (a == nullptr || b == nullptr) return;
  std::swap(a->buffer, b->buffer);
  std::swap(a->vendorId, b->vendorId);
}

jint CameraMetadata_getTagFromKeyLocal(JNIEnv* env, jclass, jlong, jstring keyName) {
  return TagFromKey(env, keyName);
}

jint CameraMetadata_getTypeFromTagLocal(JNIEnv* env, jclass, jlong, jint tag) {
  return TypeFromTag(env, tag);
}

jbyteArray CameraMetadata_readValues(JNIEnv* env, jclass, jint tag, jlong ptr) {
  Meta* m = MetaOf(env, ptr);
  if (m == nullptr) return nullptr;
  int type = TypeFromTag(env, tag);
  if (type == -1) return nullptr;
  camera_metadata_ro_entry_t e;
  if (find_camera_metadata_ro_entry(m->buffer, static_cast<uint32_t>(tag), &e) != 0) {
    return nullptr;
  }
  jsize bytes = static_cast<jsize>(e.count * camera_metadata_type_size[type]);
  jbyteArray array = env->NewByteArray(bytes);
  if (array == nullptr) return nullptr;
  env->SetByteArrayRegion(array, 0, bytes, reinterpret_cast<const jbyte*>(e.data.u8));
  return array;
}

void CameraMetadata_writeValues(JNIEnv* env, jclass, jint tag, jbyteArray src, jlong ptr) {
  Meta* m = MetaOf(env, ptr);
  if (m == nullptr) return;
  int type = TypeFromTag(env, tag);
  if (type == -1) return;
  if (src == nullptr) {
    camera_metadata_entry_t e;
    if (find_camera_metadata_entry(m->buffer, static_cast<uint32_t>(tag), &e) == 0) {
      delete_camera_metadata_entry(m->buffer, e.index);
    }
    return;
  }
  jsize bytes = env->GetArrayLength(src);
  size_t size = camera_metadata_type_size[type];
  if (bytes % size != 0) {
    jniThrowException(env, "java/lang/IllegalArgumentException",
                      "Src byte array was poorly formed");
    return;
  }
  jbyte* data = env->GetByteArrayElements(src, nullptr);
  bool ok = Write(m, static_cast<uint32_t>(tag), type, data, bytes / size);
  env->ReleaseByteArrayElements(src, data, JNI_ABORT);
  if (!ok) {
    jniThrowException(env, "java/lang/IllegalStateException",
                      "Internal error while trying to update metadata");
  }
}

void CameraMetadata_dump(JNIEnv*, jclass, jlong) {}

jobject CameraMetadata_getAllVendorKeys(JNIEnv* env, jclass, jlong, jclass) {
  jclass list = env->FindClass("java/util/ArrayList");
  jmethodID init = env->GetMethodID(list, "<init>", "()V");
  jobject result = env->NewObject(list, init);
  env->DeleteLocalRef(list);
  return result;  // no vendor tags
}

void CameraMetadata_readFromParcel(JNIEnv* env, jclass, jobject parcel, jlong ptr) {
  Meta* m = MetaOf(env, ptr);
  Parcel* p = parcelForJavaObject(env, parcel);
  if (m == nullptr || p == nullptr) return;
  int64_t vendorId = p->readInt64();
  int32_t size = p->readInt32();
  if (size <= 0) {
    camera_metadata_t* empty = NewBuffer(8, 64);
    free_camera_metadata(m->buffer);
    m->buffer = empty;
    m->vendorId = vendorId;
    return;
  }
  void* data = malloc(size);
  if (data == nullptr || p->read(data, size) != OK) {
    free(data);
    jniThrowException(env, "android/os/BadParcelableException", "short camera metadata");
    return;
  }
  camera_metadata_t* copy = allocate_copy_camera_metadata_checked(
      static_cast<const camera_metadata_t*>(data), size);
  free(data);
  if (copy == nullptr) {
    jniThrowException(env, "android/os/BadParcelableException", "bad camera metadata");
    return;
  }
  free_camera_metadata(m->buffer);
  m->buffer = copy;
  m->vendorId = vendorId;
}

void CameraMetadata_writeToParcel(JNIEnv* env, jclass, jobject parcel, jlong ptr) {
  Meta* m = MetaOf(env, ptr);
  Parcel* p = parcelForJavaObject(env, parcel);
  if (m == nullptr || p == nullptr) return;
  size_t size = get_camera_metadata_size(m->buffer);
  p->writeInt64(m->vendorId);
  p->writeInt32(static_cast<int32_t>(size));
  p->write(m->buffer, size);
}

#define STR "Ljava/lang/String;"

const JNINativeMethod gCameraMetadataMethods[] = {
    {"nativeSetVendorId", "(JJ)V", GESHER_JNI(CameraMetadata_setVendorId)},
    {"nativeGetTagFromKey", "(" STR "J)I", GESHER_JNI(CameraMetadata_getTagFromKey)},
    {"nativeGetTypeFromTag", "(IJ)I", GESHER_JNI(CameraMetadata_getTypeFromTag)},
    {"nativeSetupGlobalVendorTagDescriptor", "()I",
     GESHER_JNI(CameraMetadata_setupGlobalVendorTagDescriptor)},
    {"nativeAllocate", "()J", GESHER_JNI(CameraMetadata_allocate)},
    {"nativeAllocateCopy", "(J)J", GESHER_JNI(CameraMetadata_allocateCopy)},
    {"nativeUpdate", "(JJ)V", GESHER_JNI(CameraMetadata_update)},
    {"nativeIsEmpty", "(J)Z", GESHER_JNI(CameraMetadata_isEmpty)},
    {"nativeGetEntryCount", "(J)I", GESHER_JNI(CameraMetadata_getEntryCount)},
    {"nativeGetBufferSize", "(J)J", GESHER_JNI(CameraMetadata_getBufferSize)},
    {"nativeClose", "(J)V", GESHER_JNI(CameraMetadata_close)},
    {"nativeSwap", "(JJ)V", GESHER_JNI(CameraMetadata_swap)},
    {"nativeGetTagFromKeyLocal", "(J" STR ")I", GESHER_JNI(CameraMetadata_getTagFromKeyLocal)},
    {"nativeGetTypeFromTagLocal", "(JI)I", GESHER_JNI(CameraMetadata_getTypeFromTagLocal)},
    {"nativeReadValues", "(IJ)[B", GESHER_JNI(CameraMetadata_readValues)},
    {"nativeWriteValues", "(I[BJ)V", GESHER_JNI(CameraMetadata_writeValues)},
    {"nativeDump", "(J)V", GESHER_JNI(CameraMetadata_dump)},
    {"nativeGetAllVendorKeys", "(JLjava/lang/Class;)Ljava/util/ArrayList;",
     GESHER_JNI(CameraMetadata_getAllVendorKeys)},
    {"nativeReadFromParcel", "(Landroid/os/Parcel;J)V", GESHER_JNI(CameraMetadata_readFromParcel)},
    {"nativeWriteToParcel", "(Landroid/os/Parcel;J)V", GESHER_JNI(CameraMetadata_writeToParcel)},
};

// CameraDeviceImpl: results arrive in CameraMetadataInfo.metadata, never as an FMQ size.
jlong CameraDevice_createFMQReader(JNIEnv*, jclass, jobject) { return 0; }

jlong CameraDevice_readResultMetadata(JNIEnv* env, jclass, jlong, jlong) {
  jniThrowException(env, "java/lang/IllegalStateException",
                    "no result metadata queue on Gesher");
  return 0;
}

void CameraDevice_close(JNIEnv*, jclass, jlong) {}

const JNINativeMethod gCameraDeviceImplMethods[] = {
    {"nativeCreateFMQReader", "(Landroid/os/Parcel;)J", GESHER_JNI(CameraDevice_createFMQReader)},
    {"nativeReadResultMetadata", "(JJ)J", GESHER_JNI(CameraDevice_readResultMetadata)},
    {"nativeClose", "(J)V", GESHER_JNI(CameraDevice_close)},
};

}  // namespace

int register_gesher_camera_metadata(JNIEnv* env) {
  RegisterGesherNatives(env, "android/hardware/camera2/impl/CameraMetadataNative",
                        gCameraMetadataMethods, NELEM(gCameraMetadataMethods));
  RegisterGesherNatives(env, "android/hardware/camera2/impl/CameraDeviceImpl",
                        gCameraDeviceImplMethods, NELEM(gCameraDeviceImplMethods));
  return 0;
}

}  // namespace android
