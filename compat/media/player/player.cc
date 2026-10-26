// Gesher media: the player under android.media.MediaPlayer (player.h).

#include "player.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <vector>

#include "../audio/engine.h"
#include "../audio/log.h"
#include "../video/video_decoder.h"
#include "decoder.h"
#include "extractor.h"

// hostgraphics' ANativeWindow references (dalvikvm defines them).
extern "C" {
void ANativeWindow_acquire(ANativeWindow* window);
void ANativeWindow_release(ANativeWindow* window);
}

namespace gesher::media {
namespace {

// libmedia's media_player_states.
enum : unsigned {
  kStateError = 0,
  kIdle = 1 << 0,
  kInitialized = 1 << 1,
  kPreparing = 1 << 2,
  kPrepared = 1 << 3,
  kStarted = 1 << 4,
  kPaused = 1 << 5,
  kStopped = 1 << 6,
  kPlaybackComplete = 1 << 7,
};

}  // namespace

// --- Playback: one prepared data source, on a thread --------------------------------------

class Playback : public std::enable_shared_from_this<Playback> {
 public:
  Playback(Player* owner, std::shared_ptr<DataSource> source)
      : owner_(owner), source_(std::move(source)) {}

  ~Playback() { Shutdown(); }

  void Begin() {
    auto self = shared_from_this();
    thread_ = std::thread([self] { self->Run(); });
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      quit_ = true;
      owner_ = nullptr;  // nothing more is reported
      if (source_ != nullptr) source_->Cancel();
    }
    cv_.notify_all();
    for (std::thread* th : {&thread_, &video_thread_}) {
      if (!th->joinable()) continue;
      if (th->get_id() == std::this_thread::get_id()) {
        th->detach();
      } else {
        th->join();
      }
    }
    std::shared_ptr<Track> t;
    ANativeWindow* window;
    {
      std::lock_guard<std::mutex> lock(mu_);
      t = std::move(track_);
      window = window_;
      window_ = nullptr;
    }
    if (t != nullptr) t->Release();
    if (window != nullptr) ANativeWindow_release(window);
  }

  // The Surface the video is drawn into (a reference of its own).
  void SetWindow(ANativeWindow* window) {
    if (window != nullptr) ANativeWindow_acquire(window);
    ANativeWindow* old;
    {
      std::lock_guard<std::mutex> lock(mu_);
      old = window_;
      window_ = window;
      // A paused player shows its current frame in the new surface.
      show_current_ = true;
    }
    cv_.notify_all();
    if (old != nullptr) ANativeWindow_release(old);
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (completed_) {
      completed_ = false;
      seek_us_ = 0;
      seek_mode_ = 0;
      seek_notify_ = false;
    }
    playing_ = true;
    if (track_ != nullptr) track_->Start();
    if (!clock_running_) {
      clock_anchor_ = std::chrono::steady_clock::now();
      clock_running_ = true;
    }
    cv_.notify_all();
  }

  void Pause() {
    std::lock_guard<std::mutex> lock(mu_);
    playing_ = false;
    if (track_ != nullptr) track_->Pause();
    clock_base_us_ = WallClockLocked();
    clock_running_ = false;
    cv_.notify_all();
  }

  bool IsPlaying() {
    std::lock_guard<std::mutex> lock(mu_);
    return playing_ && !completed_;
  }

  void Seek(int64_t us, int mode) {
    std::lock_guard<std::mutex> lock(mu_);
    seek_us_ = std::max<int64_t>(us, 0);
    seek_mode_ = mode;
    seek_notify_ = true;
    completed_ = false;
    cv_.notify_all();
  }

  int64_t PositionUs() {
    std::lock_guard<std::mutex> lock(mu_);
    if (completed_) return duration_us_ > 0 ? duration_us_ : last_us_;
    if (seek_us_ >= 0) return seek_us_;
    return MediaUsLocked();
  }

  // The media time now (with mu_ held): the audio's played position, or, for
  // a video alone, the wall clock since the start.
  int64_t MediaUsLocked() {
    if (decoder_ == nullptr) {
      last_us_ = WallClockLocked();
      return last_us_;
    }
    int64_t played = track_ != nullptr ? track_->Position() : 0;
    if (segments_.empty() || rate_ == 0) return base_us_;
    const Segment* seg = &segments_.front();
    for (const Segment& s : segments_) {
      if (s.start_frame <= played) seg = &s;
    }
    int64_t us = seg->start_us + (played - seg->start_frame) * 1000000 / rate_;
    last_us_ = us;
    return us;
  }

  int64_t DurationUs() {
    std::lock_guard<std::mutex> lock(mu_);
    return duration_us_;
  }

  void SetLooping(bool loop) {
    std::lock_guard<std::mutex> lock(mu_);
    loop_ = loop;
  }

  void SetVolume(float l, float r) {
    std::lock_guard<std::mutex> lock(mu_);
    left_ = l;
    right_ = r;
    if (track_ != nullptr) track_->SetVolume(l, r);
  }

  void SetStream(int stream) {
    std::lock_guard<std::mutex> lock(mu_);
    stream_ = stream;
  }

  void SetSpeed(float speed) {
    std::lock_guard<std::mutex> lock(mu_);
    speed_ = speed;
    if (track_ != nullptr) track_->SetSpeed(speed);
  }

  void SetSession(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    session_ = id;
  }

 private:
  struct Segment {
    int64_t start_frame;  // frames written to the track when it starts
    int64_t start_us;     // media time of its first frame
  };

  void Notify(int what, int a, int b) {
    Player* owner;
    {
      std::lock_guard<std::mutex> lock(mu_);
      owner = owner_;
    }
    if (owner != nullptr) owner->Notify(what, a, b);
  }

  bool Open() {
    std::string error;
    extractor_ = Extractor::Open(source_, &error);
    if (extractor_ == nullptr) {
      MEDIA_LOGE("player: %s", error.c_str());
      Notify(kMediaError, kMediaErrorUnsupported, 0);
      return false;
    }
    int64_t file_duration = -1;
    Format file = extractor_->FileFormat();
    file.GetInt64("durationUs", &file_duration);
    std::string unsupported;
    // The first video track a decoder takes.
    for (size_t i = 0; i < extractor_->TrackCount(); i++) {
      Format f = extractor_->TrackFormat(i);
      std::string mime = f.Mime();
      if (mime.rfind("video/", 0) != 0) continue;
      video_decoder_ = VideoDecoder::Create(f, &error);
      if (video_decoder_ == nullptr) {
        MEDIA_LOGW("player: %s", error.c_str());
        continue;
      }
      video_reader_ = extractor_->OpenTrack(i);
      int32_t w = 0, h = 0;
      f.GetInt32("width", &w);
      f.GetInt32("height", &h);
      int64_t d = -1;
      if (!f.GetInt64("durationUs", &d)) d = file_duration;
      std::lock_guard<std::mutex> lock(mu_);
      video_width_ = w;
      video_height_ = h;
      if (d > duration_us_) duration_us_ = d;
      MEDIA_LOGI("player: %s, %s %dx%d, %.3f s (%s extractor, %s)", source_->Uri().c_str(),
                 mime.c_str(), w, h, d / 1e6, extractor_->Name().c_str(),
                 video_decoder_->Name().c_str());
      break;
    }
    for (size_t i = 0; i < extractor_->TrackCount(); i++) {
      Format f = extractor_->TrackFormat(i);
      std::string mime = f.Mime();
      if (mime.rfind("audio/", 0) != 0) continue;
      decoder_ = AudioDecoder::Create(f, &error);
      if (decoder_ == nullptr) {
        unsupported = error;
        continue;
      }
      reader_ = extractor_->OpenTrack(i);
      int64_t d = -1;
      if (!f.GetInt64("durationUs", &d)) d = file_duration;
      std::lock_guard<std::mutex> lock(mu_);
      if (d > duration_us_) duration_us_ = d;
      MEDIA_LOGI("player: %s, %s %u Hz %u ch, %.3f s (%s extractor)", source_->Uri().c_str(),
                 mime.c_str(), decoder_->info().rate, decoder_->info().channels, d / 1e6,
                 extractor_->Name().c_str());
      break;
    }
    if ((decoder_ == nullptr || reader_ == nullptr) && reader_ != nullptr) reader_.reset();
    if (decoder_ == nullptr) reader_.reset();
    if (decoder_ == nullptr && video_decoder_ == nullptr) {
      MEDIA_LOGE("player: no playable audio or video track (%s)", unsupported.c_str());
      Notify(kMediaError, kMediaErrorUnsupported, 0);
      return false;
    }
    return true;
  }

  // --- Video: decoded on a thread of its own, each frame shown at its time on
  // the media clock (the audio's position; the wall clock without audio).

  int64_t WallClockLocked() {
    int64_t us = clock_base_us_;
    if (clock_running_) {
      us += static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - clock_anchor_).count() *
                                 speed_);
    }
    return us;
  }

  // With mu_ held: video restarts at `us` (a seek, a loop); `exact` drops the
  // frames before it (SEEK_CLOSEST), else the sync frame before it is shown.
  void VideoSeekLocked(int64_t us, bool exact) {
    video_seek_us_ = us;
    video_seek_exact_ = exact;
    video_generation_++;
    video_done_ = false;
    cv_.notify_all();
  }

  void Render(const VideoFrame& f) {
    ANativeWindow* window;
    {
      std::lock_guard<std::mutex> lock(mu_);
      window = window_;
      if (window != nullptr) ANativeWindow_acquire(window);
    }
    if (window == nullptr) return;
    RenderFrame(window, f);
    ANativeWindow_release(window);
  }

  void VideoRun() {
    std::deque<VideoFrame> frames;  // decoded, in presentation order
    Sample sample;
    bool have_sample = false, input_done = false;
    uint64_t generation = 0;
    int64_t drop_before = -1;
    bool first = true;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] {
          return quit_ || video_generation_ != generation || show_current_ ||
                 (!video_done_ && (first || (playing_ && !completed_)));
        });
        if (quit_) return;
        if (show_current_) {
          show_current_ = false;
          if (!last_frame_.data.empty() && video_generation_ == generation) {
            VideoFrame copy = last_frame_;
            lock.unlock();
            Render(copy);
            continue;
          }
        }
        if (video_generation_ != generation) {
          generation = video_generation_;
          int64_t us = video_seek_us_;
          bool exact = video_seek_exact_;
          lock.unlock();
          int status = video_reader_->Read(&sample, std::max<int64_t>(us, 0), SeekMode::kPreviousSync);
          have_sample = status == 0;
          input_done = status != 0;
          video_decoder_->Flush();
          frames.clear();
          drop_before = exact ? us : -1;
          first = true;
          continue;
        }
      }
      if (frames.empty()) {
        if (input_done) {
          std::vector<VideoFrame> out;
          video_decoder_->Drain(&out);
          for (VideoFrame& f : out) frames.push_back(std::move(f));
          if (frames.empty()) {
            std::lock_guard<std::mutex> lock(mu_);
            if (video_generation_ == generation) video_done_ = true;
            cv_.notify_all();
          }
          continue;
        }
        if (!have_sample) {
          int status = video_reader_->Read(&sample);
          have_sample = status == 0;
          input_done = status != 0;
          if (status < 0) MEDIA_LOGE("player: reading the video track failed (%d)", status);
          continue;
        }
        have_sample = false;
        std::vector<VideoFrame> out;
        if (!video_decoder_->Decode(sample, &out)) {
          MEDIA_LOGE("player: the video decoder failed at %.3f s", sample.time_us / 1e6);
          input_done = true;
          continue;
        }
        for (VideoFrame& f : out) frames.push_back(std::move(f));
        continue;
      }
      VideoFrame& f = frames.front();
      if (drop_before >= 0 && f.time_us + 1000 < drop_before) {
        frames.pop_front();
        continue;
      }
      if (first) {
        // The first frame after the start or a seek is shown at once (also when
        // paused, as on Android).
        first = false;
        drop_before = -1;
        Render(f);
        std::lock_guard<std::mutex> lock(mu_);
        last_frame_ = std::move(f);
        frames.pop_front();
        continue;
      }
      // Wait for the frame's time on the media clock.
      bool show = true, interrupted = false;
      for (;;) {
        std::unique_lock<std::mutex> lock(mu_);
        if (quit_ || video_generation_ != generation) {
          interrupted = true;
          break;
        }
        if (!playing_ || completed_) {
          cv_.wait_for(lock, std::chrono::milliseconds(20));
          continue;
        }
        // Past the audio's end (a longer video), frames follow one another at once.
        if (decoder_ != nullptr && track_ != nullptr && track_->stopped()) break;
        int64_t now = MediaUsLocked();
        int64_t ahead = f.time_us - now;
        if (ahead <= 2000) {
          // Too late to be worth showing (and more frames are waiting): dropped.
          show = ahead > -100000 || frames.size() == 1;
          break;
        }
        cv_.wait_for(lock, std::chrono::microseconds(std::min<int64_t>(ahead, 10000)));
      }
      if (interrupted) continue;
      if (show) Render(f);
      std::lock_guard<std::mutex> lock(mu_);
      if (show) {
        last_frame_ = std::move(f);
        video_shown_++;
      } else {
        video_dropped_++;
      }
      frames.pop_front();
    }
  }

  // A video without audio: the playback thread keeps the clock, the seeks and
  // the end (VideoRun draws).
  void VideoOnlyRun() {
    for (;;) {
      int64_t seek_us;
      int seek_mode;
      bool notify_seek;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] {
          return quit_ || seek_us_ >= 0 || (video_done_ && playing_ && !completed_);
        });
        if (quit_) return;
        seek_us = seek_us_;
        seek_mode = seek_mode_;
        notify_seek = seek_notify_;
        if (seek_us >= 0) {
          clock_base_us_ = seek_us;
          clock_anchor_ = std::chrono::steady_clock::now();
          VideoSeekLocked(seek_us, seek_mode == 3);
          if (seek_us_ == seek_us) seek_us_ = -1;
          if (seek_us_ < 0) seek_notify_ = false;
        }
      }
      if (seek_us >= 0) {
        if (notify_seek) Notify(kMediaSeekComplete, 0, 0);
        continue;
      }
      bool loop;
      {
        std::lock_guard<std::mutex> lock(mu_);
        loop = loop_;
        if (loop) {
          clock_base_us_ = 0;
          clock_anchor_ = std::chrono::steady_clock::now();
          VideoSeekLocked(0, false);
        } else {
          completed_ = true;
          playing_ = false;
          clock_base_us_ = duration_us_ > 0 ? duration_us_ : WallClockLocked();
          clock_running_ = false;
        }
      }
      if (!loop) Notify(kMediaPlaybackComplete, 0, 0);
    }
  }

  // The track for the decoder's output, created at the first start.
  bool EnsureTrack() {
    std::lock_guard<std::mutex> lock(mu_);
    if (track_ != nullptr) return true;
    PcmInfo info = decoder_->info();
    TrackParams p;
    p.sample_rate = info.rate;
    p.channels = info.channels;
    p.format = info.format;
    p.stream_type = stream_;
    p.capacity_frames = std::max<size_t>(info.rate / 4, 1024);
    p.session_id = session_;
    p.name = "MediaPlayer";
    std::string error;
    track_ = CreateTrack(p, &error);
    if (track_ == nullptr) {
      MEDIA_LOGE("player: %s", error.c_str());
      return false;
    }
    rate_ = info.rate;
    frame_size_ = BytesPerSample(info.format) * info.channels;
    track_->SetVolume(left_, right_);
    track_->SetSpeed(speed_);
    if (playing_) track_->Start();
    return true;
  }

  // Writes PCM to the track, waiting for room; false if a seek, a stop or the
  // end of the player interrupts.
  bool WritePcm(const std::vector<uint8_t>& pcm) {
    size_t done = 0;
    while (done < pcm.size()) {
      std::shared_ptr<Track> t;
      {
        std::unique_lock<std::mutex> lock(mu_);
        if (quit_ || seek_us_ >= 0) return false;
        t = track_;
      }
      ssize_t n = t->Write(pcm.data() + done, pcm.size() - done, false);
      if (n < 0) return false;
      done += static_cast<size_t>(n);
      written_ += n / static_cast<ssize_t>(frame_size_);
      if (done < pcm.size()) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(5));
      }
    }
    return true;
  }

  // Seeks the reader and the decoder; the track starts over.
  bool DoSeek(int64_t us, int mode, Sample* first, bool* have_first, int* status) {
    SeekMode m = mode == 3 ? SeekMode::kClosest : mode == 1 ? SeekMode::kNextSync
                                                 : mode == 2 ? SeekMode::kClosestSync
                                                             : SeekMode::kPreviousSync;
    *status = reader_->Read(first, us, m);
    *have_first = *status == 0;
    decoder_->Flush(us);
    std::shared_ptr<Track> t;
    bool playing;
    {
      std::lock_guard<std::mutex> lock(mu_);
      t = track_;
      playing = playing_;
    }
    if (t != nullptr) {
      t->Pause();
      t->Flush();
      if (playing) t->Start();
    }
    std::lock_guard<std::mutex> lock(mu_);
    written_ = 0;
    segments_.clear();
    int64_t start = *have_first ? first->time_us : us;
    // A seek "to the closest" frame plays from the target; an earlier sync
    // frame's time is the position.
    segments_.push_back({0, mode == 3 ? us : start});
    base_us_ = segments_.back().start_us;
    if (video_decoder_ != nullptr) VideoSeekLocked(us, mode == 3);
    return true;
  }

  void Run() {
    RaiseAudioThreadPriority(false);
    if (!Open()) return;
    int vw, vh;
    {
      std::lock_guard<std::mutex> lock(mu_);
      segments_.push_back({0, 0});
      vw = video_width_;
      vh = video_height_;
    }
    if (video_decoder_ != nullptr) {
      Notify(kMediaSetVideoSize, vw, vh);
      auto self = shared_from_this();
      video_thread_ = std::thread([self] { self->VideoRun(); });
    }
    Notify(kMediaPrepared, 0, 0);
    if (source_->IsNetwork()) Notify(kMediaBufferingUpdate, 100, 0);
    if (decoder_ == nullptr) {
      VideoOnlyRun();
      return;
    }

    Sample sample;
    bool have_sample = false;
    bool eos = false;
    for (;;) {
      int64_t seek_us;
      int seek_mode;
      bool notify_seek;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return quit_ || seek_us_ >= 0 || (playing_ && !completed_); });
        if (quit_) return;
        seek_us = seek_us_;
        seek_mode = seek_mode_;
        notify_seek = seek_notify_;
      }
      if (seek_us >= 0) {
        int status = 0;
        DoSeek(seek_us, seek_mode, &sample, &have_sample, &status);
        eos = status != 0;
        {
          std::lock_guard<std::mutex> lock(mu_);
          // A seek posted meanwhile runs next.
          if (seek_us_ == seek_us) seek_us_ = -1;
          if (seek_us_ < 0) seek_notify_ = false;
        }
        if (notify_seek) Notify(kMediaSeekComplete, 0, 0);
        continue;
      }
      if (!EnsureTrack()) {
        Notify(kMediaError, kMediaErrorUnsupported, 0);
        return;
      }
      if (!eos && !have_sample) {
        int status = reader_->Read(&sample);
        if (status < 0) {
          Notify(kMediaError, kMediaErrorMalformed, 0);
          return;
        }
        have_sample = status == 0;
        eos = status != 0;
      }
      std::vector<uint8_t> pcm;
      if (have_sample) {
        have_sample = false;
        if (!decoder_->Decode(sample, &pcm)) {
          Notify(kMediaError, kMediaErrorMalformed, 0);
          return;
        }
        if (!WritePcm(pcm)) continue;
        continue;
      }
      // The end.
      decoder_->Drain(&pcm);
      if (!WritePcm(pcm)) continue;
      bool loop;
      {
        std::lock_guard<std::mutex> lock(mu_);
        loop = loop_;
      }
      if (loop) {
        // Around again, without stopping the track: the position restarts at
        // 0 where these frames start.
        int status = reader_->Read(&sample, 0, SeekMode::kPreviousSync);
        have_sample = status == 0;
        eos = status != 0;
        decoder_->Flush(0);
        std::lock_guard<std::mutex> lock(mu_);
        segments_.push_back({written_, 0});
        while (segments_.size() > 8) segments_.pop_front();
        if (video_decoder_ != nullptr) VideoSeekLocked(0, false);
        continue;
      }
      std::shared_ptr<Track> t;
      {
        std::lock_guard<std::mutex> lock(mu_);
        t = track_;
      }
      t->Stop();
      bool interrupted = false;
      for (;;) {
        std::unique_lock<std::mutex> lock(mu_);
        if (t->stopped() && (video_decoder_ == nullptr || video_done_)) break;
        if (quit_ || seek_us_ >= 0 || loop_) {
          interrupted = true;
          break;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(10));
      }
      if (interrupted) {
        // setLooping(true) while draining, or a seek: go on reading.
        bool looping;
        {
          std::lock_guard<std::mutex> lock(mu_);
          looping = loop_ && seek_us_ < 0 && !quit_;
          if (looping) seek_us_ = 0, seek_mode_ = 0, seek_notify_ = false;
        }
        t->Start();
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        completed_ = true;
        playing_ = false;
      }
      Notify(kMediaPlaybackComplete, 0, 0);
    }
  }

  Player* owner_;
  std::shared_ptr<DataSource> source_;
  std::thread thread_;

  std::mutex mu_;
  std::condition_variable cv_;
  bool quit_ = false;
  bool playing_ = false;
  bool completed_ = false;
  bool loop_ = false;
  int64_t seek_us_ = -1;
  int seek_mode_ = 0;
  bool seek_notify_ = false;
  int stream_ = 3;
  int session_ = 0;
  float left_ = 1.0f, right_ = 1.0f, speed_ = 1.0f;

  std::unique_ptr<Extractor> extractor_;
  std::unique_ptr<TrackReader> reader_;
  std::unique_ptr<AudioDecoder> decoder_;
  std::shared_ptr<Track> track_;
  // Video.
  std::thread video_thread_;
  std::unique_ptr<TrackReader> video_reader_;
  std::unique_ptr<VideoDecoder> video_decoder_;
  ANativeWindow* window_ = nullptr;
  int video_width_ = 0, video_height_ = 0;
  uint64_t video_generation_ = 0;
  int64_t video_seek_us_ = 0;
  bool video_seek_exact_ = false;
  bool video_done_ = false;
  bool show_current_ = false;
  VideoFrame last_frame_;
  uint64_t video_shown_ = 0, video_dropped_ = 0;
  // The wall clock of a video without audio.
  int64_t clock_base_us_ = 0;
  std::chrono::steady_clock::time_point clock_anchor_;
  bool clock_running_ = false;
  uint32_t rate_ = 0;
  size_t frame_size_ = 1;
  int64_t written_ = 0;
  std::deque<Segment> segments_;
  int64_t base_us_ = 0;
  int64_t last_us_ = 0;
  int64_t duration_us_ = -1;
};

// --- Player: libmedia MediaPlayer's states ------------------------------------------------

Player::Player() : state_(kIdle) {}

Player::~Player() { Release(); }

void Player::SetListener(Listener l) {
  std::lock_guard<std::mutex> lock(mu_);
  listener_ = std::move(l);
}

int Player::SetDataSource(std::shared_ptr<DataSource> source) {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ != kIdle) return kPlayerInvalidOperation;
  source_ = std::move(source);
  state_ = kInitialized;
  return kPlayerOk;
}

int Player::PrepareAsync() {
  std::lock_guard<std::mutex> lock(mu_);
  if ((state_ & (kInitialized | kStopped)) == 0 || source_ == nullptr || playback_ != nullptr) {
    return kPlayerInvalidOperation;
  }
  playback_ = std::make_shared<Playback>(this, source_);
  playback_->SetStream(stream_);
  playback_->SetSession(session_id_);
  playback_->SetLooping(loop_);
  playback_->SetVolume(left_, right_);
  playback_->SetSpeed(speed_);
  if (window_ != nullptr) playback_->SetWindow(window_);
  state_ = kPreparing;
  playback_->Begin();
  return kPlayerOk;
}

int Player::Prepare() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (prepare_sync_) return -114;  // -EALREADY
    prepare_sync_ = true;
  }
  int err = PrepareAsync();
  std::unique_lock<std::mutex> lock(mu_);
  if (err != kPlayerOk) {
    prepare_sync_ = false;
    return err;
  }
  prepared_cv_.wait(lock, [this] { return !prepare_sync_; });
  return prepare_status_;
}

int Player::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ & kStarted) return kPlayerOk;
  if (playback_ != nullptr && (state_ & (kPrepared | kPlaybackComplete | kPaused))) {
    playback_->SetLooping(loop_);
    playback_->SetVolume(left_, right_);
    state_ = kStarted;
    playback_->Start();
    return kPlayerOk;
  }
  MEDIA_LOGE("MediaPlayer: start in state %u", state_);
  return kPlayerInvalidOperation;
}

int Player::Stop() {
  std::shared_ptr<Playback> pb;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ & kStopped) return kPlayerOk;
    if (playback_ == nullptr || (state_ & (kStarted | kPrepared | kPaused | kPlaybackComplete)) == 0) {
      MEDIA_LOGE("MediaPlayer: stop in state %u", state_);
      return kPlayerInvalidOperation;
    }
    // Ended outside the lock: its thread may be waiting for it to notify.
    pb = std::move(playback_);
    state_ = kStopped;
    current_position_ = seek_position_ = -1;
  }
  pb->Shutdown();
  return kPlayerOk;
}

int Player::Pause() {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ & (kPaused | kPlaybackComplete)) return kPlayerOk;
  if (playback_ != nullptr && (state_ & kStarted)) {
    playback_->Pause();
    state_ = kPaused;
    return kPlayerOk;
  }
  MEDIA_LOGE("MediaPlayer: pause in state %u", state_);
  return kPlayerInvalidOperation;
}

bool Player::IsPlaying() {
  std::lock_guard<std::mutex> lock(mu_);
  if (playback_ == nullptr) return false;
  return (state_ & kStarted) && playback_->IsPlaying();
}

int Player::SeekTo(int msec, int mode) {
  std::lock_guard<std::mutex> lock(mu_);
  if (playback_ == nullptr || (state_ & (kStarted | kPrepared | kPaused | kPlaybackComplete)) == 0) {
    MEDIA_LOGE("MediaPlayer: seekTo in state %u", state_);
    return kPlayerInvalidOperation;
  }
  int64_t duration = playback_->DurationUs();
  if (duration < 0) return kPlayerInvalidOperation;  // not seekable
  msec = std::clamp<int64_t>(msec, 0, duration / 1000);
  current_position_ = msec;
  current_seek_mode_ = mode;
  if (seek_position_ < 0) {
    seek_position_ = msec;
    seek_mode_ = mode;
    playback_->Seek(static_cast<int64_t>(msec) * 1000, mode);
  }
  if (state_ & kPlaybackComplete) state_ = kPaused;
  return kPlayerOk;
}

int Player::GetCurrentPosition(int* msec) {
  std::lock_guard<std::mutex> lock(mu_);
  if (playback_ == nullptr) {
    *msec = 0;
    return (state_ & (kStopped | kInitialized)) ? kPlayerOk : kPlayerInvalidOperation;
  }
  if (current_position_ >= 0) {
    *msec = current_position_;
    return kPlayerOk;
  }
  *msec = static_cast<int>(playback_->PositionUs() / 1000);
  return kPlayerOk;
}

int Player::GetDuration(int* msec) {
  std::lock_guard<std::mutex> lock(mu_);
  if (playback_ != nullptr &&
      (state_ & (kPrepared | kStarted | kPaused | kStopped | kPlaybackComplete))) {
    int64_t d = playback_->DurationUs();
    *msec = d >= 0 ? static_cast<int>(d / 1000) : -1;
    return kPlayerOk;
  }
  *msec = 0;
  return kPlayerInvalidOperation;
}

int Player::Reset() {
  std::shared_ptr<Playback> pb;
  {
    std::lock_guard<std::mutex> lock(mu_);
    loop_ = false;
    prepare_sync_ = false;
    pb = std::move(playback_);
    source_.reset();
    state_ = kIdle;
    current_position_ = seek_position_ = -1;
  }
  prepared_cv_.notify_all();
  if (pb != nullptr) pb->Shutdown();
  return kPlayerOk;
}

void Player::Release() {
  Reset();
  ANativeWindow* window;
  {
    std::lock_guard<std::mutex> lock(mu_);
    listener_ = nullptr;
    window = window_;
    window_ = nullptr;
  }
  if (window != nullptr) ANativeWindow_release(window);
}

int Player::SetLooping(bool loop) {
  std::lock_guard<std::mutex> lock(mu_);
  loop_ = loop;
  if (playback_ != nullptr) playback_->SetLooping(loop);
  return kPlayerOk;
}

bool Player::IsLooping() {
  std::lock_guard<std::mutex> lock(mu_);
  return loop_;
}

int Player::SetVolume(float left, float right) {
  std::lock_guard<std::mutex> lock(mu_);
  left_ = left;
  right_ = right;
  if (playback_ != nullptr) playback_->SetVolume(left, right);
  return kPlayerOk;
}

int Player::SetAudioStreamType(int stream) {
  std::lock_guard<std::mutex> lock(mu_);
  if (stream_ == stream) return kPlayerOk;
  if (state_ & (kPrepared | kStarted | kPaused | kPlaybackComplete)) return kPlayerInvalidOperation;
  stream_ = stream;
  return kPlayerOk;
}

int Player::GetAudioStreamType(int* stream) {
  std::lock_guard<std::mutex> lock(mu_);
  *stream = stream_;
  return kPlayerOk;
}

int Player::SetSpeed(float speed) {
  std::lock_guard<std::mutex> lock(mu_);
  speed_ = speed;
  if (playback_ != nullptr) playback_->SetSpeed(speed);
  return kPlayerOk;
}

float Player::speed() {
  std::lock_guard<std::mutex> lock(mu_);
  return speed_;
}

void Player::ReportError(int status) { Notify(kMediaError, status, 0); }

int Player::SetVideoWindow(ANativeWindow* window) {
  std::shared_ptr<Playback> pb;
  ANativeWindow* old;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (window != nullptr) ANativeWindow_acquire(window);
    old = window_;
    window_ = window;
    pb = playback_;
  }
  if (pb != nullptr) pb->SetWindow(window);
  if (old != nullptr) ANativeWindow_release(old);
  return kPlayerOk;
}

void Player::VideoSize(int* width, int* height) {
  std::lock_guard<std::mutex> lock(mu_);
  *width = video_width_;
  *height = video_height_;
}

void Player::Notify(int what, int arg1, int arg2) {
  bool send = true;
  Listener listener;
  {
    std::lock_guard<std::mutex> lock(mu_);
    // As libmedia: nothing reaches a player with no data source but errors.
    if (state_ == kIdle && what != kMediaError) return;
    switch (what) {
      case kMediaPrepared:
        state_ = kPrepared;
        if (prepare_sync_) {
          prepare_sync_ = false;
          prepare_status_ = kPlayerOk;
          prepared_cv_.notify_all();
        }
        break;
      case kMediaPlaybackComplete:
        if (!loop_) state_ = kPlaybackComplete;
        break;
      case kMediaSetVideoSize:
        video_width_ = arg1;
        video_height_ = arg2;
        break;
      case kMediaError:
        MEDIA_LOGE("MediaPlayer: error (%d, %d)", arg1, arg2);
        state_ = kStateError;
        if (prepare_sync_) {
          prepare_sync_ = false;
          prepare_status_ = arg1;
          prepared_cv_.notify_all();
          send = false;
        }
        break;
      case kMediaSeekComplete:
        if (seek_position_ != current_position_ || seek_mode_ != current_seek_mode_) {
          // A seek came while this one ran: do it now.
          seek_position_ = current_position_;
          seek_mode_ = current_seek_mode_;
          if (playback_ != nullptr) playback_->Seek(static_cast<int64_t>(seek_position_) * 1000, seek_mode_);
          send = false;
        } else {
          current_position_ = seek_position_ = -1;
        }
        break;
      default:
        break;
    }
    listener = listener_;
  }
  if (send && listener) {
    PostEvent([listener, what, arg1, arg2] { listener(what, arg1, arg2); });
  }
}

}  // namespace gesher::media
