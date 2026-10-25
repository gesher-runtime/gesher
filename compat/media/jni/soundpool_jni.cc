// Gesher: android.media.SoundPool's natives (docs/media.md), in the builtin
// libsoundpool.so (media_jni.cc).
//
// As frameworks/base media/jni/soundpool: a sound is decoded whole to PCM on a
// loader thread (then SAMPLE_LOADED goes to the app); each play is a static
// engine track over that PCM (its loop count, rate, volume), up to
// maxStreams at once, the lowest priority (then the oldest) stream giving way
// to a new one.

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../audio/engine.h"
#include "../audio/log.h"
#include "../player/data_source.h"
#include "../player/decoder.h"
#include "jni_util.h"
#include "media_jni.h"

namespace gesher::media {
namespace {

constexpr int kSampleLoaded = 1;
constexpr size_t kMaxSampleBytes = 64u << 20;  // 64 MB of PCM per sound

jfieldID gContext;    // long mNativeContext
jmethodID gPostEvent;  // void postEventFromNative(int, int, int, Object)

struct Sound {
  std::vector<uint8_t> pcm;
  PcmInfo info;
  bool ready = false;
};

struct Stream {
  int id;
  int sound;
  int priority;
  std::shared_ptr<Track> track;
  std::shared_ptr<std::atomic<bool>> ended;  // its sound played to the end
  float left, right;
  bool paused = false;
  bool auto_paused = false;
};

class Pool {
 public:
  Pool(JNIEnv* env, jobject thiz, int max_streams, int stream_type)
      : self_(std::make_shared<WeakRef>(env, thiz)),
        max_streams_(std::max(max_streams, 1)),
        stream_type_(stream_type) {
    loader_ = std::thread([this] { LoadLoop(); });
  }

  ~Pool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      quit_ = true;
    }
    cv_.notify_all();
    loader_.join();
    for (auto& s : streams_) s.track->Release();
  }

  int Load(std::unique_ptr<DataSource> source) {
    std::lock_guard<std::mutex> lock(mu_);
    int id = next_sound_++;
    sounds_[id] = std::make_shared<Sound>();
    jobs_.push_back({id, std::shared_ptr<DataSource>(std::move(source))});
    cv_.notify_all();
    return id;
  }

  bool Unload(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    return sounds_.erase(id) > 0;
  }

  int Play(int sound_id, float left, float right, int priority, int loop, float rate) {
    std::lock_guard<std::mutex> lock(mu_);
    Reap();
    auto it = sounds_.find(sound_id);
    if (it == sounds_.end() || !it->second->ready) return 0;
    const Sound& sound = *it->second;
    if (static_cast<int>(streams_.size()) >= max_streams_) {
      // The lowest priority, oldest first, gives way; none lower: no play.
      auto victim = streams_.end();
      for (auto s = streams_.begin(); s != streams_.end(); ++s) {
        if (victim == streams_.end() || s->priority < victim->priority) victim = s;
      }
      if (victim == streams_.end() || victim->priority > priority) return 0;
      victim->track->Release();
      streams_.erase(victim);
    }
    size_t frame = BytesPerSample(sound.info.format) * sound.info.channels;
    TrackParams p;
    p.sample_rate = sound.info.rate;
    p.channels = sound.info.channels;
    p.format = sound.info.format;
    p.stream_type = stream_type_;
    p.is_static = true;
    p.capacity_frames = std::max<size_t>(sound.pcm.size() / frame, 1);
    p.name = "SoundPool";
    std::string error;
    auto track = CreateTrack(p, &error);
    if (track == nullptr) {
      MEDIA_LOGE("SoundPool: %s", error.c_str());
      return 0;
    }
    track->Write(sound.pcm.data(), sound.pcm.size(), false);
    auto ended = std::make_shared<std::atomic<bool>>(false);
    TrackEvents ev;
    ev.end = [ended] { ended->store(true); };
    track->SetEvents(std::move(ev));
    if (loop != 0) track->SetLoop(0, static_cast<uint32_t>(p.capacity_frames), loop);
    track->SetSampleRate(static_cast<uint32_t>(sound.info.rate * std::clamp(rate, 0.5f, 2.0f)));
    float mute = muted_ ? 0.0f : 1.0f;
    track->SetVolume(left * mute, right * mute);
    track->Start();
    int id = next_stream_++;
    streams_.push_back({id, sound_id, priority, track, ended, left, right});
    return id;
  }

  template <typename F>
  void With(int stream_id, F f) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& s : streams_) {
      if (s.id == stream_id) f(s);
    }
  }

  void Stop(int stream_id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto s = streams_.begin(); s != streams_.end(); ++s) {
      if (s->id == stream_id) {
        s->track->Release();
        streams_.erase(s);
        return;
      }
    }
  }

  void AutoPause(bool pause) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& s : streams_) {
      if (pause && !s.paused) {
        s.track->Pause();
        s.auto_paused = true;
      } else if (!pause && s.auto_paused) {
        s.track->Start();
        s.auto_paused = false;
      }
    }
  }

  void Mute(bool mute) {
    std::lock_guard<std::mutex> lock(mu_);
    muted_ = mute;
    for (auto& s : streams_) s.track->SetVolume(mute ? 0 : s.left, mute ? 0 : s.right);
  }

  bool muted() const { return muted_; }

 private:
  struct Job {
    int id;
    std::shared_ptr<DataSource> source;
  };

  // Streams whose sound ended are gone; mu_ held.
  void Reap() {
    for (auto s = streams_.begin(); s != streams_.end();) {
      if (s->ended->load()) {
        s->track->Release();
        s = streams_.erase(s);
      } else {
        ++s;
      }
    }
  }

  void LoadLoop() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
        if (quit_) return;
        job = jobs_.front();
        jobs_.pop_front();
      }
      auto sound = std::make_shared<Sound>();
      std::string error;
      bool ok = DecodeFile(job.source, kMaxSampleBytes, &sound->pcm, &sound->info, &error);
      if (!ok) MEDIA_LOGE("SoundPool: sound %d: %s", job.id, error.c_str());
      sound->ready = ok;
      {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sounds_.find(job.id);
        if (it == sounds_.end()) continue;  // unloaded meanwhile
        it->second = sound;
      }
      auto self = self_;
      int id = job.id, status = ok ? 0 : 1;
      PostEvent([self, id, status] {
        JNIEnv* env = AttachedEnv("gesher-soundpool");
        if (env == nullptr) return;
        jobject pool = env->NewLocalRef(self->get());
        if (pool == nullptr) return;  // collected
        env->CallVoidMethod(pool, gPostEvent, kSampleLoaded, id, status, nullptr);
        CheckException(env, "SoundPool.postEventFromNative");
        env->DeleteLocalRef(pool);
      });
    }
  }

  std::shared_ptr<WeakRef> self_;  // the SoundPool, not kept alive by its natives
  int max_streams_;
  int stream_type_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool quit_ = false;
  bool muted_ = false;
  std::map<int, std::shared_ptr<Sound>> sounds_;
  std::deque<Job> jobs_;
  std::vector<Stream> streams_;
  int next_sound_ = 1;
  int next_stream_ = 1;
  std::thread loader_;
};

Pool* GetPool(JNIEnv* env, jobject thiz) {
  return reinterpret_cast<Pool*>(env->GetLongField(thiz, gContext));
}

jint JNICALL SP_setup(JNIEnv* env, jobject thiz, jint maxStreams, jobject attributes, jstring) {
  EnsureAppName(env);
  auto* pool = new Pool(env, thiz, maxStreams, LegacyStreamType(env, attributes));
  env->SetLongField(thiz, gContext, reinterpret_cast<jlong>(pool));
  return 0;
}

void JNICALL SP_release(JNIEnv* env, jobject thiz) {
  Pool* pool = GetPool(env, thiz);
  env->SetLongField(thiz, gContext, 0);
  delete pool;
}

jint JNICALL SP_load(JNIEnv* env, jobject thiz, jobject fileDescriptor, jlong offset, jlong length,
                     jint) {
  Pool* pool = GetPool(env, thiz);
  if (pool == nullptr || fileDescriptor == nullptr) return 0;
  jclass fdc = env->FindClass("java/io/FileDescriptor");
  jint fd = env->GetIntField(fileDescriptor, env->GetFieldID(fdc, "descriptor", "I"));
  std::string error;
  auto source = OpenFdSource(fd, offset, length, &error);
  if (source == nullptr) {
    MEDIA_LOGE("SoundPool.load: %s", error.c_str());
    return 0;
  }
  return pool->Load(std::move(source));
}

jboolean JNICALL SP_unload(JNIEnv* env, jobject thiz, jint id) {
  Pool* pool = GetPool(env, thiz);
  return pool != nullptr && pool->Unload(id) ? JNI_TRUE : JNI_FALSE;
}

jint JNICALL SP_play(JNIEnv* env, jobject thiz, jint sound, jfloat left, jfloat right, jint priority,
                     jint loop, jfloat rate, jint) {
  Pool* pool = GetPool(env, thiz);
  return pool != nullptr ? pool->Play(sound, left, right, priority, loop, rate) : 0;
}

void JNICALL SP_pause(JNIEnv* env, jobject thiz, jint id) {
  if (Pool* pool = GetPool(env, thiz)) {
    pool->With(id, [](Stream& s) {
      s.track->Pause();
      s.paused = true;
    });
  }
}

void JNICALL SP_resume(JNIEnv* env, jobject thiz, jint id) {
  if (Pool* pool = GetPool(env, thiz)) {
    pool->With(id, [](Stream& s) {
      s.track->Start();
      s.paused = false;
    });
  }
}

void JNICALL SP_autoPause(JNIEnv* env, jobject thiz) {
  if (Pool* pool = GetPool(env, thiz)) pool->AutoPause(true);
}
void JNICALL SP_autoResume(JNIEnv* env, jobject thiz) {
  if (Pool* pool = GetPool(env, thiz)) pool->AutoPause(false);
}

void JNICALL SP_stop(JNIEnv* env, jobject thiz, jint id) {
  if (Pool* pool = GetPool(env, thiz)) pool->Stop(id);
}

void JNICALL SP_setVolume(JNIEnv* env, jobject thiz, jint id, jfloat left, jfloat right) {
  if (Pool* pool = GetPool(env, thiz)) {
    bool muted = pool->muted();
    pool->With(id, [=](Stream& s) {
      s.left = left;
      s.right = right;
      if (!muted) s.track->SetVolume(left, right);
    });
  }
}

void JNICALL SP_mute(JNIEnv* env, jobject thiz, jboolean mute) {
  if (Pool* pool = GetPool(env, thiz)) pool->Mute(mute);
}

void JNICALL SP_setPriority(JNIEnv* env, jobject thiz, jint id, jint priority) {
  if (Pool* pool = GetPool(env, thiz)) pool->With(id, [=](Stream& s) { s.priority = priority; });
}

void JNICALL SP_setLoop(JNIEnv* env, jobject thiz, jint id, jint loop) {
  if (Pool* pool = GetPool(env, thiz)) {
    pool->With(id, [=](Stream& s) {
      s.track->SetLoop(0, static_cast<uint32_t>(s.track->static_frames()), loop);
    });
  }
}

void JNICALL SP_setRate(JNIEnv* env, jobject thiz, jint id, jfloat rate) {
  if (Pool* pool = GetPool(env, thiz)) {
    pool->With(id, [=](Stream& s) {
      s.track->SetSampleRate(
          static_cast<uint32_t>(s.track->params().sample_rate * std::clamp(rate, 0.5f, 2.0f)));
    });
  }
}

const JNINativeMethod kMethods[] = {
    {"native_setup", "(ILjava/lang/Object;Ljava/lang/String;)I", GESHER_NATIVE(SP_setup)},
    {"native_release", "()V", GESHER_NATIVE(SP_release)},
    {"_load", "(Ljava/io/FileDescriptor;JJI)I", GESHER_NATIVE(SP_load)},
    {"unload", "(I)Z", GESHER_NATIVE(SP_unload)},
    {"_play", "(IFFIIFI)I", GESHER_NATIVE(SP_play)},
    {"pause", "(I)V", GESHER_NATIVE(SP_pause)},
    {"resume", "(I)V", GESHER_NATIVE(SP_resume)},
    {"autoPause", "()V", GESHER_NATIVE(SP_autoPause)},
    {"autoResume", "()V", GESHER_NATIVE(SP_autoResume)},
    {"stop", "(I)V", GESHER_NATIVE(SP_stop)},
    {"_setVolume", "(IFF)V", GESHER_NATIVE(SP_setVolume)},
    {"_mute", "(Z)V", GESHER_NATIVE(SP_mute)},
    {"setPriority", "(II)V", GESHER_NATIVE(SP_setPriority)},
    {"setLoop", "(II)V", GESHER_NATIVE(SP_setLoop)},
    {"setRate", "(IF)V", GESHER_NATIVE(SP_setRate)},
};

}  // namespace

int RegisterSoundPool(JNIEnv* env) {
  const char* name = "android/media/SoundPool";
  if (!RegisterStandins(env, name)) return 0;
  jclass cls = env->FindClass(name);
  gContext = env->GetFieldID(cls, "mNativeContext", "J");
  gPostEvent = env->GetMethodID(cls, "postEventFromNative", "(IIILjava/lang/Object;)V");
  env->DeleteLocalRef(cls);
  if (gContext == nullptr || gPostEvent == nullptr) {
    env->ExceptionClear();
    MEDIA_LOGE("SoundPool's fields are not as expected; its natives stay stand-ins");
    return 0;
  }
  RegisterMethods(env, name, kMethods);
  return 0;
}

}  // namespace gesher::media
