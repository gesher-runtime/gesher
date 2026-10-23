package com.gesher.app.input;

import android.hardware.input.IInputManager;
import android.view.InputDevice;
import android.view.KeyCharacterMap;

/**
 * Android's virtual keyboard on Gesher (docs/input.md): device id
 * {@link KeyCharacterMap#VIRTUAL_KEYBOARD}, the device every key event from a
 * Gesher window comes from, with the key character map Generic.kcm
 * ($ANDROID_ROOT/usr/keychars/Generic.kcm, or the copy built into the
 * runtime). {@link #inputManager()} serves it to {@code InputManagerGlobal},
 * which {@link KeyCharacterMap#load} and {@code KeyEvent.getUnicodeChar()}
 * ask for the device's map. Natives: compat/input/input_jni.cc.
 */
public final class VirtualKeyboard {
  private VirtualKeyboard() {}

  /** Generic.kcm, as the map of device {@code deviceId}. */
  public static KeyCharacterMap loadKeyCharacterMap(int deviceId) {
    return nLoad(deviceId);
  }

  /** The virtual keyboard: a full keyboard with Generic.kcm. */
  public static InputDevice device() {
    return new InputDevice.Builder()
        .setId(KeyCharacterMap.VIRTUAL_KEYBOARD)
        .setName("Virtual")
        .setDescriptor("gesher-virtual-keyboard")
        .setIsVirtualDevice(true)
        .setSources(InputDevice.SOURCE_KEYBOARD)
        .setKeyboardType(InputDevice.KEYBOARD_TYPE_ALPHABETIC)
        .setKeyCharacterMap(loadKeyCharacterMap(KeyCharacterMap.VIRTUAL_KEYBOARD))
        .build();
  }

  /**
   * An input manager service that knows the virtual keyboard and nothing
   * else: for {@code InputManagerGlobal.createTestSession}, or to register as
   * the "input" service.
   */
  public static IInputManager inputManager() {
    return new InputManager();
  }

  /** The input service's answers about devices; the rest are IInputManager.Default's. */
  public static class InputManager extends IInputManager.Default {
    private InputDevice mKeyboard;

    @Override
    public int[] getInputDeviceIds() {
      return new int[] {KeyCharacterMap.VIRTUAL_KEYBOARD};
    }

    @Override
    public synchronized InputDevice getInputDevice(int id) {
      if (id != KeyCharacterMap.VIRTUAL_KEYBOARD) {
        return null;
      }
      if (mKeyboard == null) {
        mKeyboard = device();
      }
      return mKeyboard;
    }
  }

  private static native KeyCharacterMap nLoad(int deviceId);
}
