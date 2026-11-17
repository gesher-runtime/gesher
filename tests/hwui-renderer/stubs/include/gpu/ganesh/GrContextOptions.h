// Test-only boundary for the options used by Gesher's Ganesh qualification.
// Types and defaults match external/skia at android-16.0.0_r4. This header is
// visible only to the portable host fixtures, never to production builds.
#pragma once

struct GrContextOptions {
  enum class Enable { kNo, kYes, kDefault };

  Enable fReduceOpsTaskSplitting = Enable::kDefault;
  bool fDisableDistanceFieldPaths = false;
  bool fPreferExternalImagesOverES3 = false;
};
