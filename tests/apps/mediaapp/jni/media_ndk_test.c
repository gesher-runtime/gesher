// mediaapp's NDK audio (docs/media.md): AAudio (a data callback stream and a
// blocking-write stream) and OpenSL ES (a buffer queue player, a player of an
// asset by file descriptor), called from MediaActivity through JNI. Each plays
// a tone the test finds in the audio capture.
#include <aaudio/AAudio.h>
#include <jni.h>
#include <math.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// --- AAudio ------------------------------------------------------------------

struct Tone {
  double phase, step;
  int64_t frames;
};

static aaudio_data_callback_result_t on_data(AAudioStream* s, void* user, void* data, int32_t n) {
  struct Tone* t = (struct Tone*)user;
  float* out = (float*)data;
  int32_t ch = AAudioStream_getChannelCount(s);
  for (int32_t i = 0; i < n; i++) {
    float v = (float)(0.5 * sin(t->phase));
    t->phase += t->step;
    for (int32_t c = 0; c < ch; c++) out[i * ch + c] = v;
  }
  t->frames += n;
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

JNIEXPORT jstring JNICALL Java_com_gesher_mediaapp_MediaActivity_aaudioCallbackTone(
    JNIEnv* env, jclass cls, jint hz, jint ms) {
  AAudioStreamBuilder* b;
  AAudioStream* s;
  struct Tone t = {0, 0, 0};
  char msg[256];
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return (*env)->NewStringUTF(env, "no builder");
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(b, 2);
  AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setDataCallback(b, on_data, &t);
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) {
    snprintf(msg, sizeof msg, "open %s", AAudio_convertResultToText(r));
    return (*env)->NewStringUTF(env, msg);
  }
  int32_t rate = AAudioStream_getSampleRate(s);
  t.step = 2 * M_PI * hz / rate;
  AAudioStream_requestStart(s);
  aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
  AAudioStream_waitForStateChange(s, AAUDIO_STREAM_STATE_STARTING, &next, 100000000);
  sleep_ms(ms);
  AAudioStream_requestStop(s);
  int64_t frames = t.frames;
  snprintf(msg, sizeof msg, "rate %d, channels %d, float %d, state %s, burst %s, frames %s",
           rate, AAudioStream_getChannelCount(s),
           AAudioStream_getFormat(s) == AAUDIO_FORMAT_PCM_FLOAT,
           AAudio_convertStreamStateToText(AAudioStream_getState(s)),
           AAudioStream_getFramesPerBurst(s) > 0 ? "ok" : "bad",
           frames > rate * ms / 1000 * 8 / 10 ? "ok" : "too few");
  AAudioStream_close(s);
  return (*env)->NewStringUTF(env, msg);
}

JNIEXPORT jstring JNICALL Java_com_gesher_mediaapp_MediaActivity_aaudioWriteTone(
    JNIEnv* env, jclass cls, jint hz, jint ms) {
  AAudioStreamBuilder* b;
  AAudioStream* s;
  char msg[256];
  AAudio_createStreamBuilder(&b);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setChannelCount(b, 1);
  AAudioStreamBuilder_setSampleRate(b, 22050);
  AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) {
    snprintf(msg, sizeof msg, "open %s", AAudio_convertResultToText(r));
    return (*env)->NewStringUTF(env, msg);
  }
  int32_t frames = 22050 * ms / 1000;
  static int16_t pcm[22050 * 4];
  for (int32_t i = 0; i < frames; i++) pcm[i] = (int16_t)(16383 * sin(2 * M_PI * hz * i / 22050.0));
  AAudioStream_requestStart(s);
  int32_t done = 0;
  while (done < frames) {
    int32_t n = AAudioStream_write(s, pcm + done, frames - done, 1000000000LL);
    if (n <= 0) break;
    done += n;
  }
  // Let it play out.
  while (AAudioStream_getFramesRead(s) < frames) sleep_ms(10);
  AAudioStream_requestStop(s);
  snprintf(msg, sizeof msg, "rate %d, written %lld, read %s", AAudioStream_getSampleRate(s),
           (long long)AAudioStream_getFramesWritten(s),
           AAudioStream_getFramesRead(s) >= frames ? "all" : "short");
  AAudioStream_close(s);
  return (*env)->NewStringUTF(env, msg);
}

JNIEXPORT jstring JNICALL Java_com_gesher_mediaapp_MediaActivity_aaudioInput(JNIEnv* env, jclass cls) {
  AAudioStreamBuilder* b;
  AAudioStream* s = NULL;
  AAudio_createStreamBuilder(&b);
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  AAudioStreamBuilder_delete(b);
  if (r == AAUDIO_OK) AAudioStream_close(s);
  return (*env)->NewStringUTF(env, AAudio_convertResultToText(r));
}

// --- OpenSL ES -----------------------------------------------------------------

static SLObjectItf g_engine_obj, g_mix_obj;
static SLEngineItf g_engine;

static int ensure_engine(void) {
  if (g_engine != NULL) return 1;
  if (slCreateEngine(&g_engine_obj, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS) return 0;
  (*g_engine_obj)->Realize(g_engine_obj, SL_BOOLEAN_FALSE);
  (*g_engine_obj)->GetInterface(g_engine_obj, SL_IID_ENGINE, &g_engine);
  (*g_engine)->CreateOutputMix(g_engine, &g_mix_obj, 0, NULL, NULL);
  (*g_mix_obj)->Realize(g_mix_obj, SL_BOOLEAN_FALSE);
  return 1;
}

#define QUEUE_FRAMES 2205  /* 50 ms at 44100 Hz */
static int16_t g_bufs[2][QUEUE_FRAMES];
static int g_next, g_left;
static double g_phase, g_step;
static volatile int g_callbacks;

static void fill(int16_t* buf) {
  for (int i = 0; i < QUEUE_FRAMES; i++) {
    buf[i] = (int16_t)(16383 * sin(g_phase));
    g_phase += g_step;
  }
}

static void on_buffer(SLAndroidSimpleBufferQueueItf q, void* ctx) {
  g_callbacks++;
  if (g_left <= 0) return;
  g_left--;
  fill(g_bufs[g_next]);
  (*q)->Enqueue(q, g_bufs[g_next], sizeof g_bufs[0]);
  g_next ^= 1;
}

JNIEXPORT jstring JNICALL Java_com_gesher_mediaapp_MediaActivity_openslQueueTone(
    JNIEnv* env, jclass cls, jint hz, jint ms) {
  char msg[256];
  if (!ensure_engine()) return (*env)->NewStringUTF(env, "no engine");
  SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
  SLDataFormat_PCM fmt = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource src = {&loc, &fmt};
  SLDataLocator_OutputMix out = {SL_DATALOCATOR_OUTPUTMIX, g_mix_obj};
  SLDataSink sink = {&out, NULL};
  const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
  const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  SLObjectItf player;
  SLresult r = (*g_engine)->CreateAudioPlayer(g_engine, &player, &src, &sink, 2, ids, req);
  if (r != SL_RESULT_SUCCESS) {
    snprintf(msg, sizeof msg, "CreateAudioPlayer %u", (unsigned)r);
    return (*env)->NewStringUTF(env, msg);
  }
  (*player)->Realize(player, SL_BOOLEAN_FALSE);
  SLPlayItf play;
  SLAndroidSimpleBufferQueueItf queue;
  SLVolumeItf volume;
  (*player)->GetInterface(player, SL_IID_PLAY, &play);
  (*player)->GetInterface(player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue);
  (*player)->GetInterface(player, SL_IID_VOLUME, &volume);
  (*volume)->SetVolumeLevel(volume, -602);  /* -6.02 dB: half the amplitude */
  g_step = 2 * M_PI * hz / 44100.0;
  g_phase = 0;
  g_callbacks = 0;
  g_left = ms / 50 - 2;
  g_next = 0;
  (*queue)->RegisterCallback(queue, on_buffer, NULL);
  fill(g_bufs[0]);
  (*queue)->Enqueue(queue, g_bufs[0], sizeof g_bufs[0]);
  fill(g_bufs[1]);
  (*queue)->Enqueue(queue, g_bufs[1], sizeof g_bufs[1]);
  (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
  sleep_ms(ms + 300);
  SLAndroidSimpleBufferQueueState st;
  (*queue)->GetState(queue, &st);
  SLuint32 state;
  (*play)->GetPlayState(play, &state);
  (*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED);
  snprintf(msg, sizeof msg, "playing %d, buffers left %u, taken %u, callbacks %s",
           state == SL_PLAYSTATE_PLAYING, (unsigned)st.count, (unsigned)st.index,
           g_callbacks == ms / 50 ? "all" : "not all");
  (*player)->Destroy(player);
  return (*env)->NewStringUTF(env, msg);
}

static volatile int g_at_end;
static void on_play(SLPlayItf play, void* ctx, SLuint32 event) {
  if (event & SL_PLAYEVENT_HEADATEND) g_at_end = 1;
}

JNIEXPORT jstring JNICALL Java_com_gesher_mediaapp_MediaActivity_openslFdPlay(
    JNIEnv* env, jclass cls, jint fd, jlong offset, jlong length) {
  char msg[256];
  if (!ensure_engine()) return (*env)->NewStringUTF(env, "no engine");
  SLDataLocator_AndroidFD loc = {SL_DATALOCATOR_ANDROIDFD, fd, offset, length};
  SLDataFormat_MIME fmt = {SL_DATAFORMAT_MIME, NULL, SL_CONTAINERTYPE_UNSPECIFIED};
  SLDataSource src = {&loc, &fmt};
  SLDataLocator_OutputMix out = {SL_DATALOCATOR_OUTPUTMIX, g_mix_obj};
  SLDataSink sink = {&out, NULL};
  const SLInterfaceID ids[] = {SL_IID_SEEK};
  const SLboolean req[] = {SL_BOOLEAN_TRUE};
  SLObjectItf player;
  SLresult r = (*g_engine)->CreateAudioPlayer(g_engine, &player, &src, &sink, 1, ids, req);
  if (r != SL_RESULT_SUCCESS) {
    snprintf(msg, sizeof msg, "CreateAudioPlayer %u", (unsigned)r);
    return (*env)->NewStringUTF(env, msg);
  }
  r = (*player)->Realize(player, SL_BOOLEAN_FALSE);
  SLPlayItf play;
  (*player)->GetInterface(player, SL_IID_PLAY, &play);
  g_at_end = 0;
  (*play)->RegisterCallback(play, on_play, NULL);
  (*play)->SetCallbackEventsMask(play, SL_PLAYEVENT_HEADATEND);
  SLmillisecond duration = 0;
  (*play)->GetDuration(play, &duration);
  (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
  for (int i = 0; i < 300 && !g_at_end; i++) sleep_ms(10);
  snprintf(msg, sizeof msg, "realize %u, duration %s, at end %d", (unsigned)r,
           duration >= 740 && duration <= 860 ? "ok" : "bad", g_at_end);
  (*player)->Destroy(player);
  return (*env)->NewStringUTF(env, msg);
}
