# Media (docs/media.md, roadmap phase 8, ADR 0042): sound and video for apps.
#
# compat/media/audio: the audio engine of an app process (resampling, mixing,
# WASAPI shared mode, a WAV capture for machines without an audio endpoint)
# and the audio state the device's processes share. compat/media/jni: the
# framework's media natives over it (AudioTrack, AudioSystem, the audio
# policy's), registered in libandroid_runtime through register_gesher_media
# (compat/framework/android_runtime.cc).
#
# An OBJECT library: every object reaches dalvikvm, as framework_runtime's.

set(_media "${CMAKE_SOURCE_DIR}/compat/media")

# AOSP's extractors and the open codecs (cmake/aosp/media_codecs.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/media_codecs.cmake)

set(GESHER_MEDIA_AUDIO_SOURCES
  ${_media}/audio/engine.cc
  ${_media}/audio/shared_state.cc
  ${_media}/audio/sink_file.cc
  ${_media}/audio/sink_wasapi.cc)

add_library(gesher_media OBJECT
  ${GESHER_MEDIA_AUDIO_SOURCES}
  ${_media}/jni/jni_util.cc
  ${_media}/jni/audio_system_jni.cc
  ${_media}/jni/audio_track_jni.cc)
target_link_libraries(gesher_media PUBLIC nativehelper log ole32 uuid)
target_compile_options(gesher_media PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)

target_link_libraries(dalvikvm PRIVATE gesher_media)

# The player under MediaPlayer, SoundPool and MediaMetadataRetriever
# (compat/media/player) and their natives.
add_library(gesher_media_player OBJECT
  ${_media}/player/data_source.cc
  ${_media}/player/http_source.cc
  ${_media}/player/extractor.cc
  ${_media}/player/decoder.cc
  ${_media}/player/decode_file.cc
  ${_media}/player/mf_decoder.cc
  ${_media}/player/player.cc
  # Video (stage 2, ADR 0051): decoders and MediaCodec.
  ${_media}/video/video_decoder.cc
  ${_media}/video/mf_video.cc
  ${_media}/video/soft_video.cc
  ${_media}/video/soft_video_avc.cc
  ${_media}/video/soft_video_m4v.cc
  ${_media}/video/render.cc
  ${_media}/jni/media_codec_jni.cc
  ${_media}/jni/media_jni.cc
  ${_media}/jni/media_player_jni.cc
  ${_media}/jni/media_jni_others.cc
  ${_media}/jni/media_extractor_jni.cc
  ${_media}/jni/soundpool_jni.cc
  # The NDK's AAudio and OpenSL ES for apps' ELF libraries (bionic shim symbols).
  ${_media}/ndk/aaudio.cc
  ${_media}/ndk/opensles.cc)
target_include_directories(gesher_media_player PRIVATE ${CMAKE_SOURCE_DIR}/compat/bionic)
target_link_libraries(gesher_media_player PUBLIC gesher_media gesher_media_codecs gesher_bionic
  winhttp ole32)
target_compile_options(gesher_media_player PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter
  # libutils' TypeHelpers.h trips -Wdeprecated-copy on key_value_pair_t<String8, String8>.
  -Wno-deprecated-copy)
set_target_properties(gesher_media_player PROPERTIES CXX_STANDARD 23)
# The software video decoders' headers (cmake/aosp/media_video_codecs.cmake), as system
# headers: -Werror holds for soft_video.cc itself.
set(_soft_video_isystem)
foreach(_d ${GESHER_SOFT_VIDEO_INCLUDES})
  list(APPEND _soft_video_isystem "-isystem${_d}")
endforeach()
set_source_files_properties(${_media}/video/soft_video.cc PROPERTIES COMPILE_OPTIONS "${_soft_video_isystem}")
foreach(_s AVC M4V)
  set(_isystem)
  foreach(_d ${GESHER_SOFT_${_s}_INCLUDES})
    list(APPEND _isystem "-isystem${_d}")
  endforeach()
  string(TOLOWER ${_s} _file)
  set_source_files_properties(${_media}/video/soft_video_${_file}.cc PROPERTIES COMPILE_OPTIONS "${_isystem}")
endforeach()
add_dependencies(media_codecs_all gesher_media_player)

target_link_libraries(dalvikvm PRIVATE gesher_media_player)

# The engine's probe (compat/media/probe), run in CI on Windows
# (tests/windows/tracks/38-media.ps1): logs to stderr, no runtime.
add_executable(media_probe ${_media}/probe/media_probe.cc ${GESHER_MEDIA_AUDIO_SOURCES})
target_compile_definitions(media_probe PRIVATE GESHER_MEDIA_STANDALONE)
target_compile_options(media_probe PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(media_probe PRIVATE ole32 uuid pthread)

# The surfaces hwui draws (stage 2, ADR 0051): SurfaceTexture and TextureView's layer,
# SurfaceView's surface (compat/media/surface), built with hwui (its headers, Skia).
target_sources(hwui PRIVATE
  ${_media}/surface/queue_surface.cc
  ${_media}/surface/surface_jni.cc)
