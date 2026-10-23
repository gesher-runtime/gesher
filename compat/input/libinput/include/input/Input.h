// Gesher: libinput's input/Input.h includes the AIDL constants it uses only
// under __linux__ (Windows is not a target of libinput's host build); they are
// included here first. docs/input.md.
#pragma once

#include <android/os/IInputConstants.h>
#include <android/os/MotionEventFlag.h>

#include_next <input/Input.h>
