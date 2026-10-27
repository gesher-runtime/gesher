// Gesher media: AOSP's software video decoders (video_decoder.h, ADR 0051):
// the codecs Media Foundation lacks on a machine (cmake/aosp/media_video_codecs.cmake).
//
//  - VP8 and VP9: libvpx (external/libvpx, its generic C configuration);
//  - AV1: libdav1d (external/libdav1d, C only, 8 and 10 bits);
//  - H.265: libhevc's decoder (external/libhevc, with its SSSE3/SSE4.2 code);
//  - H.264: libavc's decoder (soft_video_avc.cc), MPEG-4 part 2 and H.263:
//    the m4v_h263 decoder (soft_video_m4v.cc), where Media Foundation has none
//    (media3, ADR 0059).
//
// Each gives its pictures as 8-bit I420 frames in presentation order; deeper
// pictures are shifted down to 8 bits, 4:2:2 and 4:4:4 chroma subsampled.

#include <errno.h>
#include <malloc.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#include <dav1d/dav1d.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "ihevc_typedefs.h"
#include "ihevcd_cxa.h"

#include "../audio/log.h"
#include "video_decoder.h"

namespace gesher::media {
namespace {

int Threads(int most) {
  int n = static_cast<int>(std::thread::hardware_concurrency());
  return std::clamp(n, 1, most);
}

// --- Picture copies -----------------------------------------------------------------------

struct Plane {
  const uint8_t* data = nullptr;
  ptrdiff_t stride = 0;
};

inline uint8_t Sample8(const Plane& p, int x, int y, bool high, int shift) {
  const uint8_t* row = p.data + static_cast<ptrdiff_t>(y) * p.stride;
  if (!high) return row[x];
  uint16_t v;
  memcpy(&v, row + 2 * static_cast<ptrdiff_t>(x), sizeof(v));
  int s = v >> shift;
  return static_cast<uint8_t>(s > 255 ? 255 : s);
}

// A w x h picture (planes Y, U, V; chroma subsampled by ss_x/ss_y, none
// without chroma; samples of 16 bits when `high`, `depth` bits deep) as an
// I420 frame at `time_us`.
VideoFrame CopyPicture(int w, int h, const Plane (&p)[3], bool high, int depth, int ss_x, int ss_y,
                       bool has_chroma, int64_t time_us) {
  VideoFrame f;
  f.Allocate(w, h);
  f.time_us = time_us;
  const int shift = high ? std::max(depth - 8, 0) : 0;
  uint8_t* y = f.data.data();
  for (int r = 0; r < h; r++) {
    uint8_t* d = y + static_cast<size_t>(r) * f.stride_y;
    if (!high) {
      memcpy(d, p[0].data + static_cast<ptrdiff_t>(r) * p[0].stride, static_cast<size_t>(w));
    } else {
      for (int x = 0; x < w; x++) d[x] = Sample8(p[0], x, r, true, shift);
    }
  }
  uint8_t* u = y + static_cast<size_t>(f.stride_y) * f.height;
  uint8_t* v = u + static_cast<size_t>(f.stride_uv) * ((f.height + 1) / 2);
  const int cw = (w + 1) / 2, ch = (h + 1) / 2;
  if (!has_chroma) {
    memset(u, 128, static_cast<size_t>(f.stride_uv) * ch);
    memset(v, 128, static_cast<size_t>(f.stride_uv) * ch);
    return f;
  }
  // The source's chroma size, for clamping.
  const int sw = ss_x ? (w + 1) / 2 : w, sh = ss_y ? (h + 1) / 2 : h;
  const bool direct = !high && ss_x && ss_y;
  for (int r = 0; r < ch; r++) {
    uint8_t* du = u + static_cast<size_t>(r) * f.stride_uv;
    uint8_t* dv = v + static_cast<size_t>(r) * f.stride_uv;
    const int sy = std::min(ss_y ? r : 2 * r, sh - 1);
    if (direct) {
      memcpy(du, p[1].data + static_cast<ptrdiff_t>(sy) * p[1].stride, static_cast<size_t>(cw));
      memcpy(dv, p[2].data + static_cast<ptrdiff_t>(sy) * p[2].stride, static_cast<size_t>(cw));
      continue;
    }
    for (int x = 0; x < cw; x++) {
      const int sx = std::min(ss_x ? x : 2 * x, sw - 1);
      du[x] = Sample8(p[1], sx, sy, high, shift);
      dv[x] = Sample8(p[2], sx, sy, high, shift);
    }
  }
  return f;
}

// --- VP8 and VP9: libvpx ------------------------------------------------------------------

class VpxDecoder : public VideoDecoder {
 public:
  ~VpxDecoder() override {
    if (open_) vpx_codec_destroy(&ctx_);
  }

  std::string Name() const override { return std::string("soft ") + (vp9_ ? "vp9" : "vp8") + " (libvpx)"; }

  bool Init(bool vp9, std::string* error) {
    vp9_ = vp9;
    vpx_codec_dec_cfg_t cfg = {};
    cfg.threads = static_cast<unsigned>(Threads(4));
    vpx_codec_err_t err =
        vpx_codec_dec_init(&ctx_, vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(), &cfg, 0);
    if (err != VPX_CODEC_OK) {
      *error = Name() + ": " + vpx_codec_err_to_string(err);
      return false;
    }
    open_ = true;
    return true;
  }

  bool Decode(const Sample& in, std::vector<VideoFrame>* out) override {
    if (in.data.empty()) return true;
    last_time_us_ = in.time_us;
    vpx_codec_err_t err = vpx_codec_decode(&ctx_, in.data.data(), static_cast<unsigned>(in.data.size()),
                                           nullptr, 0);
    if (err != VPX_CODEC_OK) {
      const char* detail = vpx_codec_error_detail(&ctx_);
      MEDIA_LOGE("%s: %s%s%s", Name().c_str(), vpx_codec_err_to_string(err), detail ? ": " : "",
                 detail ? detail : "");
      return false;
    }
    Pull(out, in.time_us);
    return true;
  }

  void Flush() override {
    std::vector<VideoFrame> dropped;
    Drain(&dropped);
  }

  void Drain(std::vector<VideoFrame>* out) override {
    vpx_codec_decode(&ctx_, nullptr, 0, nullptr, 0);
    Pull(out, last_time_us_);
  }

 private:
  void Pull(std::vector<VideoFrame>* out, int64_t time_us) {
    vpx_codec_iter_t iter = nullptr;
    while (vpx_image_t* img = vpx_codec_get_frame(&ctx_, &iter)) {
      const bool high = (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0;
      Plane p[3];
      for (int i = 0; i < 3; i++) {
        p[i].data = img->planes[i];
        p[i].stride = img->stride[i];
      }
      out->push_back(CopyPicture(static_cast<int>(img->d_w), static_cast<int>(img->d_h), p, high,
                                 static_cast<int>(img->bit_depth),
                                 static_cast<int>(img->x_chroma_shift),
                                 static_cast<int>(img->y_chroma_shift), true, time_us));
    }
  }

  vpx_codec_ctx_t ctx_ = {};
  bool open_ = false;
  bool vp9_ = false;
  int64_t last_time_us_ = 0;
};

// --- AV1: libdav1d ------------------------------------------------------------------------

void Dav1dLog(void*, const char* format, va_list ap) {
  char b[512];
  vsnprintf(b, sizeof(b), format, ap);
  size_t n = strlen(b);
  if (n > 0 && b[n - 1] == '\n') b[n - 1] = '\0';
  MEDIA_LOGI("dav1d: %s", b);
}

class Dav1dDecoder : public VideoDecoder {
 public:
  ~Dav1dDecoder() override {
    if (c_ != nullptr) dav1d_close(&c_);
  }

  std::string Name() const override { return "soft av1 (dav1d)"; }

  bool Init(std::string* error) {
    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.n_threads = Threads(4);
    // One picture in flight: each temporal unit's picture comes out with it.
    s.max_frame_delay = 1;
    s.logger.cookie = nullptr;
    s.logger.callback = Dav1dLog;
    int r = dav1d_open(&c_, &s);
    if (r < 0) {
      *error = Name() + ": dav1d_open " + std::to_string(r);
      c_ = nullptr;
      return false;
    }
    return true;
  }

  bool Decode(const Sample& in, std::vector<VideoFrame>* out) override {
    if (in.data.empty()) return true;
    Dav1dData data = {};
    uint8_t* p = dav1d_data_create(&data, in.data.size());
    if (p == nullptr) return false;
    memcpy(p, in.data.data(), in.data.size());
    data.m.timestamp = in.time_us;
    for (int guard = 0; data.sz > 0; guard++) {
      int r = dav1d_send_data(c_, &data);
      if ((r < 0 && r != DAV1D_ERR(EAGAIN)) || guard > 1000) {
        MEDIA_LOGE("%s: dav1d_send_data %d", Name().c_str(), r);
        dav1d_data_unref(&data);
        return false;
      }
      if (!Pull(out)) {
        dav1d_data_unref(&data);
        return false;
      }
    }
    return true;
  }

  void Flush() override { dav1d_flush(c_); }

  void Drain(std::vector<VideoFrame>* out) override { Pull(out); }

 private:
  bool Pull(std::vector<VideoFrame>* out) {
    for (;;) {
      Dav1dPicture pic = {};
      int r = dav1d_get_picture(c_, &pic);
      if (r == DAV1D_ERR(EAGAIN)) return true;
      if (r < 0) {
        MEDIA_LOGE("%s: dav1d_get_picture %d", Name().c_str(), r);
        return false;
      }
      Plane p[3];
      p[0].data = static_cast<const uint8_t*>(pic.data[0]);
      p[0].stride = pic.stride[0];
      for (int i = 1; i < 3; i++) {
        p[i].data = static_cast<const uint8_t*>(pic.data[i]);
        p[i].stride = pic.stride[1];
      }
      const Dav1dPixelLayout layout = pic.p.layout;
      out->push_back(CopyPicture(pic.p.w, pic.p.h, p, pic.p.bpc > 8, pic.p.bpc,
                                 layout == DAV1D_PIXEL_LAYOUT_I444 ? 0 : 1,
                                 layout == DAV1D_PIXEL_LAYOUT_I420 ? 1 : 0,
                                 layout != DAV1D_PIXEL_LAYOUT_I400, pic.m.timestamp));
      dav1d_picture_unref(&pic);
    }
  }

  Dav1dContext* c_ = nullptr;
};

// --- H.265: libhevc -----------------------------------------------------------------------

void* HevcAlloc(void*, WORD32 alignment, WORD32 size) {
  return _aligned_malloc(static_cast<size_t>(size), static_cast<size_t>(alignment));
}
void HevcFree(void*, void* p) { _aligned_free(p); }

constexpr int Align(int v, int a) { return (v + a - 1) / a * a; }

class HevcDecoder : public VideoDecoder {
 public:
  ~HevcDecoder() override {
    if (handle_ == nullptr) return;
    ihevcd_cxa_delete_ip_t ip = {};
    ihevcd_cxa_delete_op_t op = {};
    ip.s_ivd_delete_ip_t.u4_size = sizeof(ip);
    ip.s_ivd_delete_ip_t.e_cmd = IVD_CMD_DELETE;
    op.s_ivd_delete_op_t.u4_size = sizeof(op);
    ihevcd_cxa_api_function(handle_, &ip, &op);
  }

  std::string Name() const override { return "soft hevc (libhevc)"; }

  bool Init(const Format& f, std::string* error) {
    int32_t w = 0, h = 0;
    f.GetInt32("width", &w);
    f.GetInt32("height", &h);
    width_ = w > 0 ? w : 320;
    height_ = h > 0 ? h : 240;
    stride_ = Align(width_, 128);
    prefix_ = AnnexBParameterSets(f, "video/hevc");

    ihevcd_cxa_create_ip_t ip = {};
    ihevcd_cxa_create_op_t op = {};
    ip.s_ivd_create_ip_t.u4_size = sizeof(ip);
    ip.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
    ip.s_ivd_create_ip_t.u4_share_disp_buf = 0;
    ip.s_ivd_create_ip_t.e_output_format = IV_YUV_420P;
    ip.s_ivd_create_ip_t.pf_aligned_alloc = HevcAlloc;
    ip.s_ivd_create_ip_t.pf_aligned_free = HevcFree;
    ip.s_ivd_create_ip_t.pv_mem_ctxt = nullptr;
    ip.u4_enable_frame_info = 0;
    ip.u4_keep_threads_active = 0;
    op.s_ivd_create_op_t.u4_size = sizeof(op);
    if (ihevcd_cxa_api_function(nullptr, &ip, &op) != IV_SUCCESS ||
        op.s_ivd_create_op_t.pv_handle == nullptr) {
      *error = Name() + ": create failed (0x" + Hex(op.s_ivd_create_op_t.u4_error_code) + ")";
      return false;
    }
    handle_ = static_cast<iv_obj_t*>(op.s_ivd_create_op_t.pv_handle);
    handle_->pv_fxns = reinterpret_cast<void*>(ihevcd_cxa_api_function);
    handle_->u4_size = sizeof(iv_obj_t);
    Configure();
    return true;
  }

  bool Decode(const Sample& in, std::vector<VideoFrame>* out) override {
    std::vector<uint8_t> bytes;
    const std::vector<uint8_t>* data = &in.data;
    if (!prefix_sent_ && !prefix_.empty()) {
      bytes = prefix_;
      bytes.insert(bytes.end(), in.data.begin(), in.data.end());
      data = &bytes;
    }
    prefix_sent_ = true;
    if (data->empty()) return true;
    const uint32_t marker = next_marker_++;
    times_[marker] = in.time_us;
    size_t pos = 0;
    for (int guard = 0; pos < data->size() && guard < 256; guard++) {
      if (!header_) SetParams(IVD_DECODE_HEADER);
      ihevcd_cxa_video_decode_op_t op = {};
      Call(data->data() + pos, data->size() - pos, marker, &op);
      ivd_video_decode_op_t& o = op.s_ivd_video_decode_op_t;
      const uint32_t err = o.u4_error_code & IVD_ERROR_MASK;
      if (err == IVD_MEM_ALLOC_FAILED || err == IVD_STREAM_WIDTH_HEIGHT_NOT_SUPPORTED) {
        MEDIA_LOGE("%s: error 0x%x", Name().c_str(), o.u4_error_code);
        return false;
      }
      if (err == IVD_RES_CHANGED) {
        // A new size: what is inside comes out, then the decoder starts over.
        FlushInternal(out);
        Reset();
        continue;
      }
      if (IS_IVD_FATAL_ERROR(o.u4_error_code)) {
        MEDIA_LOGE("%s: fatal error 0x%x", Name().c_str(), o.u4_error_code);
        return false;
      }
      if (!header_) {
        if (o.u4_pic_wd > 0 && o.u4_pic_ht > 0) {
          // The size is known: the same bytes again, now decoding pictures.
          width_ = static_cast<int>(o.u4_pic_wd);
          height_ = static_cast<int>(o.u4_pic_ht);
          stride_ = Align(width_, 128);
          header_ = true;
          SetParams(IVD_DECODE_FRAME);
          continue;
        }
        if (o.u4_num_bytes_consumed == 0) break;
        pos += o.u4_num_bytes_consumed;
        continue;
      }
      if (o.u4_output_present) Emit(o, out);
      if (o.u4_num_bytes_consumed == 0) break;
      pos += o.u4_num_bytes_consumed;
      // One picture an access unit (what follows it is ignored, as C2SoftHevcDec does).
      if (o.u4_frame_decoded_flag) break;
    }
    return true;
  }

  void Flush() override {
    std::vector<VideoFrame> dropped;
    FlushInternal(&dropped);
    Reset();
  }

  void Drain(std::vector<VideoFrame>* out) override {
    FlushInternal(out);
    Reset();
  }

 private:
  static std::string Hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "%x", v);
    return b;
  }

  void Control(void* ip, void* op) { ihevcd_cxa_api_function(handle_, ip, op); }

  void Configure() {
    ihevcd_cxa_ctl_set_num_cores_ip_t ip = {};
    ihevcd_cxa_ctl_set_num_cores_op_t op = {};
    ip.u4_size = sizeof(ip);
    ip.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.e_sub_cmd = static_cast<IVD_CONTROL_API_COMMAND_TYPE_T>(IHEVCD_CXA_CMD_CTL_SET_NUM_CORES);
    ip.u4_num_cores = static_cast<UWORD32>(Threads(4));
    op.u4_size = sizeof(op);
    Control(&ip, &op);
    SetParams(IVD_DECODE_FRAME);
  }

  void SetParams(IVD_VIDEO_DECODE_MODE_T mode) {
    ihevcd_cxa_ctl_set_config_ip_t ip = {};
    ihevcd_cxa_ctl_set_config_op_t op = {};
    ivd_ctl_set_config_ip_t& i = ip.s_ivd_ctl_set_config_ip_t;
    i.u4_size = sizeof(ip);
    i.e_cmd = IVD_CMD_VIDEO_CTL;
    i.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
    i.u4_disp_wd = static_cast<UWORD32>(stride_);
    i.e_frm_skip_mode = IVD_SKIP_NONE;
    i.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
    i.e_vid_dec_mode = mode;
    op.s_ivd_ctl_set_config_op_t.u4_size = sizeof(op);
    Control(&ip, &op);
  }

  // One decode call, into the output buffer; null input in flush mode.
  void Call(const uint8_t* in, size_t size, uint32_t marker, ihevcd_cxa_video_decode_op_t* op) {
    const int h = Align(height_, 16);
    const size_t luma = static_cast<size_t>(stride_) * h, chroma = luma / 4;
    if (buffer_.size() < luma + 2 * chroma) buffer_.assign(luma + 2 * chroma, 0);
    ihevcd_cxa_video_decode_ip_t ip = {};
    ivd_video_decode_ip_t& i = ip.s_ivd_video_decode_ip_t;
    i.u4_size = sizeof(ip);
    i.e_cmd = IVD_CMD_VIDEO_DECODE;
    i.u4_ts = marker;
    i.pv_stream_buffer = const_cast<uint8_t*>(in);
    i.u4_num_Bytes = static_cast<UWORD32>(size);
    i.s_out_buffer.u4_num_bufs = 3;
    i.s_out_buffer.pu1_bufs[0] = buffer_.data();
    i.s_out_buffer.pu1_bufs[1] = buffer_.data() + luma;
    i.s_out_buffer.pu1_bufs[2] = buffer_.data() + luma + chroma;
    i.s_out_buffer.u4_min_out_buf_size[0] = static_cast<UWORD32>(luma);
    i.s_out_buffer.u4_min_out_buf_size[1] = static_cast<UWORD32>(chroma);
    i.s_out_buffer.u4_min_out_buf_size[2] = static_cast<UWORD32>(chroma);
    op->s_ivd_video_decode_op_t.u4_size = sizeof(*op);
    op->s_ivd_video_decode_op_t.u4_output_present = 0;
    ihevcd_cxa_api_function(handle_, &ip, op);
  }

  void Emit(const ivd_video_decode_op_t& o, std::vector<VideoFrame>* out) {
    const iv_yuv_buf_t& b = o.s_disp_frm_buf;
    int w = b.u4_y_wd > 0 ? static_cast<int>(b.u4_y_wd) : width_;
    int h = b.u4_y_ht > 0 ? static_cast<int>(b.u4_y_ht) : height_;
    if (b.pv_y_buf == nullptr || b.pv_u_buf == nullptr || b.pv_v_buf == nullptr) return;
    Plane p[3];
    p[0].data = static_cast<const uint8_t*>(b.pv_y_buf);
    p[0].stride = b.u4_y_strd > 0 ? static_cast<int>(b.u4_y_strd) : stride_;
    p[1].data = static_cast<const uint8_t*>(b.pv_u_buf);
    p[1].stride = b.u4_u_strd > 0 ? static_cast<int>(b.u4_u_strd) : stride_ / 2;
    p[2].data = static_cast<const uint8_t*>(b.pv_v_buf);
    p[2].stride = b.u4_v_strd > 0 ? static_cast<int>(b.u4_v_strd) : stride_ / 2;
    int64_t t = last_out_us_;
    auto it = times_.find(o.u4_ts);
    if (it != times_.end()) {
      t = it->second;
      times_.erase(it);
    }
    last_out_us_ = t;
    out->push_back(CopyPicture(w, h, p, false, 8, 1, 1, true, t));
  }

  // Flush mode: the pictures still held for display.
  void FlushInternal(std::vector<VideoFrame>* out) {
    ivd_ctl_flush_ip_t ip = {};
    ivd_ctl_flush_op_t op = {};
    ip.u4_size = sizeof(ip);
    ip.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.e_sub_cmd = IVD_CMD_CTL_FLUSH;
    op.u4_size = sizeof(op);
    Control(&ip, &op);
    for (int guard = 0; guard < 64; guard++) {
      ihevcd_cxa_video_decode_op_t dop = {};
      Call(nullptr, 0, 0, &dop);
      if (!dop.s_ivd_video_decode_op_t.u4_output_present) break;
      Emit(dop.s_ivd_video_decode_op_t, out);
    }
  }

  // Back to the start of a stream: headers again, the parameter sets in front
  // of the next access unit.
  void Reset() {
    ivd_ctl_reset_ip_t ip = {};
    ivd_ctl_reset_op_t op = {};
    ip.u4_size = sizeof(ip);
    ip.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.e_sub_cmd = IVD_CMD_CTL_RESET;
    op.u4_size = sizeof(op);
    Control(&ip, &op);
    Configure();
    header_ = false;
    prefix_sent_ = false;
    times_.clear();
  }

  iv_obj_t* handle_ = nullptr;
  std::vector<uint8_t> prefix_;
  bool prefix_sent_ = false;
  bool header_ = false;
  int width_ = 0, height_ = 0, stride_ = 0;
  std::vector<uint8_t> buffer_;
  uint32_t next_marker_ = 0;
  std::map<uint32_t, int64_t> times_;
  int64_t last_out_us_ = 0;
};

const char* const kMimes[] = {"video/x-vnd.on2.vp8", "video/x-vnd.on2.vp9", "video/av01",
                              "video/hevc", "video/avc", "video/mp4v-es", "video/3gpp"};

}  // namespace

std::vector<VideoCodecInfo> SoftVideoCodecs() {
  std::vector<VideoCodecInfo> out;
  for (const char* mime : kMimes) {
    VideoCodecInfo info;
    info.mime = mime;
    info.backend = "soft";
    info.hardware = false;
    info.max_width = 4096;
    info.max_height = 2304;
    out.push_back(info);
  }
  return out;
}

std::unique_ptr<VideoDecoder> CreateSoftVideoDecoder(const Format& track, std::string* error) {
  const std::string mime = track.Mime();
  if (mime == "video/x-vnd.on2.vp8" || mime == "video/x-vnd.on2.vp9") {
    auto d = std::make_unique<VpxDecoder>();
    if (!d->Init(mime == "video/x-vnd.on2.vp9", error)) return nullptr;
    return d;
  }
  if (mime == "video/av01") {
    auto d = std::make_unique<Dav1dDecoder>();
    if (!d->Init(error)) return nullptr;
    return d;
  }
  if (mime == "video/hevc") {
    auto d = std::make_unique<HevcDecoder>();
    if (!d->Init(track, error)) return nullptr;
    return d;
  }
  if (mime == "video/avc") return CreateSoftAvcDecoder(track, error);
  if (mime == "video/mp4v-es") return CreateSoftM4vDecoder(track, true, error);
  if (mime == "video/3gpp") return CreateSoftM4vDecoder(track, false, error);
  *error = "no software decoder for " + mime;
  return nullptr;
}

}  // namespace gesher::media
