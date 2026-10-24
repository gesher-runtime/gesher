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

# The engine's probe (compat/media/probe), run in CI on Windows
# (tests/windows/tracks/38-media.ps1): logs to stderr, no runtime.
add_executable(media_probe ${_media}/probe/media_probe.cc ${GESHER_MEDIA_AUDIO_SOURCES})
target_compile_definitions(media_probe PRIVATE GESHER_MEDIA_STANDALONE)
target_compile_options(media_probe PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(media_probe PRIVATE ole32 uuid pthread)
