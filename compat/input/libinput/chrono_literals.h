// Gesher: std::chrono literals (5ms) that libinput's sources use without a
// using-directive of their own; on Android a header they include brings it in.
// Force-included into libinput and the input natives (cmake/aosp/input.cmake).
#pragma once

#include <chrono>

using namespace std::chrono_literals;
