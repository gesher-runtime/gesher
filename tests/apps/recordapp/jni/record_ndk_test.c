// recordapp's NDK audio input (media3, docs/media.md "Recording"): AAudio input (a
// data callback stream and a blocking read) and an OpenSL ES audio recorder into an
// Android simple buffer queue, called from RecordActivity through JNI. Each records a
// second of the microphone (GESHER_AUDIO_SOURCE's file: 750 Hz for 0.4 s, then 1250 Hz
// for 0.4 s) and reports the frequency it finds in each tone's window.
#include <aaudio/AAudio.h>
#include <jni.h>
#include <math.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// Upward zero crossings (with hysteresis) per second in [a, b) seconds of mono floats.
static double hz_in(const float* s, int n, int rate, double a, double b) {
  int from = (int)(a * rate), to = (int)(b * rate);
  if (to > n) to = n;
  if (from >= to) return 0;
  long crossings = 0;
  float prev = 0;
  for (int i = from; i < to; i++) {
    if (prev < -0.03f && s[i] >= 0) crossings++;
    if (s[i] < -0.03f || s[i] >= 0) prev = s[i];
  }
  return crossings / ((double)(to - from) / rate);
}

static const char* tone(double hz, double want, char* buf) {
  if (fabs(hz - want) <= want * 0.02) {
    snprintf(buf, 32, "%d Hz", (int)want);
  } else {
    snprintf(buf, 32, "%.1f Hz", hz);
  }
  return buf;
}

// "750 Hz then 1250 Hz" for a second recorded from the start.
static void describe(const float* s, int n, int rate, char* out, size_t size) {
  char a[32], b[32];
  snprintf(out, size, "%s then %s", tone(hz_in(s, n, rate, 0.05, 0.35), 750, a),
           tone(hz_in(s, n, rate, 0.55, 0.85), 1250, b));
}

// --- AAudio --------------------------------------------------------------------------

struct Capture {
  float* buf;
  int n, want, channels, callbacks;
};

static aaudio_data_callback_result_t on_input(AAudioStream* s, void* user, void* data, int32_t frames) {
  struct Capture* c = (struct Capture*)user;
  const float* in = (const float*)data;
  for (int32_t i = 0; i < frames && c->n < c->want; i++) c->buf[c->n++] = in[i * c->channels];
  c->callbacks++;
  return c->n < c->want ? AAUDIO_CALLBACK_RESULT_CONTINUE : AAUDIO_CALLBACK_RESULT_STOP;
}

JNIEXPORT jstring JNICALL Java_com_gesher_recordapp_RecordActivity_aaudioInputCallback(JNIEnv* env, jclass cls) {
  AAudioStreamBuilder* b;
  AAudioStream* s;
  char msg[256], tones[96];
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return (*env)->NewStringUTF(env, "no builder");
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setSampleRate(b, 48000);
  AAudioStreamBuilder_setChannelCount(b, 1);
  AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
  struct Capture c = {0};
  c.want = 48000;
  c.channels = 1;
  c.buf = (float*)calloc((size_t)c.want, sizeof(float));
  AAudioStreamBuilder_setDataCallback(b, on_input, &c);
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) {
    free(c.buf);
    snprintf(msg, sizeof msg, "open %s", AAudio_convertResultToText(r));
    return (*env)->NewStringUTF(env, msg);
  }
  int32_t dir = AAudioStream_getDirection(s), rate = AAudioStream_getSampleRate(s);
  AAudioStream_requestStart(s);
  for (int i = 0; i < 300 && c.n < c.want; i++) sleep_ms(10);
  AAudioStream_requestStop(s);
  int64_t read = AAudioStream_getFramesRead(s);
  AAudioStream_close(s);
  describe(c.buf, c.n, rate, tones, sizeof tones);
  snprintf(msg, sizeof msg, "direction %s, rate %d, %d frames, callbacks %s, frames read %s, %s",
           dir == AAUDIO_DIRECTION_INPUT ? "input" : "output", rate, c.n,
           c.callbacks >= 50 ? "50+" : "few", read >= c.want ? "all" : "short", tones);
  free(c.buf);
  return (*env)->NewStringUTF(env, msg);
}

JNIEXPORT jstring JNICALL Java_com_gesher_recordapp_RecordActivity_aaudioInputRead(JNIEnv* env, jclass cls) {
  AAudioStreamBuilder* b;
  AAudioStream* s;
  char msg[256], tones[96];
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return (*env)->NewStringUTF(env, "no builder");
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setSampleRate(b, 44100);
  AAudioStreamBuilder_setChannelCount(b, 2);
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) {
    snprintf(msg, sizeof msg, "open %s", AAudio_convertResultToText(r));
    return (*env)->NewStringUTF(env, msg);
  }
  const int want = 44100;
  int16_t* pcm = (int16_t*)calloc((size_t)want * 2, sizeof(int16_t));
  float* mono = (float*)calloc((size_t)want, sizeof(float));
  AAudioStream_requestStart(s);
  int got = 0;
  while (got < want) {
    int32_t n = AAudioStream_read(s, pcm + got * 2, want - got, 2000000000LL);
    if (n <= 0) break;
    got += n;
  }
  AAudioStream_requestStop(s);
  AAudioStream_close(s);
  for (int i = 0; i < got; i++) mono[i] = pcm[i * 2] / 32768.0f;
  describe(mono, got, 44100, tones, sizeof tones);
  snprintf(msg, sizeof msg, "stereo 16-bit 44100, read %d frames, %s", got, tones);
  free(pcm);
  free(mono);
  return (*env)->NewStringUTF(env, msg);
}

// --- OpenSL ES -----------------------------------------------------------------------

#define SL_BUFFER_FRAMES 1600  // 100 ms at 16 kHz

struct Rec {
  int16_t bufs[2][SL_BUFFER_FRAMES];
  float* all;
  int n, want, next, callbacks;
};

static void on_buffer(SLAndroidSimpleBufferQueueItf q, void* ctx) {
  struct Rec* r = (struct Rec*)ctx;
  int16_t* b = r->bufs[r->next];
  for (int i = 0; i < SL_BUFFER_FRAMES && r->n < r->want; i++) r->all[r->n++] = b[i] / 32768.0f;
  r->callbacks++;
  if (r->n < r->want) (*q)->Enqueue(q, b, sizeof(r->bufs[0]));
  r->next ^= 1;
}

JNIEXPORT jstring JNICALL Java_com_gesher_recordapp_RecordActivity_openslRecorder(JNIEnv* env, jclass cls) {
  SLObjectItf engine_obj, rec_obj;
  SLEngineItf engine;
  SLRecordItf record;
  SLAndroidSimpleBufferQueueItf queue;
  char msg[256], tones[96];
  if (slCreateEngine(&engine_obj, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS) {
    return (*env)->NewStringUTF(env, "no engine");
  }
  (*engine_obj)->Realize(engine_obj, SL_BOOLEAN_FALSE);
  (*engine_obj)->GetInterface(engine_obj, SL_IID_ENGINE, &engine);
  SLDataLocator_IODevice dev = {SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT,
                                SL_DEFAULTDEVICEID_AUDIOINPUT, NULL};
  SLDataSource src = {&dev, NULL};
  SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
  SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
  SLDataSink sink = {&loc, &pcm};
  const SLInterfaceID ids[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
  const SLboolean req[1] = {SL_BOOLEAN_TRUE};
  SLresult res = (*engine)->CreateAudioRecorder(engine, &rec_obj, &src, &sink, 1, ids, req);
  if (res != SL_RESULT_SUCCESS) {
    (*engine_obj)->Destroy(engine_obj);
    snprintf(msg, sizeof msg, "CreateAudioRecorder %u", (unsigned)res);
    return (*env)->NewStringUTF(env, msg);
  }
  (*rec_obj)->Realize(rec_obj, SL_BOOLEAN_FALSE);
  (*rec_obj)->GetInterface(rec_obj, SL_IID_RECORD, &record);
  (*rec_obj)->GetInterface(rec_obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue);
  struct Rec* r = (struct Rec*)calloc(1, sizeof(struct Rec));
  r->want = 16000;
  r->all = (float*)calloc((size_t)r->want, sizeof(float));
  (*queue)->RegisterCallback(queue, on_buffer, r);
  (*queue)->Enqueue(queue, r->bufs[0], sizeof(r->bufs[0]));
  (*queue)->Enqueue(queue, r->bufs[1], sizeof(r->bufs[1]));
  (*record)->SetRecordState(record, SL_RECORDSTATE_RECORDING);
  for (int i = 0; i < 300 && r->n < r->want; i++) sleep_ms(10);
  SLuint32 state = 0;
  (*record)->GetRecordState(record, &state);
  SLmillisecond pos = 0;
  (*record)->GetPosition(record, &pos);
  (*record)->SetRecordState(record, SL_RECORDSTATE_STOPPED);
  (*rec_obj)->Destroy(rec_obj);
  (*engine_obj)->Destroy(engine_obj);
  describe(r->all, r->n, 16000, tones, sizeof tones);
  snprintf(msg, sizeof msg, "state %s, %d frames, %d buffers, position %s, %s",
           state == SL_RECORDSTATE_RECORDING ? "recording" : "other", r->n, r->callbacks,
           pos >= 900 ? "1 s" : "short", tones);
  free(r->all);
  free(r);
  return (*env)->NewStringUTF(env, msg);
}
