// The WindowEvent modifier ABI. The original generic bits remain stable.
#pragma once

#include <stdint.h>

namespace gesher {

enum WindowModifier : int32_t {
  kModifierShift = 1, kModifierCtrl = 2, kModifierAlt = 4, kModifierCapsLock = 8,
  kModifierMeta = 16, kModifierNumLock = 32, kModifierScrollLock = 64,
  kModifierShiftLeft = 1 << 8, kModifierShiftRight = 1 << 9,
  kModifierCtrlLeft = 1 << 10, kModifierCtrlRight = 1 << 11,
  kModifierAltLeft = 1 << 12, kModifierAltRight = 1 << 13,
  kModifierMetaLeft = 1 << 14, kModifierMetaRight = 1 << 15,
};

inline int32_t NormalizeModifiers(int32_t m) {
  if (m & (kModifierShiftLeft | kModifierShiftRight)) m |= kModifierShift;
  if (m & (kModifierCtrlLeft | kModifierCtrlRight)) m |= kModifierCtrl;
  if (m & (kModifierAltLeft | kModifierAltRight)) m |= kModifierAlt;
  if (m & (kModifierMetaLeft | kModifierMetaRight)) m |= kModifierMeta;
  return m;
}

// WM_KEYDOWN/UP use generic VK_SHIFT/CONTROL/MENU; the scan and extended
// bits distinguish their physical sides. Explicit sided keys are also valid.
inline int32_t ModifierForVirtualKey(int32_t vk, int32_t scan) {
  switch (vk) {
    case 0x10: return (scan & 0xff) == 0x36 ? kModifierShiftRight : kModifierShiftLeft;
    case 0x11: return (scan & 0x100) ? kModifierCtrlRight : kModifierCtrlLeft;
    case 0x12: return (scan & 0x100) ? kModifierAltRight : kModifierAltLeft;
    case 0xa0: return kModifierShiftLeft;
    case 0xa1: return kModifierShiftRight;
    case 0xa2: return kModifierCtrlLeft;
    case 0xa3: return kModifierCtrlRight;
    case 0xa4: return kModifierAltLeft;
    case 0xa5: return kModifierAltRight;
    case 0x5b: return kModifierMetaLeft;
    case 0x5c: return kModifierMetaRight;
    default: return 0;
  }
}

// Shared by the Win32 event producer and the IME. GetKeyState observes the
// state for the current message, unlike GetAsyncKeyState's hardware state.
template <class GetState>
int32_t ReadModifiers(GetState get) {
  int32_t m = 0;
  const int keys[] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0x5b, 0x5c};
  for (int vk : keys) {
    if (get(vk) & 0x8000) m |= ModifierForVirtualKey(vk, 0);
  }
  if (get(0x14) & 1) m |= kModifierCapsLock;
  if (get(0x90) & 1) m |= kModifierNumLock;
  if (get(0x91) & 1) m |= kModifierScrollLock;
  return NormalizeModifiers(m);
}

inline bool HasShortcutModifier(int32_t m) {
  return (NormalizeModifiers(m) &
          (kModifierShift | kModifierCtrl | kModifierAlt | kModifierMeta)) != 0;
}

}  // namespace gesher
