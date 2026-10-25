// Gesher media: a whole file decoded to PCM (decoder.h): SoundPool's samples,
// the media probe's checks.

#include <algorithm>

#include "../audio/log.h"
#include "decoder.h"
#include "extractor.h"

namespace gesher::media {

bool DecodeFile(std::shared_ptr<DataSource> source, size_t max_bytes, std::vector<uint8_t>* pcm,
                PcmInfo* info, std::string* error) {
  auto ex = Extractor::Open(std::move(source), error);
  if (ex == nullptr) return false;
  for (size_t i = 0; i < ex->TrackCount(); i++) {
    Format f = ex->TrackFormat(i);
    if (f.Mime().rfind("audio/", 0) != 0) continue;
    auto dec = AudioDecoder::Create(f, error);
    if (dec == nullptr) continue;
    auto reader = ex->OpenTrack(i);
    if (reader == nullptr) continue;
    pcm->clear();
    Sample s;
    int status;
    while ((status = reader->Read(&s)) == 0) {
      if (!dec->Decode(s, pcm)) {
        *error = "decoding failed";
        return false;
      }
      if (pcm->size() > max_bytes) {
        MEDIA_LOGW("decode: more than %zu bytes of PCM; the rest dropped", max_bytes);
        break;
      }
    }
    if (status < 0) {
      *error = "reading failed";
      return false;
    }
    dec->Drain(pcm);
    *info = dec->info();
    size_t frame = BytesPerSample(info->format) * std::max(info->channels, 1u);
    pcm->resize(std::min(pcm->size(), max_bytes) / frame * frame);
    return true;
  }
  if (error->empty()) *error = "no audio track";
  return false;
}

}  // namespace gesher::media
