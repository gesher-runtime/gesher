// Gesher (surfaces track, ADR 0056): android.graphics.YuvImage's natives.
//
// hwui's jni/YuvToJpegEncoder.cpp is left out of the build (its JPEG/R half
// needs libultrahdr), so YuvImage.compressToJpeg had no native. This is that
// file's plain JPEG half over the same libjpeg-turbo Skia's codecs use: NV21
// (ImageFormat.NV21, HAL YCrCb_420_SP) and YUY2 (ImageFormat.YUY2, HAL
// YCbCr_422_I), written as raw YCbCr data (4:2:0 or 4:2:2 sampling) to the
// Java OutputStream through hwui's stream adaptor. compressToJpegR (Ultra HDR)
// answers false: no gain-map encoder.

#include <jni.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>  // before jpeglib.h, which uses FILE
#include <string.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <SkStream.h>

#include <CreateJavaOutputStreamAdaptor.h>  // libs/hwui/jni
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

extern "C" {
#include "jpeglib.h"
#include "jerror.h"
}

namespace {

constexpr int kNv21 = 0x11;  // HAL_PIXEL_FORMAT_YCrCb_420_SP, ImageFormat.NV21
constexpr int kYuy2 = 0x14;  // HAL_PIXEL_FORMAT_YCbCr_422_I, ImageFormat.YUY2

struct ErrorMgr {
  jpeg_error_mgr pub;
  jmp_buf jmp;
};

void ErrorExit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<ErrorMgr*>(cinfo->err);
  (*cinfo->err->output_message)(cinfo);
  longjmp(err->jmp, 1);
}

constexpr size_t kBufferSize = 1024;

struct StreamDestination : jpeg_destination_mgr {
  SkWStream* stream;
  uint8_t buffer[kBufferSize];
};

void InitDestination(j_compress_ptr cinfo) {
  auto* d = static_cast<StreamDestination*>(cinfo->dest);
  d->next_output_byte = d->buffer;
  d->free_in_buffer = kBufferSize;
}

boolean EmptyOutputBuffer(j_compress_ptr cinfo) {
  auto* d = static_cast<StreamDestination*>(cinfo->dest);
  if (!d->stream->write(d->buffer, kBufferSize)) ERREXIT(cinfo, JERR_FILE_WRITE);
  d->next_output_byte = d->buffer;
  d->free_in_buffer = kBufferSize;
  return TRUE;
}

void TermDestination(j_compress_ptr cinfo) {
  auto* d = static_cast<StreamDestination*>(cinfo->dest);
  size_t size = kBufferSize - d->free_in_buffer;
  if (size > 0 && !d->stream->write(d->buffer, size)) ERREXIT(cinfo, JERR_FILE_WRITE);
  d->stream->flush();
}

// NV21 rows [row, row + 16) into raw planes (16 Y rows, 8 Cb and 8 Cr rows).
void Nv21Rows(const uint8_t* y_plane, const uint8_t* vu_plane, const int* strides, int row,
              int width, int height, std::vector<uint8_t>* y_rows, std::vector<uint8_t>* u_rows,
              std::vector<uint8_t>* v_rows, JSAMPROW* y, JSAMPROW* cb, JSAMPROW* cr) {
  const int half = (width + 1) / 2;
  for (int i = 0; i < 16; i++) {
    int r = row + i < height ? row + i : height - 1;
    uint8_t* dst = y_rows->data() + static_cast<size_t>(i) * width;
    memcpy(dst, y_plane + static_cast<size_t>(r) * strides[0], static_cast<size_t>(width));
    y[i] = dst;
  }
  for (int i = 0; i < 8; i++) {
    int r = row / 2 + i;
    int last = (height + 1) / 2 - 1;
    if (r > last) r = last;
    const uint8_t* vu = vu_plane + static_cast<size_t>(r) * strides[1];
    uint8_t* u = u_rows->data() + static_cast<size_t>(i) * half;
    uint8_t* v = v_rows->data() + static_cast<size_t>(i) * half;
    for (int x = 0; x < half; x++) {
      v[x] = vu[2 * x];
      u[x] = vu[2 * x + 1];
    }
    cb[i] = u;
    cr[i] = v;
  }
}

// YUY2 rows [row, row + 16) into raw planes (16 Y, Cb and Cr rows; 4:2:2).
void Yuy2Rows(const uint8_t* yuv, const int* strides, int row, int width, int height,
              std::vector<uint8_t>* y_rows, std::vector<uint8_t>* u_rows,
              std::vector<uint8_t>* v_rows, JSAMPROW* y, JSAMPROW* cb, JSAMPROW* cr) {
  const int half = (width + 1) / 2;
  for (int i = 0; i < 16; i++) {
    int r = row + i < height ? row + i : height - 1;
    const uint8_t* s = yuv + static_cast<size_t>(r) * strides[0];
    uint8_t* yd = y_rows->data() + static_cast<size_t>(i) * width;
    uint8_t* u = u_rows->data() + static_cast<size_t>(i) * half;
    uint8_t* v = v_rows->data() + static_cast<size_t>(i) * half;
    for (int x = 0; x < half; x++) {
      yd[2 * x] = s[4 * x];
      if (2 * x + 1 < width) yd[2 * x + 1] = s[4 * x + 2];
      u[x] = s[4 * x + 1];
      v[x] = s[4 * x + 3];
    }
    y[i] = yd;
    cb[i] = u;
    cr[i] = v;
  }
}

bool Encode(SkWStream* stream, const uint8_t* yuv, int format, int width, int height,
            const int* offsets, const int* strides, int quality) {
  jpeg_compress_struct cinfo;
  ErrorMgr err;
  StreamDestination dest;
  dest.stream = stream;
  dest.init_destination = InitDestination;
  dest.empty_output_buffer = EmptyOutputBuffer;
  dest.term_destination = TermDestination;
  cinfo.err = jpeg_std_error(&err.pub);
  err.pub.error_exit = ErrorExit;
  // Allocated before setjmp, freed after (the vectors outlive a longjmp).
  const int half = (width + 1) / 2;
  std::vector<uint8_t> y_rows(static_cast<size_t>(16) * width);
  std::vector<uint8_t> u_rows(static_cast<size_t>(16) * half);
  std::vector<uint8_t> v_rows(static_cast<size_t>(16) * half);
  if (setjmp(err.jmp)) {
    jpeg_destroy_compress(&cinfo);
    return false;
  }
  jpeg_create_compress(&cinfo);
  cinfo.dest = &dest;
  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_YCbCr;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_set_colorspace(&cinfo, JCS_YCbCr);
  cinfo.raw_data_in = TRUE;
  cinfo.dct_method = JDCT_IFAST;
  const bool nv21 = format == kNv21;
  cinfo.comp_info[0].h_samp_factor = 2;
  cinfo.comp_info[0].v_samp_factor = 2;
  cinfo.comp_info[1].h_samp_factor = 1;
  cinfo.comp_info[1].v_samp_factor = nv21 ? 1 : 2;
  cinfo.comp_info[2].h_samp_factor = 1;
  cinfo.comp_info[2].v_samp_factor = nv21 ? 1 : 2;
  jpeg_start_compress(&cinfo, TRUE);
  JSAMPROW y[16], cb[16], cr[16];
  JSAMPARRAY planes[3] = {y, cb, cr};
  while (cinfo.next_scanline < cinfo.image_height) {
    int row = static_cast<int>(cinfo.next_scanline);
    if (nv21) {
      Nv21Rows(yuv + offsets[0], yuv + offsets[1], strides, row, width, height, &y_rows, &u_rows,
               &v_rows, y, cb, cr);
    } else {
      Yuy2Rows(yuv + offsets[0], strides, row, width, height, &y_rows, &u_rows, &v_rows, y, cb,
               cr);
    }
    jpeg_write_raw_data(&cinfo, planes, 16);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  return true;
}

jboolean JNICALL YuvImage_compressToJpeg(JNIEnv* env, jclass, jbyteArray in_yuv, jint format,
                                         jint width, jint height, jintArray offsets,
                                         jintArray strides, jint quality, jobject jstream,
                                         jbyteArray jstorage) {
  if (format != kNv21 && format != kYuy2) return JNI_FALSE;
  if (width <= 0 || height <= 0 || offsets == nullptr || strides == nullptr) return JNI_FALSE;
  if (env->GetArrayLength(offsets) < (format == kNv21 ? 2 : 1) ||
      env->GetArrayLength(strides) < (format == kNv21 ? 2 : 1)) {
    return JNI_FALSE;
  }
  int off[2] = {0, 0}, str[2] = {0, 0};
  env->GetIntArrayRegion(offsets, 0, format == kNv21 ? 2 : 1, off);
  env->GetIntArrayRegion(strides, 0, format == kNv21 ? 2 : 1, str);
  // The data must cover the rows the strides and offsets name.
  const jsize length = env->GetArrayLength(in_yuv);
  int64_t need = format == kNv21
                     ? std::max<int64_t>(off[0] + int64_t{str[0]} * height,
                                         off[1] + int64_t{str[1]} * ((height + 1) / 2))
                     : off[0] + int64_t{str[0]} * height;
  if (need > length) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "YUV data too short");
    return JNI_FALSE;
  }
  std::unique_ptr<SkWStream> stream(CreateJavaOutputStreamAdaptor(env, jstream, jstorage));
  if (stream == nullptr) return JNI_FALSE;
  jbyte* yuv = env->GetByteArrayElements(in_yuv, nullptr);
  bool ok = Encode(stream.get(), reinterpret_cast<const uint8_t*>(yuv), format, width, height,
                   off, str, quality);
  env->ReleaseByteArrayElements(in_yuv, yuv, JNI_ABORT);
  return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean JNICALL YuvImage_compressToJpegR(JNIEnv*, jclass, jbyteArray, jint, jbyteArray, jint,
                                          jint, jint, jint, jobject, jbyteArray, jbyteArray,
                                          jintArray, jintArray) {
  ALOGW("YuvImage.compressToJpegR: no Ultra HDR (JPEG/R) encoder in Gesher");
  return JNI_FALSE;
}

const JNINativeMethod kMethods[] = {
    {"nativeCompressToJpeg", "([BIII[I[IILjava/io/OutputStream;[B)Z",
     reinterpret_cast<void*>(&YuvImage_compressToJpeg)},
    {"nativeCompressToJpegR", "([BI[BIIIILjava/io/OutputStream;[B[B[I[I)Z",
     reinterpret_cast<void*>(&YuvImage_compressToJpegR)},
};

}  // namespace

int register_gesher_yuv_image(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "android/graphics/YuvImage", kMethods,
                                  sizeof(kMethods) / sizeof(kMethods[0]));
}
