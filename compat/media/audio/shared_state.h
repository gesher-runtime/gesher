// Gesher: the audio state every process of a Gesher device shares
// (docs/media.md): stream volume indexes, mutes, the master volume, which
// streams play, the next audio session id.
//
// On Android, AudioService (system server) keeps the volumes and
// AudioFlinger (audioserver) applies them to every app's tracks. On Gesher
// each app process mixes its own tracks, so the system server's AudioService
// writes the volumes here (through AudioSystem's natives) and each process's
// engine reads them. The protected device gives guests a read-only view of
// the controls, and tracks playback activity separately per process.
#pragma once

#include <cstdint>

#include <gesher/audio_device_state.h>

namespace gesher::media {

// Volumes: index in [min, max]; a group plays at Gain(stream).
void InitStreamVolume(int stream, int min_index, int max_index);
void SetStreamVolume(int stream, int index, bool muted);
int GetStreamVolume(int stream);
int GetStreamMinVolume(int stream);
int GetStreamMaxVolume(int stream);
bool GetStreamMuted(int stream);
// The amplitude a stream plays at (index / max, 0 when muted): the Windows
// session volume of its group.
float StreamGain(int stream);
// From the Windows volume mixer: the group's session volume and mute.
void SetStreamGainFromSession(int stream, float level, bool muted);

void SetMasterVolume(float v);
float GetMasterVolume();
void SetMasterMute(bool mute);
bool GetMasterMute();

// Changes whenever any of the above does.
uint32_t Generation();

// A stream is playing (in this process) until about `until_ms` (GetTickCount64).
void PreparePlaybackActivity();
void UseBrokerAudioControls(audio_state::DeviceState* controls);
void MarkStreamActive(int stream, uint64_t until_ms);
bool StreamActive(int stream, int in_past_ms);

// AudioSystem.newAudioSessionId: unique on the device.
int NewSessionId();

}  // namespace gesher::media
