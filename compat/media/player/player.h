// Gesher media: the player under android.media.MediaPlayer (docs/media.md).
//
// What libmedia's MediaPlayer (the client state machine) and NuPlayer (the
// playback) do for an app on Android, in the app's process: the states and
// their rules follow frameworks/av media/libmedia/mediaplayer.cpp (an
// operation in the wrong state answers INVALID_OPERATION, and the JNI layer
// turns that into an exception or a MEDIA_ERROR as Android's does); the
// playback runs an extractor, a decoder and an engine track on a thread of
// its own.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "data_source.h"

namespace gesher::media {

// android::status_t values the JNI layer maps.
constexpr int kPlayerOk = 0;
constexpr int kPlayerInvalidOperation = -38;
constexpr int kPlayerBadValue = -22;
constexpr int kPlayerUnknownError = -2147483647 - 1;
// MediaPlayer's error codes (ext1 of MEDIA_ERROR).
constexpr int kMediaErrorIo = -1004;
constexpr int kMediaErrorMalformed = -1007;
constexpr int kMediaErrorUnsupported = -1010;
// MediaPlayer's events.
enum PlayerEvent {
  kMediaPrepared = 1,
  kMediaPlaybackComplete = 2,
  kMediaBufferingUpdate = 3,
  kMediaSeekComplete = 4,
  kMediaSetVideoSize = 5,
  kMediaStarted = 6,
  kMediaPaused = 7,
  kMediaStopped = 8,
  kMediaError = 100,
  kMediaInfo = 200,
};

class Playback;

class Player {
 public:
  // Called with each event (in order, on the engine's event thread).
  using Listener = std::function<void(int what, int arg1, int arg2)>;

  Player();
  ~Player();

  void SetListener(Listener l);
  int SetDataSource(std::shared_ptr<DataSource> source);
  int Prepare();        // waits; the error code, not an event, on failure
  int PrepareAsync();   // kMediaPrepared or kMediaError later
  int Start();
  int Stop();
  int Pause();
  bool IsPlaying();
  int SeekTo(int msec, int mode);
  int GetCurrentPosition(int* msec);
  int GetDuration(int* msec);
  int Reset();
  void Release();
  int SetLooping(bool loop);
  bool IsLooping();
  int SetVolume(float left, float right);
  int SetAudioStreamType(int stream);
  int GetAudioStreamType(int* stream);
  int SetSpeed(float speed);
  float speed();
  // An operation failed where the JNI layer throws nothing: MEDIA_ERROR
  // (status, 0) goes to the app, as libmedia's process_media_player_call does.
  void ReportError(int status);
  int session_id() const { return session_id_; }
  void set_session_id(int id) { session_id_ = id; }

 private:
  friend class Playback;
  // From the playback thread: updates the state as MediaPlayer::notify does,
  // then tells the listener (not for an error ending a synchronous prepare).
  void Notify(int what, int arg1, int arg2);

  std::mutex mu_;
  std::condition_variable prepared_cv_;
  unsigned state_;
  std::shared_ptr<DataSource> source_;
  std::shared_ptr<Playback> playback_;
  Listener listener_;
  bool loop_ = false;
  float left_ = 1.0f, right_ = 1.0f;
  float speed_ = 1.0f;
  int stream_ = 3;
  int session_id_ = 0;
  bool prepare_sync_ = false;
  int prepare_status_ = 0;
  int current_position_ = -1;  // a seek's target, until it completes
  int seek_position_ = -1;
  int seek_mode_ = 0;
  int current_seek_mode_ = 0;
};

}  // namespace gesher::media
