// Owned Windows profile-directory notifications, without the generic fd poll path.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gesher::dexopt {
uint64_t OpenProfileWatch(const std::u16string& path, uint32_t* error);
// Empty output means explicit refresh/overflow. Errors terminate this watch.
bool ReadProfileWatch(uint64_t token, std::vector<std::u16string>* names, uint32_t* error);
void WakeProfileWatch(uint64_t token);
void CloseProfileWatch(uint64_t token);
}
