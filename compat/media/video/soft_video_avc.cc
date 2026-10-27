// Gesher media: H.264 through AOSP's libavc decoder (video_decoder.h; media3,
// ADR 0059): the fallback where Media Foundation has no H.264 decoder MFT (an
// N edition without the Media Feature Pack, or GESHER_VIDEO_DECODERS=soft).
// Its own file: libavc's common/ivd.h and iv.h are also libhevc's names.
//
// As C2SoftAvcDec drives it: the header first (IVD_DECODE_HEADER) to learn the
// size, then frames; each decode call consumes what it can of an access unit;
// pictures come out in display order (the decoder reorders), each carrying
// the u4_ts marker of the access unit it came from. Drain: flush mode until no
// picture is left, then a reset.

#include <malloc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#include "ih264_typedefs.h"
#include "ih264d.h"
#include "iv.h"
#include "ivd.h"

#include "../audio/log.h"
#include "video_decoder.h"

namespace gesher::media {
namespace {

void* AvcAlloc(void*, WORD32 alignment, WORD32 size) {
  return _aligned_malloc(static_cast<size_t>(size), static_cast<size_t>(alignment));
}
void AvcFree(void*, void* p) { _aligned_free(p); }

constexpr int Align(int v, int a) { return (v + a - 1) / a * a; }

class AvcDecoder : public VideoDecoder {
 public:
  ~AvcDecoder() override {
    if (handle_ == nullptr) return;
    ih264d_delete_ip_t ip = {};
    ih264d_delete_op_t op = {};
    ip.s_ivd_delete_ip_t.u4_size = sizeof(ip);
    ip.s_ivd_delete_ip_t.e_cmd = IVD_CMD_DELETE;
    op.s_ivd_delete_op_t.u4_size = sizeof(op);
    ih264d_api_function(handle_, &ip, &op);
  }

  std::string Name() const override { return "soft avc (libavc)"; }

  bool Init(const Format& f, std::string* error) {
    int32_t w = 0, h = 0;
    f.GetInt32("width", &w);
    f.GetInt32("height", &h);
    width_ = w > 0 ? w : 320;
    height_ = h > 0 ? h : 240;
    stride_ = Align(width_, 128);
    prefix_ = AnnexBParameterSets(f, "video/avc");

    ih264d_create_ip_t ip = {};
    ih264d_create_op_t op = {};
    ip.s_ivd_create_ip_t.u4_size = sizeof(ip);
    ip.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
    ip.s_ivd_create_ip_t.u4_share_disp_buf = 0;
    ip.s_ivd_create_ip_t.e_output_format = IV_YUV_420P;
    ip.s_ivd_create_ip_t.pf_aligned_alloc = AvcAlloc;
    ip.s_ivd_create_ip_t.pf_aligned_free = AvcFree;
    ip.s_ivd_create_ip_t.pv_mem_ctxt = nullptr;
    ip.u4_enable_frame_info = 0;
    ip.u4_keep_threads_active = 0;
    op.s_ivd_create_op_t.u4_size = sizeof(op);
    if (ih264d_api_function(nullptr, &ip, &op) != IV_SUCCESS ||
        op.s_ivd_create_op_t.pv_handle == nullptr) {
      char b[16];
      snprintf(b, sizeof(b), "%x", op.s_ivd_create_op_t.u4_error_code);
      *error = Name() + ": create failed (0x" + b + ")";
      return false;
    }
    handle_ = static_cast<iv_obj_t*>(op.s_ivd_create_op_t.pv_handle);
    handle_->pv_fxns = reinterpret_cast<void*>(ih264d_api_function);
    handle_->u4_size = sizeof(iv_obj_t);
    Configure();
    return true;
  }

  bool Decode(const Sample& in, std::vector<VideoFrame>* out) override {
    // The parameter sets (csd-0/csd-1) first, on their own, as MediaCodec's
    // codec-config buffers reach C2SoftAvcDec.
    if (!prefix_sent_ && !prefix_.empty()) {
      prefix_sent_ = true;
      if (!DecodeBytes(prefix_, in.time_us, out)) return false;
    }
    prefix_sent_ = true;
    return DecodeBytes(in.data, in.time_us, out);
  }

  bool DecodeBytes(const std::vector<uint8_t>& bytes, int64_t time_us,
                   std::vector<VideoFrame>* out) {
    const std::vector<uint8_t>* data = &bytes;
    if (data->empty()) return true;
    const uint32_t marker = next_marker_++;
    times_[marker] = time_us;
    size_t pos = 0;
    for (int guard = 0; pos < data->size() && guard < 256; guard++) {
      if (!header_) SetParams(IVD_DECODE_HEADER);
      ih264d_video_decode_op_t op = {};
      Call(data->data() + pos, data->size() - pos, marker, &op);
      ivd_video_decode_op_t& o = op.s_ivd_video_decode_op_t;
      const uint32_t err = o.u4_error_code & IVD_ERROR_MASK;
      if (err == IVD_MEM_ALLOC_FAILED || err == IVD_STREAM_WIDTH_HEIGHT_NOT_SUPPORTED) {
        MEDIA_LOGE("%s: error 0x%x", Name().c_str(), o.u4_error_code);
        return false;
      }
      if (err == IVD_RES_CHANGED) {
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
          width_ = static_cast<int>(o.u4_pic_wd);
          height_ = static_cast<int>(o.u4_pic_ht);
          stride_ = Align(width_, 128);
          header_ = true;
          SetParams(IVD_DECODE_FRAME);
          // The header call consumed the parameter sets; the frames follow.
        }
        if (o.u4_num_bytes_consumed == 0) break;
        pos += o.u4_num_bytes_consumed;
        continue;
      }
      if (o.u4_output_present) Emit(o, out);
      if (o.u4_num_bytes_consumed == 0) break;
      pos += o.u4_num_bytes_consumed;
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
  void Control(void* ip, void* op) { ih264d_api_function(handle_, ip, op); }

  void Configure() {
    ih264d_ctl_set_num_cores_ip_t ip = {};
    ih264d_ctl_set_num_cores_op_t op = {};
    ip.u4_size = sizeof(ip);
    ip.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.e_sub_cmd = static_cast<IVD_CONTROL_API_COMMAND_TYPE_T>(IH264D_CMD_CTL_SET_NUM_CORES);
    ip.u4_num_cores = static_cast<UWORD32>(
        std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 4));
    op.u4_size = sizeof(op);
    Control(&ip, &op);
    SetParams(IVD_DECODE_FRAME);
  }

  void SetParams(IVD_VIDEO_DECODE_MODE_T mode) {
    ih264d_ctl_set_config_ip_t ip = {};
    ih264d_ctl_set_config_op_t op = {};
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

  void Call(const uint8_t* in, size_t size, uint32_t marker, ih264d_video_decode_op_t* op) {
    const int h = Align(height_, 16);
    const size_t luma = static_cast<size_t>(stride_) * h, chroma = luma / 4;
    if (buffer_.size() < luma + 2 * chroma) buffer_.assign(luma + 2 * chroma, 0);
    ih264d_video_decode_ip_t ip = {};
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
    ih264d_api_function(handle_, &ip, op);
  }

  void Emit(const ivd_video_decode_op_t& o, std::vector<VideoFrame>* out) {
    const iv_yuv_buf_t& b = o.s_disp_frm_buf;
    const int w = b.u4_y_wd > 0 ? static_cast<int>(b.u4_y_wd) : width_;
    const int h = b.u4_y_ht > 0 ? static_cast<int>(b.u4_y_ht) : height_;
    if (b.pv_y_buf == nullptr || b.pv_u_buf == nullptr || b.pv_v_buf == nullptr) return;
    const int ys = b.u4_y_strd > 0 ? static_cast<int>(b.u4_y_strd) : stride_;
    const int us = b.u4_u_strd > 0 ? static_cast<int>(b.u4_u_strd) : stride_ / 2;
    const int vs = b.u4_v_strd > 0 ? static_cast<int>(b.u4_v_strd) : stride_ / 2;
    VideoFrame f;
    f.Allocate(w, h);
    int64_t t = last_out_us_;
    auto it = times_.find(o.u4_ts);
    if (it != times_.end()) {
      t = it->second;
      times_.erase(it);
    }
    last_out_us_ = t;
    f.time_us = t;
    uint8_t* y = f.data.data();
    for (int r = 0; r < h; r++) {
      memcpy(y + static_cast<size_t>(r) * f.stride_y,
             static_cast<const uint8_t*>(b.pv_y_buf) + static_cast<size_t>(r) * ys,
             static_cast<size_t>(w));
    }
    uint8_t* u = y + static_cast<size_t>(f.stride_y) * f.height;
    uint8_t* v = u + static_cast<size_t>(f.stride_uv) * ((f.height + 1) / 2);
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int r = 0; r < ch; r++) {
      memcpy(u + static_cast<size_t>(r) * f.stride_uv,
             static_cast<const uint8_t*>(b.pv_u_buf) + static_cast<size_t>(r) * us,
             static_cast<size_t>(cw));
      memcpy(v + static_cast<size_t>(r) * f.stride_uv,
             static_cast<const uint8_t*>(b.pv_v_buf) + static_cast<size_t>(r) * vs,
             static_cast<size_t>(cw));
    }
    out->push_back(std::move(f));
  }

  void FlushInternal(std::vector<VideoFrame>* out) {
    ivd_ctl_flush_ip_t ip = {};
    ivd_ctl_flush_op_t op = {};
    ip.u4_size = sizeof(ip);
    ip.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.e_sub_cmd = IVD_CMD_CTL_FLUSH;
    op.u4_size = sizeof(op);
    Control(&ip, &op);
    for (int guard = 0; guard < 64; guard++) {
      ih264d_video_decode_op_t dop = {};
      Call(nullptr, 0, 0, &dop);
      if (!dop.s_ivd_video_decode_op_t.u4_output_present) break;
      Emit(dop.s_ivd_video_decode_op_t, out);
    }
  }

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

}  // namespace

std::unique_ptr<VideoDecoder> CreateSoftAvcDecoder(const Format& track, std::string* error) {
  auto d = std::make_unique<AvcDecoder>();
  if (!d->Init(track, error)) return nullptr;
  return d;
}

}  // namespace gesher::media
