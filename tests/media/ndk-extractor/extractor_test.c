// The public NDK calls load the actual bridge in gesher-exec.
#include <media/NdkMediaExtractor.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
  if (!(condition)) { fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

enum { kFileOffset = 37, kWaveHeader = 44, kAudioSize = 16000 };
struct CallbackSource {
  int fd;
  int64_t bytes;
  atomic_int reads, closes;
  atomic_bool block, reading, closed;
};

static ssize_t ReadAt(void* opaque, off64_t pos, void* out, size_t size) {
  struct CallbackSource* state = opaque;
  atomic_fetch_add(&state->reads, 1);
  if (atomic_load(&state->block)) {
    atomic_store(&state->reading, true);
    while (!atomic_load(&state->closed)) usleep(1000);
    return -1;
  }
  if (pos < 0 || pos >= state->bytes) return -1;
  if (size > (uint64_t)(state->bytes - pos)) size = state->bytes - pos;
  ssize_t read = pread64(state->fd, out, size, kFileOffset + pos);
  return read == 0 ? -1 : read;
}

static ssize_t GetSize(void* opaque) { return ((struct CallbackSource*)opaque)->bytes; }

static void Close(void* opaque) {
  struct CallbackSource* state = opaque;
  atomic_fetch_add(&state->closes, 1);
  atomic_store(&state->closed, true);
}

static AMediaDataSource* CallbackDescriptor(struct CallbackSource* state) {
  AMediaDataSource* source = AMediaDataSource_new();
  if (source != NULL) {
    AMediaDataSource_setUserdata(source, state);
    AMediaDataSource_setReadAt(source, ReadAt);
    AMediaDataSource_setGetSize(source, GetSize);
    AMediaDataSource_setClose(source, Close);
  }
  return source;
}

struct OpenTask { AMediaExtractor* ex; AMediaDataSource* source; media_status_t result; };
static void* OpenCustom(void* opaque) {
  struct OpenTask* task = opaque;
  task->result = AMediaExtractor_setDataSourceCustom(task->ex, task->source);
  return NULL;
}

static int CheckWave(AMediaExtractor* ex) {
  CHECK(AMediaExtractor_getTrackCount(ex) == 1);
  AMediaFormat* format = AMediaExtractor_getTrackFormat(ex, 0);
  CHECK(format != NULL);
  const char* mime = NULL;
  int32_t rate = 0;
  CHECK(AMediaFormat_getString(format, "mime", &mime) && strcmp(mime, "audio/raw") == 0);
  CHECK(AMediaFormat_getInt32(format, "sample-rate", &rate) && rate == 8000);
  CHECK(AMediaFormat_delete(format) == AMEDIA_OK);
  CHECK(AMediaExtractor_getSampleTrackIndex(ex) == -1);
  CHECK(AMediaExtractor_selectTrack(ex, 1) != AMEDIA_OK);
  CHECK(AMediaExtractor_selectTrack(ex, 0) == AMEDIA_OK);
  CHECK(AMediaExtractor_selectTrack(ex, 0) == AMEDIA_OK);
  CHECK(AMediaExtractor_getSampleTime(ex) == 0);
  CHECK(AMediaExtractor_getSampleTrackIndex(ex) == 0);
  CHECK(AMediaExtractor_getSampleFlags(ex) == AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC);
  CHECK(AMediaExtractor_getSampleCryptoInfo(ex) == NULL);
  CHECK(AMediaExtractor_getPsshInfo(ex) == NULL);
  CHECK(AMediaExtractor_getCachedDuration(ex) == -1);
  format = AMediaFormat_new();
  AMediaFormat_setString(format, "stale", "old");
  CHECK(AMediaExtractor_getSampleFormat(ex, format) == AMEDIA_OK);
  CHECK(AMediaFormat_getInt32(format, "track-index", &rate) && rate == 0);
  CHECK(!AMediaFormat_getString(format, "stale", &mime));
  AMediaFormat_delete(format);

  const ssize_t sample = AMediaExtractor_getSampleSize(ex);
  uint8_t data[kAudioSize];
  data[0] = 89;
  CHECK(sample > 1 && (size_t)sample <= sizeof(data));
  CHECK(AMediaExtractor_readSampleData(ex, data, sample - 1) == -1 && data[0] == 89);
  CHECK(AMediaExtractor_readSampleData(ex, data, sizeof(data)) == sample && data[0] == 0);
  CHECK(AMediaExtractor_getSampleTime(ex) == 0);
  CHECK(AMediaExtractor_seekTo(ex, 500000, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC) == AMEDIA_OK);
  CHECK(AMediaExtractor_getSampleTime(ex) == 500000);
  CHECK(AMediaExtractor_readSampleData(ex, data, sizeof(data)) > 0 && data[0] == (4000 % 251));
  CHECK(AMediaExtractor_unselectTrack(ex, 0) == AMEDIA_OK);
  CHECK(AMediaExtractor_getSampleTime(ex) == -1);
  CHECK(AMediaExtractor_selectTrack(ex, 0) == AMEDIA_OK);

  size_t bytes = 0;
  do {
    ssize_t read = AMediaExtractor_readSampleData(ex, data, sizeof(data));
    CHECK(read > 0);
    bytes += read;
  } while (AMediaExtractor_advance(ex));
  CHECK(bytes == kAudioSize);
  CHECK(AMediaExtractor_getSampleSize(ex) == -1);
  CHECK(AMediaExtractor_getSampleFlags(ex) == UINT32_MAX);
  CHECK(AMediaExtractor_seekTo(ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC) == AMEDIA_OK);
  CHECK(AMediaExtractor_getSampleTime(ex) == 0);
  return 0;
}

static int CheckMp4(const char* path) {
  AMediaExtractor* ex = AMediaExtractor_new();
  CHECK(ex != NULL && AMediaExtractor_setDataSource(ex, path) == AMEDIA_OK);
  CHECK(AMediaExtractor_getTrackCount(ex) == 2);
  PsshInfo* pssh = AMediaExtractor_getPsshInfo(ex);
  static const uint8_t uuid[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  CHECK(pssh != NULL && pssh->numentries == 1 && pssh->entries[0].datalen == 3);
  CHECK(memcmp(pssh->entries[0].uuid, uuid, sizeof(uuid)) == 0);
  CHECK(memcmp(pssh->entries[0].data, "xyz", 3) == 0);
  CHECK(AMediaExtractor_getPsshInfo(ex) == pssh);

  size_t video = 2;
  for (size_t i = 0; i < 2; ++i) {
    AMediaFormat* fmt = AMediaExtractor_getTrackFormat(ex, i);
    const char* mime = NULL;
    CHECK(fmt != NULL && AMediaFormat_getString(fmt, "mime", &mime));
    if (strcmp(mime, "video/avc") == 0) {
      video = i;
      void* data = NULL;
      size_t length = 0;
      CHECK(AMediaFormat_getBuffer(fmt, "csd-0", &data, &length) &&
            length >= 5 && memcmp(data, "\x00\x00\x00\x01", 4) == 0);
      CHECK(AMediaFormat_getBuffer(fmt, "csd-1", &data, &length) && length >= 5);
    }
    AMediaFormat_delete(fmt);
    CHECK(AMediaExtractor_selectTrack(ex, i) == AMEDIA_OK);
  }
  CHECK(video < 2);
  unsigned samples[2] = {0};
  int64_t previous = INT64_MIN;
  do {
    int track = AMediaExtractor_getSampleTrackIndex(ex);
    int64_t time = AMediaExtractor_getSampleTime(ex);
    CHECK(track >= 0 && track < 2 && time >= previous);
    CHECK(AMediaExtractor_getSampleSize(ex) > 0);
    ++samples[track];
    previous = time;
  } while (AMediaExtractor_advance(ex));
  CHECK(samples[0] > 1 && samples[1] > 1);
  CHECK(AMediaExtractor_unselectTrack(ex, 1 - video) == AMEDIA_OK);
  CHECK(AMediaExtractor_seekTo(ex, 400000, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC) == AMEDIA_OK);
  CHECK(AMediaExtractor_getSampleTrackIndex(ex) == (int)video);
  CHECK(AMediaExtractor_getSampleTime(ex) == 0);
  CHECK(AMediaExtractor_seekTo(ex, 400000, AMEDIAEXTRACTOR_SEEK_NEXT_SYNC) == AMEDIA_OK);
  CHECK(AMediaExtractor_getSampleTime(ex) == 500000);
  CHECK(AMediaExtractor_getSampleFlags(ex) & AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC);
  CHECK(AMediaExtractor_getPsshInfo(ex) == pssh);
  AMediaExtractor_delete(ex);
  return 0;
}

int main(int argc, char** argv) {
  CHECK(argc == 4);
  void* handle = dlopen("libmediandk.so", RTLD_NOW);
  CHECK(handle != NULL && dlsym(handle, "AMediaExtractor_new") != NULL &&
        dlsym(handle, "AMediaDataSource_setReadAt") != NULL);

  AMediaExtractor* ex = AMediaExtractor_new();
  int fd = open(argv[1], O_RDONLY);
  CHECK(ex != NULL && fd >= 0);
  CHECK(AMediaExtractor_setDataSourceFd(ex, fd, 0, kWaveHeader) != AMEDIA_OK);
  CHECK(AMediaExtractor_setDataSourceFd(ex, fd, kFileOffset, kWaveHeader + kAudioSize) == AMEDIA_OK);
  CHECK(lseek(fd, 0, SEEK_CUR) == 0);
  CHECK(close(fd) == 0);
  CHECK(AMediaExtractor_setDataSource(ex, argv[2]) != AMEDIA_OK);
  CHECK(CheckWave(ex) == 0);
  CHECK(AMediaExtractor_delete(ex) == AMEDIA_OK);

  ex = AMediaExtractor_new();
  CHECK(AMediaExtractor_setDataSource(ex, argv[2]) == AMEDIA_OK);
  CHECK(CheckWave(ex) == 0);
  AMediaExtractor_delete(ex);

  AMediaDataSource* source = AMediaDataSource_newUri(argv[2], 0, NULL);
  CHECK(source != NULL);
  ex = AMediaExtractor_new();
  CHECK(AMediaExtractor_setDataSourceCustom(ex, source) == AMEDIA_OK);
  AMediaDataSource_delete(source);
  CHECK(CheckWave(ex) == 0);
  AMediaExtractor_delete(ex);

  struct CallbackSource state = {
    .fd = open(argv[1], O_RDONLY), .bytes = kWaveHeader + kAudioSize
  };
  CHECK(state.fd >= 0);
  source = CallbackDescriptor(&state);
  ex = AMediaExtractor_new();
  CHECK(source != NULL);
  CHECK(AMediaExtractor_setDataSourceCustom(ex, source) == AMEDIA_OK);
  AMediaDataSource_delete(source);
  CHECK(CheckWave(ex) == 0);
  CHECK(atomic_load(&state.reads) > 1 && atomic_load(&state.closes) == 0);
  AMediaExtractor_delete(ex);
  CHECK(atomic_load(&state.closes) == 1);

  atomic_store(&state.closed, false);
  atomic_store(&state.closes, 0);
  atomic_store(&state.block, true);
  source = CallbackDescriptor(&state);
  ex = AMediaExtractor_new();
  struct OpenTask task = { .ex = ex, .source = source, .result = AMEDIA_OK };
  pthread_t thread;
  CHECK(source != NULL && pthread_create(&thread, NULL, OpenCustom, &task) == 0);
  for (int i = 0; i < 5000 && !atomic_load(&state.reading); i++) usleep(1000);
  CHECK(atomic_load(&state.reading));
  AMediaDataSource_close(source);
  CHECK(pthread_join(thread, NULL) == 0 && task.result != AMEDIA_OK);
  AMediaDataSource_close(source);
  AMediaExtractor_delete(ex);
  AMediaDataSource_delete(source);
  CHECK(atomic_load(&state.closes) == 1);
  close(state.fd);
  CHECK(CheckMp4(argv[3]) == 0);
  dlclose(handle);
  puts("NDK extractor: FD range, custom source, cancellation, seeks, PCM, AVC CSD and PSSH passed");
  return 0;
}
