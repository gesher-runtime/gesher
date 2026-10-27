// Gesher media: H.264 through AOSP's libavc encoder (encoder.h; ADR 0059): the
// fallback where Media Foundation has no H.264 encoder MFT (and what
// GESHER_VIDEO_ENCODERS=soft selects). As C2SoftAvcEnc drives it: memory records
// sized by the encoder, its init, the controls (dimensions, frame rate, bitrate,
// QP bounds, motion estimation, GOP, baseline profile), the header mode's first
// call for the parameter sets, then a picture per call (I420 in; the input
// buffers are the encoder's until it hands them back).
// Its own file: libavc's encoder headers (iv2.h) name iv_obj_t as the decoder's do.

#include <malloc.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <thread>

#include "ih264_typedefs.h"
#include "ih264e.h"
#include "ih264e_error.h"
#include "iv2.h"
#include "ive2.h"

#include "../audio/log.h"
#include "encoder.h"

namespace gesher::media {
namespace {

template <typename Ip>
void Head(Ip* ip, IVE_CONTROL_API_COMMAND_TYPE_T sub) {
  ip->u4_size = sizeof(*ip);
  ip->e_cmd = IVE_CMD_VIDEO_CTL;
  ip->e_sub_cmd = sub;
  ip->u4_timestamp_high = static_cast<UWORD32>(-1);
  ip->u4_timestamp_low = static_cast<UWORD32>(-1);
}

class SoftAvcEncoder : public VideoEncoder {
 public:
  ~SoftAvcEncoder() override {
    if (ctx_ != nullptr) {
      iv_retrieve_mem_rec_ip_t ip = {};
      iv_retrieve_mem_rec_op_t op = {};
      ip.u4_size = sizeof(ip);
      ip.e_cmd = IV_CMD_RETRIEVE_MEMREC;
      ip.ps_mem_rec = mem_.data();
      op.u4_size = sizeof(op);
      ih264e_api_function(ctx_, &ip, &op);
    }
    for (iv_mem_rec_t& r : mem_) {
      if (r.pv_base != nullptr) _aligned_free(r.pv_base);
    }
  }

  std::string Name() const override { return "soft avc encoder (libavc)"; }

  bool Init(const VideoEncoderParams& p, std::string* error) {
    p_ = p;
    const UWORD32 w = static_cast<UWORD32>(p.width), h = static_cast<UWORD32>(p.height);
    iv_num_mem_rec_ip_t nip = {};
    iv_num_mem_rec_op_t nop = {};
    nip.u4_size = sizeof(nip);
    nip.e_cmd = IV_CMD_GET_NUM_MEM_REC;
    nop.u4_size = sizeof(nop);
    if (ih264e_api_function(nullptr, &nip, &nop) != IV_SUCCESS) {
      *error = "libavc: no memory records";
      return false;
    }
    mem_.assign(nop.u4_num_mem_rec, iv_mem_rec_t{});
    for (iv_mem_rec_t& r : mem_) {
      r.u4_size = sizeof(iv_mem_rec_t);
      r.e_mem_type = IV_NA_MEM_TYPE;
    }
    ih264e_fill_mem_rec_ip_t fip = {};
    ih264e_fill_mem_rec_op_t fop = {};
    iv_fill_mem_rec_ip_t& f = fip.s_ive_ip;
    f.u4_size = sizeof(fip);
    f.e_cmd = IV_CMD_FILL_NUM_MEM_REC;
    f.ps_mem_rec = mem_.data();
    f.u4_num_mem_rec = static_cast<UWORD32>(mem_.size());
    f.u4_max_wd = w;
    f.u4_max_ht = h;
    f.u4_max_level = 41;
    f.e_color_format = IV_YUV_420P;
    f.u4_max_ref_cnt = 2;
    f.u4_max_reorder_cnt = 0;
    f.u4_max_srch_rng_x = 256;
    f.u4_max_srch_rng_y = 256;
    f.u4_keep_threads_active = 0;
    fop.s_ive_op.u4_size = sizeof(fop);
    if (ih264e_api_function(nullptr, &fip, &fop) != IV_SUCCESS) {
      *error = "libavc: fill memory records failed";
      return false;
    }
    for (iv_mem_rec_t& r : mem_) {
      r.pv_base = _aligned_malloc(std::max<UWORD32>(r.u4_mem_size, 1), std::max<UWORD32>(r.u4_mem_alignment, 16));
      if (r.pv_base == nullptr) {
        *error = "libavc: out of memory";
        return false;
      }
    }
    ctx_ = static_cast<iv_obj_t*>(mem_[0].pv_base);
    ctx_->u4_size = sizeof(iv_obj_t);
    ctx_->pv_fxns = reinterpret_cast<void*>(ih264e_api_function);
    ih264e_init_ip_t iip = {};
    ih264e_init_op_t iop = {};
    ive_init_ip_t& i = iip.s_ive_ip;
    i.u4_size = sizeof(iip);
    i.e_cmd = IV_CMD_INIT;
    i.u4_num_mem_rec = static_cast<UWORD32>(mem_.size());
    i.ps_mem_rec = mem_.data();
    i.u4_max_wd = w;
    i.u4_max_ht = h;
    i.u4_max_ref_cnt = 2;
    i.u4_max_reorder_cnt = 0;
    i.u4_max_level = 41;
    i.e_inp_color_fmt = IV_YUV_420P;
    i.u4_enable_recon = 0;
    i.e_recon_color_fmt = IV_YUV_420P;
    i.e_rc_mode = IVE_RC_STORAGE;
    i.u4_max_framerate = 120000;
    i.u4_max_bitrate = 240000000;
    i.u4_num_bframes = 0;
    i.e_content_type = IV_PROGRESSIVE;
    i.u4_max_srch_rng_x = 256;
    i.u4_max_srch_rng_y = 256;
    i.e_slice_mode = IVE_SLICE_MODE_NONE;
    i.u4_slice_param = 256;
    i.e_arch = ARCH_X86_SSE42;
    i.e_soc = SOC_GENERIC;
    i.u4_keep_threads_active = 0;
    iop.s_ive_op.u4_size = sizeof(iop);
    if (ih264e_api_function(ctx_, &iip, &iop) != IV_SUCCESS) {
      *error = "libavc: init failed";
      ctx_ = nullptr;
      return false;
    }
    Controls();
    return true;
  }

  bool Encode(const VideoFrame& frame, bool key, std::vector<EncodedPacket>* out) override {
    if (!header_ && !Header(out)) return false;
    if (key) SetFrameType(IV_IDR_FRAME);
    const int w = p_.width, h = p_.height, cw = w / 2, ch = h / 2;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 3 / 2);
    for (int y = 0; y < h; y++) {
      memcpy(buf.data() + static_cast<size_t>(y) * w,
             frame.y() + static_cast<size_t>(frame.crop_top + y) * frame.stride_y + frame.crop_left,
             static_cast<size_t>(w));
    }
    uint8_t* u = buf.data() + static_cast<size_t>(w) * h;
    uint8_t* v = u + static_cast<size_t>(cw) * ch;
    for (int y = 0; y < ch; y++) {
      const size_t row = static_cast<size_t>(frame.crop_top / 2 + y) * frame.stride_uv + frame.crop_left / 2;
      memcpy(u + static_cast<size_t>(y) * cw, frame.u() + row, static_cast<size_t>(cw));
      memcpy(v + static_cast<size_t>(y) * cw, frame.v() + row, static_cast<size_t>(cw));
    }
    const uint32_t index = next_index_++;
    times_[index] = frame.time_us;
    void* key_ptr = buf.data();
    inputs_[key_ptr] = std::move(buf);
    return Call(static_cast<uint8_t*>(key_ptr), index, false, out);
  }

  bool Drain(std::vector<EncodedPacket>* out) override {
    for (int guard = 0; guard < 16; guard++) {
      bool last = false;
      if (!Call(nullptr, 0, true, out, &last)) return false;
      if (last) break;
    }
    return true;
  }

 private:
  template <typename Ip, typename Op>
  void Ctl(Ip* ip, Op* op) {
    op->u4_size = sizeof(*op);
    ih264e_api_function(ctx_, ip, op);
  }

  void Controls() {
    {
      ive_ctl_set_num_cores_ip_t ip = {};
      ive_ctl_set_num_cores_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_NUM_CORES);
      ip.u4_num_cores = static_cast<UWORD32>(std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 4));
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_dimensions_ip_t ip = {};
      ive_ctl_set_dimensions_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_DIMENSIONS);
      ip.u4_wd = static_cast<UWORD32>(p_.width);
      ip.u4_ht = static_cast<UWORD32>(p_.height);
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_frame_rate_ip_t ip = {};
      ive_ctl_set_frame_rate_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_FRAMERATE);
      ip.u4_src_frame_rate = static_cast<UWORD32>(std::max(p_.frame_rate, 1));
      ip.u4_tgt_frame_rate = ip.u4_src_frame_rate;
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_ipe_params_ip_t ip = {};
      ive_ctl_set_ipe_params_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_IPE_PARAMS);
      ip.u4_enable_intra_4x4 = 1;
      ip.u4_enc_speed_preset = IVE_NORMAL;
      ip.u4_constrained_intra_pred = 0;
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_bitrate_ip_t ip = {};
      ive_ctl_set_bitrate_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_BITRATE);
      ip.u4_target_bitrate = static_cast<UWORD32>(std::max(p_.bitrate, 64000));
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_qp_ip_t ip = {};
      ive_ctl_set_qp_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_QP);
      ip.u4_i_qp_min = ip.u4_p_qp_min = ip.u4_b_qp_min = 10;
      ip.u4_i_qp_max = ip.u4_p_qp_max = ip.u4_b_qp_max = 40;
      ip.u4_i_qp = 22;
      ip.u4_p_qp = 28;
      ip.u4_b_qp = 22;
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_vbv_params_ip_t ip = {};
      ive_ctl_set_vbv_params_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_VBV_PARAMS);
      ip.u4_vbv_buf_size = 0;
      ip.u4_vbv_buffer_delay = 1000;
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_me_params_ip_t ip = {};
      ive_ctl_set_me_params_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_ME_PARAMS);
      ip.u4_enable_fast_sad = 0;
      ip.u4_enable_alt_ref = 0;
      ip.u4_enable_hpel = 1;
      ip.u4_enable_qpel = 1;
      ip.u4_me_speed_preset = 100;
      ip.u4_srch_rng_x = 64;
      ip.u4_srch_rng_y = 48;
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_gop_params_ip_t ip = {};
      ive_ctl_set_gop_params_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_GOP_PARAMS);
      const UWORD32 gop = p_.i_frame_interval_s > 0
                              ? static_cast<UWORD32>(std::max(1.0f, p_.i_frame_interval_s * p_.frame_rate))
                              : 1;
      ip.u4_i_frm_interval = gop;
      ip.u4_idr_frm_interval = gop;
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_deblock_params_ip_t ip = {};
      ive_ctl_set_deblock_params_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_DEBLOCK_PARAMS);
      ip.u4_disable_deblock_level = 0;
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_profile_params_ip_t ip = {};
      ive_ctl_set_profile_params_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_PROFILE_PARAMS);
      ip.e_profile = IV_PROFILE_BASE;
      ip.u4_entropy_coding_mode = 0;
      Ctl(&ip, &op);
    }
    {
      ive_ctl_set_enc_mode_ip_t ip = {};
      ive_ctl_set_enc_mode_op_t op = {};
      Head(&ip, IVE_CMD_CTL_SET_ENC_MODE);
      ip.e_enc_mode = IVE_ENC_MODE_HEADER;
      Ctl(&ip, &op);
    }
  }

  void SetFrameType(IV_PICTURE_CODING_TYPE_T type) {
    ive_ctl_set_frame_type_ip_t ip = {};
    ive_ctl_set_frame_type_op_t op = {};
    Head(&ip, IVE_CMD_CTL_SET_FRAMETYPE);
    ip.e_frame_type = type;
    Ctl(&ip, &op);
  }

  // The header mode's call: the parameter sets, as the codec-config packet.
  bool Header(std::vector<EncodedPacket>* out) {
    std::vector<uint8_t> buf(4096);
    ih264e_video_encode_ip_t eip = {};
    ih264e_video_encode_op_t eop = {};
    ive_video_encode_ip_t& e = eip.s_ive_ip;
    e.u4_size = sizeof(eip);
    e.e_cmd = IVE_CMD_VIDEO_ENCODE;
    e.s_inp_buf.u4_size = sizeof(iv_raw_buf_t);
    e.s_inp_buf.e_color_fmt = IV_YUV_420P;
    e.s_out_buf.pv_buf = buf.data();
    e.s_out_buf.u4_bufsize = static_cast<UWORD32>(buf.size());
    eop.s_ive_op.u4_size = sizeof(eop);
    if (ih264e_api_function(ctx_, &eip, &eop) != IV_SUCCESS) {
      MEDIA_LOGE("%s: the header did not encode (0x%x)", Name().c_str(), eop.s_ive_op.u4_error_code);
      return false;
    }
    EncodedPacket c;
    c.data.assign(buf.begin(), buf.begin() + eop.s_ive_op.s_out_buf.u4_bytes);
    c.config = true;
    out->push_back(std::move(c));
    header_ = true;
    return true;
  }

  bool Call(uint8_t* in, uint32_t index, bool last, std::vector<EncodedPacket>* out, bool* is_last = nullptr) {
    const int w = p_.width, h = p_.height;
    for (size_t size = std::max<size_t>(static_cast<size_t>(w) * h * 3 / 2, 1 << 16);; size *= 2) {
      std::vector<uint8_t> buf(size);
      ih264e_video_encode_ip_t eip = {};
      ih264e_video_encode_op_t eop = {};
      ive_video_encode_ip_t& e = eip.s_ive_ip;
      e.u4_size = sizeof(eip);
      e.e_cmd = IVE_CMD_VIDEO_ENCODE;
      e.u4_timestamp_low = index;
      e.u4_is_last = last ? 1 : 0;
      e.s_out_buf.pv_buf = buf.data();
      e.s_out_buf.u4_bufsize = static_cast<UWORD32>(buf.size());
      iv_raw_buf_t& r = e.s_inp_buf;
      r.u4_size = sizeof(iv_raw_buf_t);
      r.e_color_fmt = IV_YUV_420P;
      if (in != nullptr) {
        r.apv_bufs[0] = in;
        r.apv_bufs[1] = in + static_cast<size_t>(w) * h;
        r.apv_bufs[2] = in + static_cast<size_t>(w) * h * 5 / 4;
        r.au4_wd[0] = static_cast<UWORD32>(w);
        r.au4_wd[1] = r.au4_wd[2] = static_cast<UWORD32>(w / 2);
        r.au4_ht[0] = static_cast<UWORD32>(h);
        r.au4_ht[1] = r.au4_ht[2] = static_cast<UWORD32>(h / 2);
        r.au4_strd[0] = static_cast<UWORD32>(w);
        r.au4_strd[1] = r.au4_strd[2] = static_cast<UWORD32>(w / 2);
      }
      eop.s_ive_op.u4_size = sizeof(eop);
      IV_STATUS_T status = ih264e_api_function(ctx_, &eip, &eop);
      ive_video_encode_op_t& o = eop.s_ive_op;
      if (status != IV_SUCCESS) {
        if ((o.u4_error_code & 0xFF) == IH264E_BITSTREAM_BUFFER_OVERFLOW && size < (64u << 20)) continue;
        MEDIA_LOGE("%s: encode failed (0x%x)", Name().c_str(), o.u4_error_code);
        return false;
      }
      // The input the encoder is done with.
      if (o.s_inp_buf.apv_bufs[0] != nullptr) inputs_.erase(o.s_inp_buf.apv_bufs[0]);
      if (o.output_present && o.s_out_buf.u4_bytes > 0) {
        EncodedPacket pkt;
        pkt.data.assign(buf.begin(), buf.begin() + o.s_out_buf.u4_bytes);
        auto it = times_.find(o.u4_timestamp_low);
        if (it != times_.end()) {
          pkt.time_us = it->second;
          times_.erase(it);
        }
        pkt.key = o.u4_encoded_frame_type == IV_IDR_FRAME || o.u4_encoded_frame_type == IV_I_FRAME;
        out->push_back(std::move(pkt));
      }
      if (is_last != nullptr) *is_last = o.u4_is_last != 0 || !o.output_present;
      return true;
    }
  }

  VideoEncoderParams p_;
  iv_obj_t* ctx_ = nullptr;
  std::vector<iv_mem_rec_t> mem_;
  bool header_ = false;
  uint32_t next_index_ = 0;
  std::map<uint32_t, int64_t> times_;
  std::map<void*, std::vector<uint8_t>> inputs_;
};

}  // namespace

std::unique_ptr<VideoEncoder> CreateSoftAvcEncoder(const VideoEncoderParams& p, std::string* error) {
  auto e = std::make_unique<SoftAvcEncoder>();
  if (!e->Init(p, error)) return nullptr;
  return e;
}

}  // namespace gesher::media
