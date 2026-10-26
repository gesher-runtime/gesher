# Media (docs/media.md, ADR 0042): AOSP's media extractors and the open audio
# codecs, built for Windows from their Android.bp source sets, for the player
# under MediaPlayer, SoundPool, MediaExtractor and MediaMetadataRetriever
# (compat/media/player).
#
#  - libstagefright_foundation (without IMemory), libstagefright_id3, _esds,
#    _metadatautils, libmediandk_format and libmedia_ndkformatpriv: what the
#    extractors are written against. frameworks/av-media in aosp.lock is a
#    checkout of frameworks/av of its own (and frameworks/native-media one of
#    frameworks/native's media_plugin headers), so the framework build's sparse
#    sets are untouched;
#  - the extractors (MP3, MPEG-4/M4A, Ogg, FLAC, WAV, AAC ADTS, Matroska/WebM), each built
#    with its GETEXTRACTORDEF renamed, as each is a plugin of its own on Android;
#  - the decoders: libstagefright_mp3dec (MP3), libvorbisidec (Tremolo:
#    Vorbis), libopus (Opus), libFLAC (FLACExtractor decodes FLAC itself).
#    AAC goes to Media Foundation's decoder (ADR 0042).
#
# compat/media/aosp/include holds what Gesher stands in for (the extractors'
# aconfig flags, the NDK's binder and api-level calls they make).

set(_avm "${AOSP_DIR}/frameworks/av-media")
set(_mod "${_avm}/media/module")
set(_sysmedia "${AOSP_DIR}/system/media")
set(_trem "${AOSP_DIR}/external/tremolo")
set(_opus "${AOSP_DIR}/external/libopus")
set(_flac "${AOSP_DIR}/external/flac")
set(_media_aosp "${CMAKE_SOURCE_DIR}/compat/media/aosp")

# Headers every piece below sees (header_libs: av-headers, media_ndk_headers,
# libstagefright_headers, libstagefright_foundation_headers,
# libmedia_datasource_headers, media_plugin_headers, libaudio_system_headers,
# libaudioclient_headers, libstagefright_id3_headers).
add_library(gesher_media_aosp_headers INTERFACE)
target_include_directories(gesher_media_aosp_headers INTERFACE
  ${_media_aosp}/include
  ${_avm}/include
  ${_avm}/media/ndk/include
  ${_avm}/media/libstagefright/include
  ${_mod}/foundation/include
  ${_avm}/media/libmedia/include
  ${_avm}/media/libaudioclient/include
  ${_mod}/id3/include
  ${_mod}/esds/include
  ${_mod}/metadatautils/include
  ${AOSP_DIR}/frameworks/native-media/headers/media_plugin
  ${_sysmedia}/audio/include
  ${_sysmedia}/audio_utils/include)
target_link_libraries(gesher_media_aosp_headers INTERFACE utils cutils log base binder)

set(_media_aosp_flags -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
  -Wno-unused-but-set-variable -Wno-implicit-fallthrough -Wno-sign-compare
  -Wno-deprecated-declarations -Wno-missing-field-initializers -Wno-multichar
  -Wno-deprecated-copy -Wno-inconsistent-missing-override -Wno-unused-private-field
  -Wno-format -Wno-c99-designator -Wno-vla-cxx-extension
  "SHELL:-include ${_media_aosp}/include/gesher_media_aosp.h")

# --- libstagefright_foundation_without_imemory and its companions ------------------------
set(_fnd "${_mod}/foundation")
add_library(stagefright_foundation STATIC
  ${_fnd}/AAtomizer.cpp ${_fnd}/ABitReader.cpp ${_fnd}/ABuffer.cpp ${_fnd}/ADebug.cpp
  ${_fnd}/AHandler.cpp ${_fnd}/ALooper.cpp ${_fnd}/ALooperRoster.cpp ${_fnd}/AMessage.cpp
  ${_fnd}/AString.cpp ${_fnd}/AStringUtils.cpp ${_fnd}/AudioPresentationInfo.cpp
  ${_fnd}/ByteUtils.cpp ${_fnd}/ColorUtils.cpp ${_fnd}/ColorUtils_fill.cpp
  ${_fnd}/FoundationUtils.cpp ${_fnd}/MediaBuffer.cpp ${_fnd}/MediaBufferBase.cpp
  ${_fnd}/MediaBufferGroup.cpp ${_fnd}/MediaDefs.cpp ${_fnd}/MediaKeys.cpp ${_fnd}/MetaData.cpp
  ${_fnd}/MetaDataBase.cpp ${_fnd}/OpusHeader.cpp ${_fnd}/avc_utils.cpp ${_fnd}/base64.cpp
  ${_fnd}/hexdump.cpp
  # libstagefright_id3, libstagefright_esds, libstagefright_metadatautils
  ${_mod}/id3/ID3.cpp
  ${_mod}/esds/ESDS.cpp
  ${_mod}/metadatautils/MetaDataUtils.cpp
  # libmediandk_format, libmedia_ndkformatpriv
  ${_avm}/media/ndk/NdkMediaFormat.cpp
  ${_avm}/media/libmedia/NdkMediaErrorPriv.cpp
  ${_avm}/media/libmedia/NdkMediaFormatPriv.cpp
  # audio_utils' sample conversions (the FLAC and WAV extractors)
  ${_sysmedia}/audio_utils/primitives.c)
target_include_directories(stagefright_foundation PRIVATE ${_fnd}/include/media/stagefright/foundation)
target_link_libraries(stagefright_foundation PUBLIC gesher_media_aosp_headers)
target_compile_definitions(stagefright_foundation PRIVATE NO_IMEMORY
  "EXPORT=__attribute__((visibility(\"default\")))")
target_compile_options(stagefright_foundation PRIVATE ${_media_aosp_flags})
set_target_properties(stagefright_foundation PROPERTIES CXX_STANDARD 23)

# --- The extractors ----------------------------------------------------------------------
# One library each, GETEXTRACTORDEF renamed: compat/media/player/extractor.cc lists them.
set(GESHER_MEDIA_EXTRACTORS)
function(gesher_media_extractor name)
  add_library(extractor_${name} STATIC ${ARGN})
  target_include_directories(extractor_${name} PRIVATE ${_mod}/extractors/${name}/include
    ${_mod}/extractors/${name})
  target_link_libraries(extractor_${name} PUBLIC stagefright_foundation)
  target_compile_definitions(extractor_${name} PRIVATE GETEXTRACTORDEF=GesherExtractorDef_${name})
  target_compile_options(extractor_${name} PRIVATE ${_media_aosp_flags})
  set_target_properties(extractor_${name} PROPERTIES CXX_STANDARD 23)
  set(GESHER_MEDIA_EXTRACTORS ${GESHER_MEDIA_EXTRACTORS} extractor_${name} PARENT_SCOPE)
endfunction()
set(_ex "${_mod}/extractors")
gesher_media_extractor(mp3 ${_ex}/mp3/MP3Extractor.cpp ${_ex}/mp3/VBRISeeker.cpp ${_ex}/mp3/XINGSeeker.cpp)
gesher_media_extractor(mp4 ${_ex}/mp4/AC4Parser.cpp ${_ex}/mp4/HeifCleanAperture.cpp
  ${_ex}/mp4/ItemTable.cpp ${_ex}/mp4/MPEG4Extractor.cpp ${_ex}/mp4/SampleIterator.cpp
  ${_ex}/mp4/SampleTable.cpp)
gesher_media_extractor(ogg ${_ex}/ogg/OggExtractor.cpp)
gesher_media_extractor(flac ${_ex}/flac/FLACExtractor.cpp)
gesher_media_extractor(wav ${_ex}/wav/WAVExtractor.cpp)
gesher_media_extractor(aac ${_ex}/aac/AACExtractor.cpp)
# Matroska/WebM: libwebm's mkvparser and libstagefright_flacdec (FLAC's CodecPrivate)
# built in, with ColorUtils_ndk.cpp (libstagefright_foundation_colorutils_ndk).
gesher_media_extractor(mkv ${_ex}/mkv/MatroskaExtractor.cpp
  ${AOSP_DIR}/external/libwebm/mkvparser/mkvparser.cc
  ${_mod}/codecs/flac/dec/FLACDecoder.cpp
  ${_fnd}/ColorUtils_ndk.cpp)
target_include_directories(extractor_mkv PRIVATE ${AOSP_DIR}/external/libwebm ${_mod}/codecs/flac/dec)

# --- libvorbisidec (Tremolo, C only) ----------------------------------------------------
set(_t "${_trem}/Tremolo")
add_library(vorbisidec STATIC
  ${_t}/bitwise.c ${_t}/codebook.c ${_t}/dsp.c ${_t}/floor0.c ${_t}/floor1.c
  ${_t}/floor_lookup.c ${_t}/framing.c ${_t}/mapping0.c ${_t}/mdct.c ${_t}/misc.c
  ${_t}/res012.c ${_t}/treminfo.c ${_t}/vorbisfile.c)
# _G_config.h: Tremolo takes llvm-mingw for Cygwin.
target_include_directories(vorbisidec PUBLIC ${_trem} PRIVATE ${_t} ${_media_aosp}/include)
target_compile_definitions(vorbisidec PRIVATE ONLY_C _GNU_SOURCE)
target_compile_options(vorbisidec PRIVATE -funsigned-char -Wno-unused-variable
  -Wno-unused-but-set-variable)
target_link_libraries(vorbisidec PRIVATE log)
target_link_libraries(extractor_ogg PRIVATE vorbisidec)

# --- libFLAC ----------------------------------------------------------------------------
set(_fl "${_flac}/src/libFLAC")
add_library(FLAC STATIC
  ${_fl}/bitmath.c ${_fl}/bitreader.c ${_fl}/bitwriter.c ${_fl}/cpu.c ${_fl}/crc.c
  ${_fl}/fixed.c ${_fl}/float.c ${_fl}/format.c ${_fl}/lpc.c ${_fl}/memory.c ${_fl}/md5.c
  ${_fl}/stream_decoder.c ${_fl}/stream_encoder.c ${_fl}/stream_encoder_framing.c ${_fl}/window.c
  ${_media_aosp}/flac/fopen_utf8.c)
# compat/media/aosp/flac/config.h in place of external/flac's Linux one.
target_include_directories(FLAC PUBLIC ${_flac}/include PRIVATE ${_media_aosp}/flac ${_fl}/include)
target_compile_definitions(FLAC PUBLIC FLAC__NO_DLL PRIVATE HAVE_CONFIG_H FLAC__NO_MD5
  FLAC__INTEGER_ONLY_LIBRARY _REENTRANT PIC U_COMMON_IMPLEMENTATION)
target_compile_options(FLAC PRIVATE -Wno-unused-parameter -Wno-unreachable-code-loop-increment)
target_link_libraries(extractor_flac PRIVATE FLAC)
target_link_libraries(extractor_mkv PRIVATE FLAC)

# --- libopus (fixed point, C) -----------------------------------------------------------
add_library(opus STATIC
  ${_opus}/celt/bands.c
  ${_opus}/celt/celt.c
  ${_opus}/celt/celt_encoder.c
  ${_opus}/celt/celt_decoder.c
  ${_opus}/celt/cwrs.c
  ${_opus}/celt/entcode.c
  ${_opus}/celt/entdec.c
  ${_opus}/celt/entenc.c
  ${_opus}/celt/kiss_fft.c
  ${_opus}/celt/laplace.c
  ${_opus}/celt/mathops.c
  ${_opus}/celt/mdct.c
  ${_opus}/celt/modes.c
  ${_opus}/celt/pitch.c
  ${_opus}/celt/celt_lpc.c
  ${_opus}/celt/quant_bands.c
  ${_opus}/celt/rate.c
  ${_opus}/celt/vq.c
  ${_opus}/silk/CNG.c
  ${_opus}/silk/code_signs.c
  ${_opus}/silk/init_decoder.c
  ${_opus}/silk/decode_core.c
  ${_opus}/silk/decode_frame.c
  ${_opus}/silk/decode_parameters.c
  ${_opus}/silk/decode_indices.c
  ${_opus}/silk/decode_pulses.c
  ${_opus}/silk/decoder_set_fs.c
  ${_opus}/silk/dec_API.c
  ${_opus}/silk/enc_API.c
  ${_opus}/silk/encode_indices.c
  ${_opus}/silk/encode_pulses.c
  ${_opus}/silk/gain_quant.c
  ${_opus}/silk/interpolate.c
  ${_opus}/silk/LP_variable_cutoff.c
  ${_opus}/silk/NLSF_decode.c
  ${_opus}/silk/NSQ.c
  ${_opus}/silk/NSQ_del_dec.c
  ${_opus}/silk/PLC.c
  ${_opus}/silk/shell_coder.c
  ${_opus}/silk/tables_gain.c
  ${_opus}/silk/tables_LTP.c
  ${_opus}/silk/tables_NLSF_CB_NB_MB.c
  ${_opus}/silk/tables_NLSF_CB_WB.c
  ${_opus}/silk/tables_other.c
  ${_opus}/silk/tables_pitch_lag.c
  ${_opus}/silk/tables_pulses_per_block.c
  ${_opus}/silk/VAD.c
  ${_opus}/silk/control_audio_bandwidth.c
  ${_opus}/silk/quant_LTP_gains.c
  ${_opus}/silk/VQ_WMat_EC.c
  ${_opus}/silk/HP_variable_cutoff.c
  ${_opus}/silk/NLSF_encode.c
  ${_opus}/silk/NLSF_VQ.c
  ${_opus}/silk/NLSF_unpack.c
  ${_opus}/silk/NLSF_del_dec_quant.c
  ${_opus}/silk/process_NLSFs.c
  ${_opus}/silk/stereo_LR_to_MS.c
  ${_opus}/silk/stereo_MS_to_LR.c
  ${_opus}/silk/check_control_input.c
  ${_opus}/silk/control_SNR.c
  ${_opus}/silk/init_encoder.c
  ${_opus}/silk/control_codec.c
  ${_opus}/silk/A2NLSF.c
  ${_opus}/silk/ana_filt_bank_1.c
  ${_opus}/silk/biquad_alt.c
  ${_opus}/silk/bwexpander_32.c
  ${_opus}/silk/bwexpander.c
  ${_opus}/silk/debug.c
  ${_opus}/silk/decode_pitch.c
  ${_opus}/silk/inner_prod_aligned.c
  ${_opus}/silk/lin2log.c
  ${_opus}/silk/log2lin.c
  ${_opus}/silk/LPC_analysis_filter.c
  ${_opus}/silk/LPC_fit.c
  ${_opus}/silk/LPC_inv_pred_gain.c
  ${_opus}/silk/table_LSF_cos.c
  ${_opus}/silk/NLSF2A.c
  ${_opus}/silk/NLSF_stabilize.c
  ${_opus}/silk/NLSF_VQ_weights_laroia.c
  ${_opus}/silk/pitch_est_tables.c
  ${_opus}/silk/resampler.c
  ${_opus}/silk/resampler_down2_3.c
  ${_opus}/silk/resampler_down2.c
  ${_opus}/silk/resampler_private_AR2.c
  ${_opus}/silk/resampler_private_down_FIR.c
  ${_opus}/silk/resampler_private_IIR_FIR.c
  ${_opus}/silk/resampler_private_up2_HQ.c
  ${_opus}/silk/resampler_rom.c
  ${_opus}/silk/sigm_Q15.c
  ${_opus}/silk/sort.c
  ${_opus}/silk/sum_sqr_shift.c
  ${_opus}/silk/stereo_decode_pred.c
  ${_opus}/silk/stereo_encode_pred.c
  ${_opus}/silk/stereo_find_predictor.c
  ${_opus}/silk/stereo_quant_pred.c
  ${_opus}/silk/fixed/LTP_analysis_filter_FIX.c
  ${_opus}/silk/fixed/LTP_scale_ctrl_FIX.c
  ${_opus}/silk/fixed/corrMatrix_FIX.c
  ${_opus}/silk/fixed/encode_frame_FIX.c
  ${_opus}/silk/fixed/find_LPC_FIX.c
  ${_opus}/silk/fixed/find_LTP_FIX.c
  ${_opus}/silk/fixed/find_pitch_lags_FIX.c
  ${_opus}/silk/fixed/find_pred_coefs_FIX.c
  ${_opus}/silk/fixed/noise_shape_analysis_FIX.c
  ${_opus}/silk/fixed/process_gains_FIX.c
  ${_opus}/silk/fixed/regularize_correlations_FIX.c
  ${_opus}/silk/fixed/residual_energy16_FIX.c
  ${_opus}/silk/fixed/residual_energy_FIX.c
  ${_opus}/silk/fixed/warped_autocorrelation_FIX.c
  ${_opus}/silk/fixed/apply_sine_window_FIX.c
  ${_opus}/silk/fixed/autocorr_FIX.c
  ${_opus}/silk/fixed/burg_modified_FIX.c
  ${_opus}/silk/fixed/k2a_FIX.c
  ${_opus}/silk/fixed/k2a_Q16_FIX.c
  ${_opus}/silk/fixed/pitch_analysis_core_FIX.c
  ${_opus}/silk/fixed/vector_ops_FIX.c
  ${_opus}/silk/fixed/schur64_FIX.c
  ${_opus}/silk/fixed/schur_FIX.c
  ${_opus}/src/mapping_matrix.c
  ${_opus}/src/opus.c
  ${_opus}/src/opus_decoder.c
  ${_opus}/src/opus_encoder.c
  ${_opus}/src/opus_multistream.c
  ${_opus}/src/opus_multistream_encoder.c
  ${_opus}/src/opus_multistream_decoder.c
  ${_opus}/src/opus_projection_encoder.c
  ${_opus}/src/opus_projection_decoder.c
  ${_opus}/src/repacketizer.c
  ${_opus}/src/extensions.c
  ${_opus}/src/analysis.c
  ${_opus}/src/mlp.c
  ${_opus}/src/mlp_data.c)
target_include_directories(opus PUBLIC ${_opus}/include
  PRIVATE ${_opus}/src ${_opus}/silk ${_opus}/celt ${_opus}/silk/fixed)
target_compile_definitions(opus PRIVATE NULL=0 LOCALE_NOT_USED restrict= OPUS_BUILD
  FIXED_POINT USE_ALLOCA SIMD_EXTRA_ALLOC_BYTES=16 HAVE_LRINT HAVE_LRINTF ENABLE_HARDENING)
target_compile_options(opus PRIVATE -fno-math-errno -Wno-unused-variable)

# --- libstagefright_mp3dec --------------------------------------------------------------
set(_mp3 "${_mod}/codecs/mp3dec")
add_library(stagefright_mp3dec STATIC
  ${_mp3}/src/pvmp3_normalize.cpp
  ${_mp3}/src/pvmp3_alias_reduction.cpp
  ${_mp3}/src/pvmp3_crc.cpp
  ${_mp3}/src/pvmp3_decode_header.cpp
  ${_mp3}/src/pvmp3_decode_huff_cw.cpp
  ${_mp3}/src/pvmp3_getbits.cpp
  ${_mp3}/src/pvmp3_dequantize_sample.cpp
  ${_mp3}/src/pvmp3_framedecoder.cpp
  ${_mp3}/src/pvmp3_get_main_data_size.cpp
  ${_mp3}/src/pvmp3_get_side_info.cpp
  ${_mp3}/src/pvmp3_get_scale_factors.cpp
  ${_mp3}/src/pvmp3_mpeg2_get_scale_data.cpp
  ${_mp3}/src/pvmp3_mpeg2_get_scale_factors.cpp
  ${_mp3}/src/pvmp3_mpeg2_stereo_proc.cpp
  ${_mp3}/src/pvmp3_huffman_decoding.cpp
  ${_mp3}/src/pvmp3_huffman_parsing.cpp
  ${_mp3}/src/pvmp3_tables.cpp
  ${_mp3}/src/pvmp3_imdct_synth.cpp
  ${_mp3}/src/pvmp3_mdct_6.cpp
  ${_mp3}/src/pvmp3_dct_6.cpp
  ${_mp3}/src/pvmp3_poly_phase_synthesis.cpp
  ${_mp3}/src/pvmp3_equalizer.cpp
  ${_mp3}/src/pvmp3_seek_synch.cpp
  ${_mp3}/src/pvmp3_stereo_proc.cpp
  ${_mp3}/src/pvmp3_reorder.cpp
  ${_mp3}/src/pvmp3_polyphase_filter_window.cpp
  ${_mp3}/src/pvmp3_mdct_18.cpp
  ${_mp3}/src/pvmp3_dct_9.cpp
  ${_mp3}/src/pvmp3_dct_16.cpp)
target_include_directories(stagefright_mp3dec PUBLIC ${_mp3}/include ${_mp3}/src)
# CMake drops function-like macros from COMPILE_DEFINITIONS: take OSCL_UNUSED_ARG
# from the force-included gesher_media_aosp.h instead.
target_compile_options(stagefright_mp3dec PRIVATE -Wno-unused-parameter
  "SHELL:-include ${_media_aosp}/include/gesher_media_aosp.h")

# The software video decoders (libvpx, libdav1d, libhevc).
include(${CMAKE_CURRENT_LIST_DIR}/media_video_codecs.cmake)

# Everything the player (cmake/aosp/media.cmake) links.
add_library(gesher_media_codecs INTERFACE)
target_link_libraries(gesher_media_codecs INTERFACE ${GESHER_MEDIA_EXTRACTORS}
  stagefright_foundation vorbisidec FLAC opus stagefright_mp3dec ${GESHER_SOFT_VIDEO_LIBS})

# Everything of the port under one name (kept for a `ninja media_codecs_all`
# while debugging; the default build now carries all of it into dalvikvm).
add_custom_target(media_codecs_all DEPENDS ${GESHER_MEDIA_EXTRACTORS} stagefright_foundation
  vorbisidec FLAC opus stagefright_mp3dec ${GESHER_SOFT_VIDEO_LIBS})
