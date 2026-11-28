#pragma once
#include <cstdint>
using media_status_t = int32_t;
constexpr media_status_t AMEDIA_OK = 0;
constexpr media_status_t AMEDIA_ERROR_UNKNOWN = -10000;
constexpr media_status_t AMEDIA_ERROR_UNSUPPORTED = -10002;
constexpr media_status_t AMEDIA_ERROR_INVALID_OBJECT = -10003;
constexpr media_status_t AMEDIA_ERROR_INVALID_PARAMETER = -10004;
constexpr media_status_t AMEDIA_ERROR_INVALID_OPERATION = -10005;
