// Compile/link the production display query with the NDK graphics target's flags.
// Windows is deliberately included first, as egl_child_surface.h includes it.
#include <windows.h>
#include "compat/bionic/display_refresh.h"

#ifndef NOGDI
#error "The display-refresh public header must preserve the caller's NOGDI"
#endif
#ifdef ENUM_CURRENT_SETTINGS
#error "The display-refresh public header must not expose the excluded GDI API"
#endif

int main() {
  gesher::bionic::gl::DisplayRefresh refresh;
  return refresh.Period(nullptr).count() <= 0;
}
