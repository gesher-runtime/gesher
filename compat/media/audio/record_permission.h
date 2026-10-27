// The native media input gate; its Java host supplies the manifest decision.
#pragma once

namespace gesher::media {

using RecordingPermissionProvider = bool (*)();
void SetRecordingPermissionProvider(RecordingPermissionProvider provider);
// A process without a recording-identity provider has no microphone grant.
bool RecordingAllowed();

}  // namespace gesher::media
