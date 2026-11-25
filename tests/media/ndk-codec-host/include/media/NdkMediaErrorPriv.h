#pragma once
#include "NdkMediaError.h"
inline media_status_t translate_error(int) { return AMEDIA_ERROR_UNKNOWN; }
