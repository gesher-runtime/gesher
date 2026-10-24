// Gesher: what hwui's EglManager uses of libgraphicsenv's GpuStatsInfo.h
// (frameworks/native/libs/graphicsenv, not built: GPU statistics go to
// GpuService on a device). The value is the device's.
#pragma once

namespace android {

class GpuStatsInfo {
public:
    enum GLTelemetryHints {
        NO_HINT = 0,
        SKIP_TELEMETRY = 1,
    };
};

}  // namespace android
