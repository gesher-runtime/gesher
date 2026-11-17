// Keep the ANGLE qualification probe and RenderThread's Ganesh configuration
// identical. A GL function table can validate even when MakeGL returns null.
#pragma once

#include <android-base/properties.h>
#include <include/gpu/ganesh/GrContextOptions.h>

#include "Properties.h"

namespace gesher::hwui {
inline void InitGaneshContextOptions(GrContextOptions& options) {
  options.fPreferExternalImagesOverES3 = true;
  options.fDisableDistanceFieldPaths = true;
  options.fReduceOpsTaskSplitting =
      android::base::GetBoolProperty(PROPERTY_REDUCE_OPS_TASK_SPLITTING, true)
          ? GrContextOptions::Enable::kYes
          : GrContextOptions::Enable::kNo;
}
}  // namespace gesher::hwui
