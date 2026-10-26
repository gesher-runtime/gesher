# Media (docs/media.md "Video", ADR 0051): AOSP's software video decoders, for
# the codecs Media Foundation lacks on a machine (compat/media/video/soft_video.cc).
# Included from media_codecs.cmake.
#
#  - libvpx (external/libvpx): VP8 and VP9 decoding, its generic (C only)
#    configuration as Android.bp's libvpx_generic_c_srcs, the encoders left out;
#  - libdav1d (external/libdav1d): AV1, C only (HAVE_ASM 0, as Android builds it
#    for x86-64), the bit-depth templates twice (8 and 16 bits), Win32 threads;
#  - libhevcdec (external/libhevc): H.265, with its SSSE3/SSE4.2 intrinsics as
#    Android.bp's x86_64 variant.
#
# Third-party code: warnings stay warnings; Gesher's own code keeps -Werror.

set(_vpx "${AOSP_DIR}/external/libvpx")
set(_dav1d "${AOSP_DIR}/external/libdav1d")
set(_hevc "${AOSP_DIR}/external/libhevc")
set(_video_third_party_flags -w)

# --- libvpx -------------------------------------------------------------------------------
set(_vpx_srcs
  config/generic/vpx_config.c
  vp8/common/alloccommon.c vp8/common/blockd.c vp8/common/dequantize.c vp8/common/entropy.c
  vp8/common/entropymode.c vp8/common/entropymv.c vp8/common/extend.c vp8/common/filter.c
  vp8/common/findnearmv.c vp8/common/generic/systemdependent.c vp8/common/idct_blk.c
  vp8/common/idctllm.c vp8/common/loopfilter_filters.c vp8/common/mbpitch.c
  vp8/common/modecont.c vp8/common/quant_common.c vp8/common/reconinter.c
  vp8/common/reconintra.c vp8/common/reconintra4x4.c vp8/common/rtcd.c
  vp8/common/setupintrarecon.c vp8/common/swapyv12buffer.c vp8/common/treecoder.c
  vp8/common/vp8_loopfilter.c vp8/common/vp8_skin_detection.c
  vp8/decoder/dboolhuff.c vp8/decoder/decodeframe.c vp8/decoder/decodemv.c
  vp8/decoder/detokenize.c vp8/decoder/onyxd_if.c vp8/decoder/threading.c vp8/vp8_dx_iface.c
  vp9/common/vp9_alloccommon.c vp9/common/vp9_blockd.c vp9/common/vp9_common_data.c
  vp9/common/vp9_entropy.c vp9/common/vp9_entropymode.c vp9/common/vp9_entropymv.c
  vp9/common/vp9_filter.c vp9/common/vp9_frame_buffers.c vp9/common/vp9_idct.c
  vp9/common/vp9_loopfilter.c vp9/common/vp9_mvref_common.c vp9/common/vp9_pred_common.c
  vp9/common/vp9_quant_common.c vp9/common/vp9_reconinter.c vp9/common/vp9_reconintra.c
  vp9/common/vp9_rtcd.c vp9/common/vp9_scale.c vp9/common/vp9_scan.c
  vp9/common/vp9_seg_common.c vp9/common/vp9_thread_common.c vp9/common/vp9_tile_common.c
  vp9/decoder/vp9_decodeframe.c vp9/decoder/vp9_decodemv.c vp9/decoder/vp9_decoder.c
  vp9/decoder/vp9_detokenize.c vp9/decoder/vp9_dsubexp.c vp9/decoder/vp9_job_queue.c
  vp9/vp9_dx_iface.c vp9/vp9_iface_common.c
  vpx/src/vpx_codec.c vpx/src/vpx_decoder.c vpx/src/vpx_image.c
  vpx_dsp/avg.c vpx_dsp/bitreader.c vpx_dsp/bitreader_buffer.c vpx_dsp/bitwriter.c
  vpx_dsp/bitwriter_buffer.c vpx_dsp/fwd_txfm.c vpx_dsp/intrapred.c vpx_dsp/inv_txfm.c
  vpx_dsp/loopfilter.c vpx_dsp/prob.c vpx_dsp/psnr.c vpx_dsp/quantize.c vpx_dsp/sad.c
  vpx_dsp/skin_detection.c vpx_dsp/sse.c vpx_dsp/subtract.c vpx_dsp/sum_squares.c
  vpx_dsp/variance.c vpx_dsp/vpx_convolve.c vpx_dsp/vpx_dsp_rtcd.c
  vpx_mem/vpx_mem.c
  vpx_scale/generic/gen_scalers.c vpx_scale/generic/vpx_scale.c vpx_scale/generic/yv12config.c
  vpx_scale/generic/yv12extend.c vpx_scale/vpx_scale_rtcd.c
  vpx_util/vpx_thread.c vpx_util/vpx_write_yuv_frame.c)
list(TRANSFORM _vpx_srcs PREPEND "${_vpx}/")
add_library(vpx STATIC ${_vpx_srcs})
target_include_directories(vpx PRIVATE ${_vpx} ${_vpx}/config/generic)
target_compile_options(vpx PRIVATE -O3 ${_video_third_party_flags})
target_link_libraries(vpx PRIVATE pthread)

# --- libdav1d -----------------------------------------------------------------------------
set(_dav1d_includes ${_dav1d} ${_dav1d}/include ${_dav1d}/config/x86_64)
set(_dav1d_flags -D_ISOC99_SOURCE -ffast-math ${_video_third_party_flags})
set(_dav1d_tmpl cdef_apply_tmpl.c cdef_tmpl.c fg_apply_tmpl.c filmgrain_tmpl.c
  ipred_prepare_tmpl.c ipred_tmpl.c itx_tmpl.c lf_apply_tmpl.c loopfilter_tmpl.c
  looprestoration_tmpl.c lr_apply_tmpl.c mc_tmpl.c recon_tmpl.c)
list(TRANSFORM _dav1d_tmpl PREPEND "${_dav1d}/src/")
foreach(_bits 8 16)
  add_library(dav1d_${_bits}bit OBJECT ${_dav1d_tmpl})
  target_include_directories(dav1d_${_bits}bit PRIVATE ${_dav1d_includes})
  target_compile_definitions(dav1d_${_bits}bit PRIVATE BITDEPTH=${_bits})
  target_compile_options(dav1d_${_bits}bit PRIVATE ${_dav1d_flags})
endforeach()
set(_dav1d_srcs cdf.c ctx.c cpu.c data.c decode.c dequant_tables.c getbits.c intra_edge.c
  itx_1d.c lf_mask.c lib.c log.c mem.c msac.c obu.c pal.c picture.c qm.c ref.c refmvs.c
  scan.c tables.c thread_task.c warpmv.c wedge.c win32/thread.c)
list(TRANSFORM _dav1d_srcs PREPEND "${_dav1d}/src/")
add_library(dav1d STATIC ${_dav1d_srcs} $<TARGET_OBJECTS:dav1d_8bit> $<TARGET_OBJECTS:dav1d_16bit>)
target_include_directories(dav1d PRIVATE ${_dav1d_includes})
target_compile_options(dav1d PRIVATE ${_dav1d_flags})

# --- libhevcdec ---------------------------------------------------------------------------
set(_hevc_srcs
  common/ihevc_buf_mgr.c common/ihevc_cabac_tables.c common/ihevc_chroma_intra_pred_filters.c
  common/ihevc_chroma_iquant_itrans_recon.c common/ihevc_chroma_iquant_recon.c
  common/ihevc_chroma_itrans_recon.c common/ihevc_chroma_itrans_recon_16x16.c
  common/ihevc_chroma_itrans_recon_8x8.c common/ihevc_chroma_recon.c
  common/ihevc_common_tables.c common/ihevc_deblk_edge_filter.c common/ihevc_deblk_tables.c
  common/ihevc_disp_mgr.c common/ihevc_dpb_mgr.c common/ihevc_inter_pred_filters.c
  common/ihevc_intra_pred_filters.c common/ihevc_iquant_itrans_recon.c
  common/ihevc_iquant_recon.c common/ihevc_itrans.c common/ihevc_itrans_recon.c
  common/ihevc_itrans_recon_16x16.c common/ihevc_itrans_recon_32x32.c
  common/ihevc_itrans_recon_8x8.c common/ihevc_mem_fns.c common/ihevc_padding.c
  common/ihevc_quant_tables.c common/ihevc_recon.c common/ihevc_sao.c
  common/ihevc_trans_tables.c common/ihevc_weighted_pred.c common/ithread.c
  decoder/ihevcd_api.c decoder/ihevcd_bitstream.c decoder/ihevcd_boundary_strength.c
  decoder/ihevcd_cabac.c decoder/ihevcd_common_tables.c decoder/ihevcd_deblk.c
  decoder/ihevcd_decode.c decoder/ihevcd_fmt_conv.c decoder/ihevcd_get_mv.c
  decoder/ihevcd_ilf_padding.c decoder/ihevcd_inter_pred.c
  decoder/ihevcd_intra_pred_mode_prediction.c decoder/ihevcd_iquant_itrans_recon_ctb.c
  decoder/ihevcd_itrans_recon_dc.c decoder/ihevcd_job_queue.c decoder/ihevcd_mv_merge.c
  decoder/ihevcd_mv_pred.c decoder/ihevcd_nal.c decoder/ihevcd_parse_headers.c
  decoder/ihevcd_parse_residual.c decoder/ihevcd_parse_slice.c
  decoder/ihevcd_parse_slice_header.c decoder/ihevcd_process_slice.c decoder/ihevcd_ref_list.c
  decoder/ihevcd_sao.c decoder/ihevcd_utils.c decoder/ihevcd_version.c
  # x86-64 (Android.bp's arch.x86_64)
  common/x86/ihevc_16x16_itrans_recon_sse42_intr.c common/x86/ihevc_32x32_itrans_recon_sse42_intr.c
  common/x86/ihevc_chroma_intra_pred_filters_sse42_intr.c
  common/x86/ihevc_chroma_intra_pred_filters_ssse3_intr.c common/x86/ihevc_deblk_ssse3_intr.c
  common/x86/ihevc_inter_pred_filters_sse42_intr.c common/x86/ihevc_inter_pred_filters_ssse3_intr.c
  common/x86/ihevc_intra_pred_filters_sse42_intr.c common/x86/ihevc_intra_pred_filters_ssse3_intr.c
  common/x86/ihevc_itrans_recon_16x16_ssse3_intr.c common/x86/ihevc_itrans_recon_32x32_ssse3_intr.c
  common/x86/ihevc_itrans_recon_sse42_intr.c common/x86/ihevc_itrans_recon_ssse3_intr.c
  common/x86/ihevc_mem_fns_ssse3_intr.c common/x86/ihevc_padding_ssse3_intr.c
  common/x86/ihevc_sao_ssse3_intr.c common/x86/ihevc_tables_x86_intr.c
  common/x86/ihevc_weighted_pred_sse42_intr.c common/x86/ihevc_weighted_pred_ssse3_intr.c
  decoder/x86/ihevcd_fmt_conv_ssse3_intr.c decoder/x86/ihevcd_function_selector.c
  decoder/x86/ihevcd_function_selector_generic.c decoder/x86/ihevcd_function_selector_sse42.c
  decoder/x86/ihevcd_function_selector_ssse3.c decoder/x86/ihevcd_it_rec_dc_sse42_intr.c
  decoder/x86/ihevcd_it_rec_dc_ssse3_intr.c)
list(TRANSFORM _hevc_srcs PREPEND "${_hevc}/")
add_library(hevcdec STATIC ${_hevc_srcs})
target_include_directories(hevcdec PRIVATE ${_hevc}/common ${_hevc}/decoder ${_hevc}/common/x86
  ${_hevc}/decoder/x86)
target_compile_definitions(hevcdec PRIVATE _LIB DISABLE_SEI X86 DISABLE_AVX2
  DEFAULT_ARCH=D_ARCH_X86_SSE42)
target_compile_options(hevcdec PRIVATE -O3 -msse4.2 -mno-avx -UKEEP_THREADS_ACTIVE
  ${_video_third_party_flags})
target_link_libraries(hevcdec PRIVATE pthread)

# The headers soft_video.cc includes (as system headers: no warnings from them).
set(GESHER_SOFT_VIDEO_INCLUDES ${_vpx} ${_dav1d}/include ${_hevc}/common ${_hevc}/decoder)
set(GESHER_SOFT_VIDEO_LIBS vpx dav1d hevcdec)
