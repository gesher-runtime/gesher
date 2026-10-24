// Gesher: the evdev constants libinput's InputEventLabels.cpp names. Windows
// has no evdev; Gesher's input does not come from Linux devices, so only the
// event types (and the one axis it names) are defined, with Linux's values
// (uapi/linux/input-event-codes.h). docs/input.md.
#pragma once

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_SW 0x05
#define EV_LED 0x11
#define EV_SND 0x12
#define EV_REP 0x14
#define EV_FF 0x15
#define EV_PWR 0x16
#define EV_FF_STATUS 0x17

#define ABS_MT_TOOL_TYPE 0x37
